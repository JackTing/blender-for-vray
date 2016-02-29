/*
 * Copyright (c) 2015, Chaos Software Ltd
 *
 * V-Ray For Blender
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "vfb_plugin_exporter_zmq.h"
#include "vfb_export_settings.h"
#include "jpeglib.h"
#include <setjmp.h>
#include <limits>

using namespace VRayForBlender;

ZmqWorkerPool::ZmqWorkerPool()
{
}

ZmqWorkerPool & ZmqWorkerPool::getInstance()
{
	static ZmqWorkerPool pool;
	return pool;
}

ClientPtr ZmqWorkerPool::getClient()
{
	if (m_Clients.empty()) {
		m_Clients.push(ClientPtr(new ZmqClient()));
	}

	auto cl = std::move(m_Clients.top());
	m_Clients.pop();
	return cl;
}

void ZmqWorkerPool::returnClient(ClientPtr cl)
{
	cl->send(VRayMessage::createMessage(VRayMessage::RendererAction::Free));
	m_Clients.push(std::move(cl));
}

ZmqWorkerPool::~ZmqWorkerPool()
{
	while (!m_Clients.empty()) {
		m_Clients.top()->setFlushOnExit(false);
		m_Clients.top()->forceFree();
		m_Clients.pop();
	}
}


struct JpegErrorManager {
	jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
};

static void jpegErrorExit(j_common_ptr cinfo) {
	JpegErrorManager * myerr = (JpegErrorManager*)cinfo->err;
	(*cinfo->err->output_message) (cinfo);
	longjmp(myerr->setjmp_buffer, 1);
}


static void init_source(j_decompress_ptr cinfo) {}

static boolean fill_input_buffer (j_decompress_ptr cinfo) {
	unsigned char *buf = (unsigned char *) cinfo->src->next_input_byte - 2;

	buf[0] = (JOCTET) 0xFF;
	buf[1] = (JOCTET) JPEG_EOI;

	cinfo->src->next_input_byte = buf;
	cinfo->src->bytes_in_buffer = 2;

	return TRUE;
}

static void skip_input_data (j_decompress_ptr cinfo, long num_bytes) {
    struct jpeg_source_mgr* src = (struct jpeg_source_mgr*) cinfo->src;

    if (num_bytes > 0) {
        src->next_input_byte += (size_t) num_bytes;
        src->bytes_in_buffer -= (size_t) num_bytes;
    }
}

static void term_source(j_decompress_ptr cinfo) {}

static void jpeg_mem_src_own(j_decompress_ptr cinfo, const unsigned char * buffer, int nbytes) {
    struct jpeg_source_mgr* src;

    if (cinfo->src == NULL) {   /* first time for this JPEG object? */
        cinfo->src = (struct jpeg_source_mgr *)
            (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
            sizeof(struct jpeg_source_mgr));
    }

    src = (struct jpeg_source_mgr*) cinfo->src;
    src->init_source = init_source;
    src->fill_input_buffer = fill_input_buffer;
    src->skip_input_data = skip_input_data;
    src->resync_to_restart = jpeg_resync_to_restart; /* use default method */
    src->term_source = term_source;
    src->bytes_in_buffer = nbytes;
    src->next_input_byte = (JOCTET*)buffer;
}

static float * jpegToPixelData(unsigned char * data, int size, int &channels) {
	jpeg_decompress_struct jpegInfo;
	JpegErrorManager jpegError;

	jpegInfo.err = jpeg_std_error(&jpegError.pub);

	jpegError.pub.error_exit = jpegErrorExit;

	if (setjmp(jpegError.setjmp_buffer)) {
		jpeg_destroy_decompress(&jpegInfo);
		return nullptr;
	}

	jpeg_create_decompress(&jpegInfo);
	jpeg_mem_src_own(&jpegInfo, data, size);

	if (jpeg_read_header(&jpegInfo, TRUE) != JPEG_HEADER_OK) {
		return nullptr;
	}

	jpegInfo.out_color_space = JCS_EXT_RGBA;

	if (!jpeg_start_decompress(&jpegInfo)) {
		return nullptr;
	}

	channels = jpegInfo.output_components;
	int rowStride = jpegInfo.output_width * jpegInfo.output_components;
	float * imageData = new float[jpegInfo.output_height * rowStride];
	JSAMPARRAY buffer = (*jpegInfo.mem->alloc_sarray)((j_common_ptr)&jpegInfo, JPOOL_IMAGE, rowStride, 1);

	int c = 0;
	while (jpegInfo.output_scanline < jpegInfo.output_height) {
		jpeg_read_scanlines(&jpegInfo, buffer, 1);

		float * dest = imageData + c * rowStride;
		unsigned char * source = buffer[0];

		for (int r = 0; r < jpegInfo.image_width * jpegInfo.output_components; ++r) {
			dest[r] = source[r] / 255.f;
		}

		++c;
	}

	jpeg_finish_decompress(&jpegInfo);
	jpeg_destroy_decompress(&jpegInfo);

	return imageData;
}

void ZmqExporter::ZmqRenderImage::update(const VRayBaseTypes::AttrImage &img, ZmqExporter * exp, bool fixImage) {
	// convertions here should match the blender's render pass channel requirements

	if (img.imageType == VRayBaseTypes::AttrImage::ImageType::RGBA_REAL && img.isBucket()) {
		// merge in the bucket

		std::unique_lock<std::mutex> lock(exp->m_ImgMutex);

		if (!this->pixels) {
			PRINT_WARN("Result image not allocated, can't merge bucket!");
		} else if (this->channels != 4) {
			PRINT_WARN("Result image missmatch of channel count - %d, instead of %d!", channels, 4);
		} else {
			lock.unlock();
			const float * sourceImage = reinterpret_cast<const float *>(img.data.get());

			updateRegion(reinterpret_cast<const float *>(img.data.get()), img.x, img.y, img.width, img.height);
			fixImage = false;
		}

	} else if (img.imageType == VRayBaseTypes::AttrImage::ImageType::JPG) {
		int channels = 0;
		float * imgData = jpegToPixelData(reinterpret_cast<unsigned char*>(img.data.get()), img.size, channels);

		{
			std::lock_guard<std::mutex> lock(exp->m_ImgMutex);

			this->channels = channels;
			this->w = img.width;
			this->h = img.height;
			delete[] pixels;
			this->pixels = imgData;
		}
	} else if (img.imageType == VRayBaseTypes::AttrImage::ImageType::RGBA_REAL ||
		       img.imageType == VRayBaseTypes::AttrImage::ImageType::RGB_REAL ||
		       img.imageType == VRayBaseTypes::AttrImage::ImageType::BW_REAL) {

		const float * imgData = reinterpret_cast<const float *>(img.data.get());
		float * myImage = nullptr;
		int channels = 0;

		switch (img.imageType) {
		case VRayBaseTypes::AttrImage::ImageType::RGBA_REAL:
			channels = 4;
			myImage = new float[img.width * img.height * channels];
			memcpy(myImage, imgData, img.width * img.height * channels * sizeof(float));

			break;
		case VRayBaseTypes::AttrImage::ImageType::RGB_REAL:
			channels = 3;
			myImage = new float[img.width * img.height * channels];

			for (int c = 0; c < img.width * img.height; ++c) {
				const float * source = imgData + (c * 4);
				float * dest = myImage + (c * channels);

				dest[0] = source[0];
				dest[1] = source[1];
				dest[2] = source[2];
			}

			break;
		case VRayBaseTypes::AttrImage::ImageType::BW_REAL:
			channels = 1;
			myImage = new float[img.width * img.height * channels];

			for (int c = 0; c < img.width * img.height; ++c) {
				const float * source = imgData + (c * 4);
				float * dest = myImage + (c * channels);

				dest[0] = source[0];
			}

			break;
		default:
			PRINT_WARN("MISSING IMAGE FORMAT CONVERTION FOR %d", img.imageType);
		}

		{
			std::lock_guard<std::mutex> lock(exp->m_ImgMutex);
			this->channels = channels;
			this->w = img.width;
			this->h = img.height;
			delete[] pixels;
			this->pixels = myImage;
		}
	}

	if (fixImage) {
		flip();
		resetAlpha();
		clamp(1.0f, 1.0f);
	}
}


ZmqExporter::ZmqExporter():
	m_Client(nullptr),
	m_LastExportedFrame(std::numeric_limits<float>::min()),
	m_IsAborted(false),
	m_Started(false),
	m_RenderWidth(0),
	m_RenderHeight(0),
	m_RenderQuality(100)
{
	checkZmqClient();
}


ZmqExporter::~ZmqExporter()
{
	free();

	std::lock_guard<std::mutex> lock(m_ZmqClientMutex);

	m_Client->setCallback(ZmqWrapper::ZmqWrapperCallback_t());
	ZmqWorkerPool::getInstance().returnClient(std::move(m_Client));
}

RenderImage ZmqExporter::get_render_channel(RenderChannelType channelType) {
	RenderImage img;

	auto imgIter = m_LayerImages.find(channelType);
	if (imgIter != m_LayerImages.end()) {
		std::unique_lock<std::mutex> lock(m_ImgMutex);
		imgIter = m_LayerImages.find(channelType);

		if (imgIter != m_LayerImages.end()) {
			RenderImage &storedImage = imgIter->second;
			if (storedImage.pixels) {
				img = std::move(RenderImage::deepCopy(storedImage));
			}
		}
	}
	return img;
}

RenderImage ZmqExporter::get_image() {
	return get_render_channel(RenderChannelType::RenderChannelTypeNone);
}

void ZmqExporter::zmqCallback(VRayMessage & message, ZmqWrapper *) {
	std::lock_guard<std::mutex> lock(m_ZmqClientMutex);

	const auto msgType = message.getType();
	if (msgType == VRayMessage::Type::SingleValue && message.getValueType() == VRayBaseTypes::ValueType::ValueTypeString) {
		if (this->on_message_update) {
			auto msg = message.getValue<VRayBaseTypes::AttrSimpleType<std::string>>()->m_Value;
			auto newLine = msg.find_first_of("\n\r");
			if (newLine != std::string::npos) {
				msg.resize(newLine);
			}

			this->on_message_update("", msg.c_str());
		}
	} else if (msgType == VRayMessage::Type::Image) {
		auto set = message.getValue<VRayBaseTypes::AttrImageSet>();
		bool ready = set->sourceType == VRayBaseTypes::ImageSourceType::ImageReady;
		for (const auto &img : set->images) {
			m_LayerImages[img.first].update(img.second, this, !is_viewport);
		}

		if (this->callback_on_rt_image_updated) {
			callback_on_rt_image_updated.cb();
		}

		if (ready && this->callback_on_image_ready) {
			this->callback_on_image_ready.cb();
		}

	} else if (msgType == VRayMessage::Type::ChangeRenderer) {
		if (message.getRendererAction() == VRayMessage::RendererAction::SetRendererStatus) {
			if (!(m_IsAborted = (message.getRendererStatus() == VRayMessage::RendererStatus::Abort))) {
				this->last_rendered_frame = message.getValue<VRayBaseTypes::AttrSimpleType<float>>()->m_Value;
			}
		}
	}
}

void ZmqExporter::init()
{
	try {
		using std::placeholders::_1;
		using std::placeholders::_2;

		m_Client->setCallback(std::bind(&ZmqExporter::zmqCallback, this, _1, _2));

		if (!m_Client->connected()) {
			char portStr[32];
			snprintf(portStr, 32, ":%d", this->m_ServerPort);
			m_Client->connect(("tcp://" + this->m_ServerAddress + portStr).c_str());
		}

		if (m_Client->connected()) {
			auto mode = this->animation_settings.use && !this->is_viewport ? VRayMessage::RendererType::Animation : VRayMessage::RendererType::RT;
			if (mode == VRayMessage::RendererType::Animation || !this->is_viewport) {
				m_RenderMode = RenderMode::RenderModeProduction;
			}
			m_Client->send(VRayMessage::createMessage(mode));
			m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::Init));
			m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::SetRenderMode, static_cast<int>(m_RenderMode)));
			m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::SetQuality, m_RenderQuality));

			m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeNone)));
			if (!is_viewport) {
				m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeVfbZdepth)));
				m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeVfbRealcolor)));
				m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeVfbNormal)));
				m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::GetImage, static_cast<int>(RenderChannelType::RenderChannelTypeVfbRenderID)));
			}
		}
	} catch (zmq::error_t &e) {
		PRINT_ERROR("Failed to initialize ZMQ client\n%s", e.what());
	}
}

void ZmqExporter::checkZmqClient()
{
	std::lock_guard<std::mutex> lock(m_ZmqClientMutex);

	if (!m_Client) {
		m_Client = ZmqWorkerPool::getInstance().getClient();
	} else {
		if (!m_Client->connected()) {
			// we can't connect dont retry
			return;
		}

		if (!m_Client->good()) {
			m_Client.release();
			m_Client = ZmqWorkerPool::getInstance().getClient();
			this->init();
		}
	}
}

void ZmqExporter::set_settings(const ExporterSettings & settings)
{
	PluginExporter::set_settings(settings);

	if (this->is_viewport) {
		this->m_RenderMode = settings.getViewportRenderMode();
	} else {
		this->m_RenderMode = settings.getRenderMode();
	}
	this->m_RenderQuality = settings.viewportQuality;
	this->m_ServerPort = settings.zmq_server_port;
	this->m_ServerAddress = settings.zmq_server_address;
	this->animation_settings = settings.settings_animation;
	if (this->animation_settings.use) {
		this->last_rendered_frame = this->animation_settings.frame_start - 1;
	}
}


void ZmqExporter::free()
{
	checkZmqClient();
	m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::Free));
}

void ZmqExporter::sync()
{
}

void ZmqExporter::set_viewport_quality(int quality)
{
	if (quality != m_RenderQuality) {
		m_RenderQuality = quality;
		m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::SetQuality, m_RenderQuality));
	}
}

void ZmqExporter::set_render_size(const int &w, const int &h)
{
	if (!is_viewport) {
		auto imageIter = m_LayerImages.find(RenderChannelType::RenderChannelTypeNone);
		if (imageIter == m_LayerImages.end()) {
			std::unique_lock<std::mutex> lock(m_ImgMutex);
			auto & image = m_LayerImages[RenderChannelType::RenderChannelTypeNone];

			if (!image.pixels || image.w != w || image.h != h || image.channels != 4) {
			
				image.channels = 4;
				image.w = w;
				image.h = h;

				delete[] image.pixels;
				image.pixels = new float[image.w * image.h * image.channels];
				memset(image.pixels, 0, image.w * image.h * image.channels);
			}
		}
	}
	checkZmqClient();
	m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::Resize, w, h));
}

void ZmqExporter::start()
{
	checkZmqClient();
	m_Started = true;
	m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::Start));
}


void ZmqExporter::stop()
{
	checkZmqClient();
	m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::Stop));
}

void ZmqExporter::export_vrscene(const std::string &filepath)
{
	checkZmqClient();
	m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::ExportScene, filepath));
}

int ZmqExporter::remove_plugin_impl(const std::string &name)
{
	checkZmqClient();
	m_Client->send(VRayMessage::createMessage(name, VRayMessage::PluginAction::Remove));
	return PluginExporter::remove_plugin_impl(name);
}

AttrPlugin ZmqExporter::export_plugin_impl(const PluginDesc & pluginDesc)
{
	checkZmqClient();

	if (pluginDesc.pluginID.empty()) {
		PRINT_WARN("[%s] PluginDesc.pluginID is not set!",
			pluginDesc.pluginName.c_str());
		return AttrPlugin();
	}

	const bool checkAnimation = animation_settings.use && !is_viewport;
	const std::string & name = pluginDesc.pluginName;
	AttrPlugin plugin(name);

	m_Client->send(VRayMessage::createMessage(name, pluginDesc.pluginID));

	if (checkAnimation) {
		assert(m_LastExportedFrame <= this->current_scene_frame && "Exporting out of order frames!");
		if (m_LastExportedFrame != this->current_scene_frame) {
			m_LastExportedFrame = this->current_scene_frame;
			m_Client->send(VRayMessage::createMessage(VRayMessage::RendererAction::SetCurrentTime, this->current_scene_frame));
		}
	}

	for (auto & attributePairs : pluginDesc.pluginAttrs) {
		const PluginAttr & attr = attributePairs.second;

		switch (attr.attrValue.type) {
		case ValueTypeUnknown:
			break;
		case ValueTypeInt:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, VRayBaseTypes::AttrSimpleType<int>(attr.attrValue.valInt)));
			break;
		case ValueTypeFloat:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, VRayBaseTypes::AttrSimpleType<float>(attr.attrValue.valFloat)));
			break;
		case ValueTypeString:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, VRayBaseTypes::AttrSimpleType<std::string>(attr.attrValue.valString)));
			break;
		case ValueTypeColor:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valColor));
			break;
		case ValueTypeVector:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valVector));
			break;
		case ValueTypeAColor:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valAColor));
			break;
		case ValueTypePlugin:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valPlugin));
			break;
		case ValueTypeTransform:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valTransform));
			break;
		case ValueTypeListInt:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valListInt));
			break;
		case ValueTypeListFloat:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valListFloat));
			break;
		case ValueTypeListVector:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valListVector));
			break;
		case ValueTypeListColor:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valListColor));
			break;
		case ValueTypeListPlugin:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valListPlugin));
			break;
		case ValueTypeListString:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valListString));
			break;
		case ValueTypeMapChannels:
			m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valMapChannels));
			break;
		case ValueTypeInstancer:
			if (checkAnimation && attr.attrValue.valInstancer.frameNumber != this->current_scene_frame) {
				auto inst = attr.attrValue.valInstancer;
				inst.frameNumber = this->current_scene_frame;
				m_Client->send(VRayMessage::createMessage(name, attr.attrName, inst));
			} else {
				m_Client->send(VRayMessage::createMessage(name, attr.attrName, attr.attrValue.valInstancer));
			}
			break;
		default:
			PRINT_INFO_EX("--- > UNIMPLEMENTED DEFAULT");
			assert(false);
			break;
		}
	}

	return plugin;
}
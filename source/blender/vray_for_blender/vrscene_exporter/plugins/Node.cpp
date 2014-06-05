/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Andrei Izrantcev <andrei.izrantcev@chaosgroup.com>
 *
 * * ***** END GPL LICENSE BLOCK *****
 */

#include "CGR_config.h"

#include "Node.h"

#include "CGR_json_plugins.h"
#include "CGR_rna.h"
#include "CGR_blender_data.h"
#include "CGR_string.h"
#include "CGR_vrscene.h"

#include "exp_nodes.h"

#include "GeomStaticMesh.h"
#include "GeomMayaHair.h"
#include "GeomMeshFile.h"
#include "GeomPlane.h"

#include "BKE_global.h"
#include "BKE_depsgraph.h"
#include "BKE_scene.h"
#include "BKE_material.h"
#include "BKE_particle.h"
#include "MEM_guardedalloc.h"
#include "BLI_sys_types.h"
#include "BLI_string.h"
#include "BLI_path_util.h"
#include "BLI_math_matrix.h"

extern "C" {
#  include "DNA_modifier_types.h"
#  include "DNA_material_types.h"
}

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/join.hpp>


VRayScene::Node::Node(Scene *scene, Main *main, Object *ob):
	VRayExportable(scene, main, ob),
	m_ntree(VRayNodeExporter::getNodeTree(m_set->b_data, (ID*)ob))
{
	m_geometry     = NULL;
	m_geometryType = VRayScene::eGeometryMesh;
	m_objectID     = 0;
	m_visible      = true;

	m_useRenderStatsOverride = false;
}


void VRayScene::Node::init(const std::string &mtlOverrideName)
{
	m_materialOverride = mtlOverrideName;

	setObjectID(m_ob->index);
	initTransform();

	initName();
	initHash();
}


void VRayScene::Node::freeData()
{
	DEBUG_PRINT(CGR_USE_DESTR_DEBUG, COLOR_RED"Node::freeData("COLOR_YELLOW"%s"COLOR_RED")"COLOR_DEFAULT, m_name.c_str());

	if(NOT(VRayExportable::m_set->m_isAnimation)) {
		if(m_geometry) {
			delete m_geometry;
			m_geometry = NULL;
		}
	}
}


char* VRayScene::Node::getTransform() const
{
	return const_cast<char*>(m_transform);
}


int VRayScene::Node::getObjectID() const
{
	return m_objectID;
}


void VRayScene::Node::initName(const std::string &name)
{
	if(NOT(name.empty()))
		m_name = name;
	else
		m_name = GetIDName((ID*)m_ob);
}


int VRayScene::Node::preInitGeometry(int useDisplaceSubdiv)
{
	RnaAccess::RnaValue rna((ID*)m_ob->data, "vray");

	if(NOT(rna.getBool("override")))
		m_geometryType = VRayScene::eGeometryMesh;
	else
		if(rna.getEnum("override_type") == 0)
			m_geometryType = VRayScene::eGeometryProxy;
		else if(rna.getEnum("override_type") == 1)
			m_geometryType = VRayScene::eGeometryPlane;

	if(m_geometryType == VRayScene::eGeometryMesh) {
		if(IsMeshValid(m_sce, m_main, m_ob)) {
			m_geometry = new GeomStaticMesh(m_sce, m_main, m_ob, useDisplaceSubdiv);
		}
	}
	else if(m_geometryType == VRayScene::eGeometryProxy)
		m_geometry = new GeomMeshFile(m_sce, m_main, m_ob);
	else if(m_geometryType == VRayScene::eGeometryPlane)
		m_geometry = new GeomPlane(m_sce, m_main, m_ob);

	if(NOT(m_geometry))
		return 0;

	m_geometry->preInit();

	// We will delete geometry as soon as possible,
	// so store name here
	m_geometryName = m_geometry->getName();

	return 1;
}


void VRayScene::Node::initGeometry()
{
	if(NOT(m_geometry)) {
		PRINT_ERROR("[%s] Node::initGeometry() => m_geometry is NULL!", m_name.c_str());
		return;
	}
	m_geometry->init();
}


void VRayScene::Node::initTransform()
{
	GetTransformHex(m_ob->obmat, m_transform);
}


void VRayScene::Node::initHash()
{
	std::stringstream hashData;
	hashData << m_transform << m_visible;

	m_hash = HashCode(hashData.str().c_str());
}


std::string VRayScene::Node::GetMaterialName(Material *ma, const std::string &materialOverride)
{
	std::string materialName = CGR_DEFAULT_MATERIAL;

	BL::NodeTree ntree = VRayNodeExporter::getNodeTree(m_set->b_data, (ID*)ma);
	if(ntree) {
		BL::Node maOutput = VRayNodeExporter::getNodeByType(ntree, "VRayNodeOutputMaterial");
		if(maOutput) {
			BL::Node maNode = VRayNodeExporter::getConnectedNode(ntree, maOutput, "Material");
			if(maNode) {
				std::string maName = StripString("NT" + ntree.name() + "N" + maNode.name());

				if(materialOverride.empty())
					materialName = maName;
				else {
					PointerRNA maOutputRNA;
					RNA_id_pointer_create((ID*)maOutput.ptr.data, &maOutputRNA);
					if(RNA_boolean_get(&maOutputRNA, "dontOverride")) {
						materialName = maName;
					}
				}
			}
		}
	}
	else {
		std::string maName = GetIDName((ID*)ma);

		if(materialOverride.empty())
			materialName = maName;
		else {
			RnaAccess::RnaValue rna(&ma->id, "vray");
			if(rna.getBool("dontOverride"))
				materialName = maName;
		}
	}

	return materialName;
}


std::string Node::GetNodeMtlMulti(Object *ob, const std::string materialOverride, AttributeValueMap &mtlMulti)
{
	if(NOT(ob->totcol))
		return CGR_DEFAULT_MATERIAL;

	StrVector mtls_list;
	StrVector ids_list;

	for(int a = 1; a <= ob->totcol; ++a) {
		std::string materialName = CGR_DEFAULT_MATERIAL;
		if(NOT(materialOverride.empty())) {
			materialName = materialOverride;
		}

		Material *ma = give_current_material(ob, a);
		// NOTE: Slot could present, but no material is selected
		if(ma) {
			materialName = Node::GetMaterialName(ma, materialOverride);
		}

		mtls_list.push_back(materialName);
		ids_list.push_back(BOOST_FORMAT_INT(a));
	}

	// No need for multi-material if only one slot
	// is used
	//
	if(mtls_list.size() == 1)
		return mtls_list[0];

	char obMtlName[MAX_ID_NAME];
	BLI_strncpy(obMtlName, ob->id.name+2, MAX_ID_NAME);
	StripString(obMtlName);

	std::string plugName("MM");
	plugName.append(obMtlName);

	mtlMulti["mtls_list"] = "List("    + boost::algorithm::join(mtls_list, ",") + ")";
	mtlMulti["ids_list"]  = "ListInt(" + boost::algorithm::join(ids_list,  ",") + ")";

	return plugName;
}


std::string VRayScene::Node::writeMtlMulti(PyObject *output)
{
	AttributeValueMap mtlMulti;

	std::string mtlName = VRayScene::Node::GetNodeMtlMulti(m_ob, m_materialOverride, mtlMulti);

	if(mtlMulti.find("mtls_list") == mtlMulti.end())
		return mtlName;

	VRayNodePluginExporter::exportPlugin("NODE", "MtlMulti", mtlName, mtlMulti);

	return mtlName;
}


std::string VRayScene::Node::writeMtlWrapper(PyObject *output, const std::string &baseMtl)
{
	RnaAccess::RnaValue rna(&m_ob->id, "vray.MtlWrapper");
	if(NOT(rna.getBool("use")))
		return baseMtl;

	std::string pluginName = "MtlWrapper" + baseMtl;

	std::stringstream ss;
	ss << "\n" << "MtlWrapper" << " " << pluginName << " {";
	ss << "\n\t" << "base_material=" << baseMtl << ";";
	writeAttributes(rna.getPtr(), m_pluginDesc.getTree("MtlWrapper"), ss);
	ss << "\n}\n";

	PYTHON_PRINT(output, ss.str().c_str());

	return pluginName;
}


std::string VRayScene::Node::writeMtlOverride(PyObject *output, const std::string &baseMtl)
{
	RnaAccess::RnaValue rna(&m_ob->id, "vray.MtlOverride");
	if(NOT(rna.getBool("use")))
		return baseMtl;

	std::string pluginName = "MtlOverride" + baseMtl;

	std::stringstream ss;
	ss << "\n" << "MtlOverride" << " " << pluginName << " {";
	ss << "\n\t" << "base_mtl=" << baseMtl << ";";
	writeAttributes(rna.getPtr(), m_pluginDesc.getTree("MtlOverride"), ss);
	ss << "\n}\n";

	PYTHON_PRINT(output, ss.str().c_str());

	return pluginName;
}


std::string VRayScene::Node::writeMtlRenderStats(PyObject *output, const std::string &baseMtl)
{
	RnaAccess::RnaValue rna(&m_ob->id, "vray.MtlRenderStats");
	if(NOT(rna.getBool("use")))
		return baseMtl;

	std::string pluginName = "MtlRenderStats" + baseMtl;

	std::stringstream ss;
	ss << "\n" << "MtlRenderStats" << " " << pluginName << " {";
	ss << "\n\t" << "base_mtl=" << baseMtl << ";";
	writeAttributes(rna.getPtr(), m_pluginDesc.getTree("MtlRenderStats"), ss);
	ss << "\n}\n";

	PYTHON_PRINT(output, ss.str().c_str());

	return pluginName;
}


std::string VRayScene::Node::writeHideFromView(const std::string &baseMtl)
{
	std::string pluginName = "HideFromView";
	pluginName.append(getName());

	AttributeValueMap hideFromViewAttrs;
	if(NOT(baseMtl.empty())) {
		hideFromViewAttrs["base_mtl"] = baseMtl;
	}
	hideFromViewAttrs["visibility"]             = BOOST_FORMAT_BOOL(m_renderStatsOverride.visibility);
	hideFromViewAttrs["gi_visibility"]          = BOOST_FORMAT_BOOL(m_renderStatsOverride.gi_visibility);
	hideFromViewAttrs["camera_visibility"]      = BOOST_FORMAT_BOOL(m_renderStatsOverride.camera_visibility);
	hideFromViewAttrs["reflections_visibility"] = BOOST_FORMAT_BOOL(m_renderStatsOverride.reflections_visibility);
	hideFromViewAttrs["refractions_visibility"] = BOOST_FORMAT_BOOL(m_renderStatsOverride.refractions_visibility);
	hideFromViewAttrs["shadows_visibility"]     = BOOST_FORMAT_BOOL(m_renderStatsOverride.shadows_visibility);

	// It's actually a material, but we will write it along with Node
	VRayNodePluginExporter::exportPlugin("NODE", "MtlRenderStats", pluginName, hideFromViewAttrs);

	return pluginName;
}


void VRayScene::Node::writeData(PyObject *output, VRayExportable *prevState, bool keyFrame)
{
	std::string material = writeMtlMulti(output);
	material = writeMtlOverride(output, material);
	material = writeMtlWrapper(output, material);
	material = writeMtlRenderStats(output, material);

	if(m_useRenderStatsOverride) {
		material = writeHideFromView(material);
	}

	AttributeValueMap pluginAttrs;
	pluginAttrs["material"]  = material.c_str();
	pluginAttrs["geometry"]  = getDataName();
	pluginAttrs["objectID"]  = BOOST_FORMAT_INT(getObjectID());
	pluginAttrs["visible"]   = BOOST_FORMAT_INT(m_visible);
	pluginAttrs["transform"] = BOOST_FORMAT_TM(m_transform);

	VRayNodePluginExporter::exportPlugin("NODE", "Node", getName(), pluginAttrs);
}


int VRayScene::Node::IsUpdated(Object *ob)
{
	if(ob->type == OB_FONT)
		return ob->id.pad2 & CGR_UPDATED_DATA;

	int updated = ob->id.pad2 & CGR_UPDATED_OBJECT;
	if(NOT(updated)) {
		if(ob->parent) {
			// XXX: Check exactly how parent update affects child object
			return VRayScene::Node::IsUpdated(ob->parent);
		}
	}
	return updated;
}


int VRayScene::Node::isUpdated()
{
	return isObjectUpdated() || isObjectDataUpdated();
}


int VRayScene::Node::isObjectUpdated()
{
	return VRayScene::Node::IsUpdated(m_ob);
}


int VRayScene::Node::isObjectDataUpdated()
{
	return m_ob->id.pad2 & CGR_UPDATED_DATA;
}


int VRayScene::Node::IsSmokeDomain(Object *ob)
{
	ModifierData *mod = (ModifierData*)ob->modifiers.first;
	while(mod) {
		if(mod->type == eModifierType_Smoke)
			return 1;
		mod = mod->next;
	}
	return 0;
}


int VRayScene::Node::HasHair(Object *ob)
{
	if(ob->particlesystem.first) {
		for(ParticleSystem *psys = (ParticleSystem*)ob->particlesystem.first; psys; psys = psys->next) {
			ParticleSettings *pset = psys->part;
			if(pset->type != PART_HAIR)
				continue;
			if(psys->part->ren_as == PART_DRAW_PATH)
				return 1;
		}
	}
	return 0;
}


int VRayScene::Node::DoRenderEmitter(Object *ob)
{
	if(ob->particlesystem.first) {
		int show_emitter = 0;
		for(ParticleSystem *psys = (ParticleSystem*)ob->particlesystem.first; psys; psys = psys->next)
			show_emitter += psys->part->draw & PART_DRAW_EMITTER;
		/* if no psys has "show emitter" selected don't render emitter */
		if (show_emitter == 0)
			return 0;
	}
	return 1;
}


int VRayScene::Node::isSmokeDomain()
{
	return Node::IsSmokeDomain(m_ob);
}


int VRayScene::Node::hasHair()
{
	return Node::HasHair(m_ob);
}


int VRayScene::Node::doRenderEmitter()
{
	return Node::DoRenderEmitter(m_ob);
}


void VRayScene::Node::setVisiblity(const int &visible)
{
	m_visible = visible;
}


void VRayScene::Node::setObjectID(const int &objectID)
{
	m_objectID = objectID;
}


void VRayScene::Node::setHideFromView(const VRayScene::RenderStats &renderStats)
{
	m_renderStatsOverride    = renderStats;
	m_useRenderStatsOverride = true;
}


int VRayScene::Node::isMeshLight()
{
	RnaAccess::RnaValue rna(&m_ob->id, "vray.LightMesh");
	return rna.getBool("use");
}


void VRayScene::Node::writeGeometry(PyObject *output, int frame)
{
	if(NOT(m_geometry)) {
		PRINT_ERROR("[%s] Node::writeGeometry() => m_geometry is NULL!", m_name.c_str());
		return;
	}

	int toDelete = m_geometry->write(output, frame);
	if(toDelete) {
		delete m_geometry;
		m_geometry = NULL;
	}
}


void VRayScene::Node::WriteHair(ExpoterSettings *settings, Object *ob)
{
	if(VRayNodeExporter::m_set->DoUpdateCheck() && NOT(IsObjectDataUpdated(ob))) {
		return;
	}

	if(ob->particlesystem.first) {
		for(ParticleSystem *psys = (ParticleSystem*)ob->particlesystem.first; psys; psys = psys->next) {
			ParticleSettings *pset = psys->part;
			if(pset->type != PART_HAIR)
				continue;
			if(psys->part->ren_as != PART_DRAW_PATH)
				continue;

			int           toDelete = false;
			GeomMayaHair *geomMayaHair = new GeomMayaHair(settings->m_sce, settings->m_main, ob);
			geomMayaHair->preInit(psys);
			if(VRayExportable::m_set->m_exportNodes)
				geomMayaHair->writeNode(settings->m_fileObject, settings->m_frameCurrent);
			if(VRayExportable::m_set->m_exportMeshes) {
				geomMayaHair->init();
				toDelete = geomMayaHair->write(settings->m_fileGeom, settings->m_frameCurrent);
			}
			if(toDelete)
				delete geomMayaHair;
		}
	}
}


void VRayScene::Node::writeHair(ExpoterSettings *settings)
{
	Node::WriteHair(settings, m_ob);
}
/*
 * Copyright 2011-2017 Blender Foundation
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

#include "kernel/kernel_compat_opencl.h"
#include "kernel/split/kernel_split_common.h"
#include "kernel/split/kernel_shader_sort.h"

__attribute__((reqd_work_group_size(64, 1, 1)))
__kernel void kernel_ocl_path_trace_shader_sort(
        ccl_global char *kg,
        ccl_constant KernelData *data)
{
	ccl_local ShaderSortLocals locals;
	kernel_shader_sort((KernelGlobals*)kg, &locals);
}

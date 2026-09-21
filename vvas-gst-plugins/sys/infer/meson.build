########################################################################
 # Copyright (C) 2020 - 2022 Xilinx, Inc.
 # Copyright (C) 2022 - 2025 Advanced Micro Devices, Inc.
 #
 # Licensed under the Apache License, Version 2.0 (the "License");
 # you may not use this file except in compliance with the License.
 # You may obtain a copy of the License at
 #
 #     http://www.apache.org/licenses/LICENSE-2.0
 #
 # Unless required by applicable law or agreed to in writing, software
 # distributed under the License is distributed on an "AS IS" BASIS,
 # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 # See the License for the specific language governing permissions and
 # limitations under the License.
#########################################################################

dpucore_path = vvascore_dep.get_variable(pkgconfig : 'libdir')
dpucore_version = vvascore_dep.version()

# ONNXRuntime dependency
ort_dep = cc.find_library('onnxruntime', required : true)

# VART library dependency
vart_dep = dependency('vart-ml', required : true)

gstvvas_xinfer = library('gstvvas_xinfer', [ 'utils/vvas_memory_pool.c', 'utils/vvas_tensor_pool.cpp',
                                             'gstvvas_xinfer_helper.cpp', 'gstvvas_xinfer.cpp'],
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  dependencies : [gstvideo_dep, gst_dep, gstvvaspool_dep, gstvvasalloc_dep, dl_dep, jansson_dep,
                  gstallocators_dep, vvascore_dep, gstvvasutils_dep, gstvvasinfermeta_dep,
                  math_dep, gstvvascoreutils_dep, vvasstructure_dep, ort_dep, vart_dep,
                  gstvvaslogbridge_dep],
  install : true,
  install_dir : plugins_install_dir,
)

pkgconfig.generate(gstvvas_xinfer, install_dir : plugins_pkgconfig_install_dir)
plugins += [gstvvas_xinfer]

# VVAS postprocessing wrapper for VART
subdir('vvas_postprocess_vart')

# Reference postprocess libraries (resnet50, resnet18, yolo) shipped as source
subdir('vvas_postprocess_models')

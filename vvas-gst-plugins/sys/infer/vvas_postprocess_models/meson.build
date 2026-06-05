########################################################################
# Copyright (C) 2026 Advanced Micro Devices, Inc.
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

# Reference postprocess libraries (resnet50, resnet18, yolo) shipped to
# customers as source. Each library implements the C ABI defined in
# <vvas_core/vvas_postprocess.h> and is loaded at runtime by vvas_xinfer
# via the `library-path` field in the postprocess JSON config. See
# README.md in this directory.

core_version = '1.0'

postprocess_resnet50 = library('vvascore_postprocess_resnet50-' + core_version,
  'postprocess_resnet50.cpp',
  cpp_args : [gst_plugins_vvas_args, '-std=c++23'],
  include_directories : [configinc],
  dependencies : [vvascore_dep, vvasutils_dep],
  install : true,
)

postprocess_resnet18 = library('vvascore_postprocess_resnet18-' + core_version,
  'postprocess_resnet18.cpp',
  cpp_args : [gst_plugins_vvas_args, '-std=c++23'],
  include_directories : [configinc],
  dependencies : [vvascore_dep, vvasutils_dep],
  install : true,
)

postprocess_yolo = library('vvascore_postprocess_yolo-' + core_version,
  'postprocess_yolo.cpp',
  cpp_args : [gst_plugins_vvas_args, '-std=c++23'],
  include_directories : [configinc],
  dependencies : [vvascore_dep, vvasutils_dep],
  install : true,
)

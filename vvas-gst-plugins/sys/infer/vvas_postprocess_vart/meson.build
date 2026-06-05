########################################################################
# Copyright (C) 2025 Advanced Micro Devices, Inc.
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

# VVAS Wrapper Library for VART
# This library provides a VVAS-compliant interface that internally uses
# VART postprocessing implementations (ResNet50, YOLOv2, SSDResNet34)

message('Configuring VVAS wrapper for VART postprocessing...')

#VART-X dependency
vartx_dep = dependency('vart-x', version : '>= 0.1', required: true)

# Get version from vart-x dependency
vart_version = vartx_dep.version()

# VVAS Core dependency (required)
vvascore_dep = dependency('vvas-core', version : '>= 1.0', required: true)

# Source files for the wrapper
vvas_wrapper_sources = [
  'vvas_postprocess_vart.cpp',
  'vvas_postprocess_vart_wrapper.cpp'
]

# Include directories
vvas_wrapper_inc = [
  include_directories('.')
]

# Dependencies
vvas_wrapper_deps = [
  vvascore_dep,
  vartx_dep
]

# Build the shared library
vvascore_postprocess_vart_lib = library('vvascore_postprocess_vart',
  vvas_wrapper_sources,
  cpp_args: ['-std=c++17', '-Wno-psabi'],
  include_directories: vvas_wrapper_inc,
  dependencies: vvas_wrapper_deps,
  version: vart_version,
  soversion: vart_version.split('.')[0],
  install: true,
)

message('Successfully configured libvvascore_postprocess_vart.so.' + vart_version)
message('  This library can be used with VVAS applications by specifying:')
message('  lib_path = "/usr/lib/libvvascore_postprocess_vart.so.' + vart_version + '"')
message('  Or use the unversioned symlink: "/usr/lib/libvvascore_postprocess_vart.so"')

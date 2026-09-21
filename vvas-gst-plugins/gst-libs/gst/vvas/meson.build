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

# VVAS metadata
meta_sources = ['gstvvaslameta.c']

gstvvaslameta = library('gstvvaslameta-' + vvas_version,
  meta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
)
gstvvaslameta_dep = declare_dependency(link_with : [gstvvaslameta], dependencies : [gst_dep, gstbase_dep, gstvideo_dep])

# VVAS preprocessing geometry metadata
preprocessmeta_sources = ['gstvvaspreprocessmeta.c']

gstvvaspreprocessmeta = library('gstvvaspreprocessmeta-' + vvas_version,
  preprocessmeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
)
gstvvaspreprocessmeta_dep = declare_dependency(link_with : [gstvvaspreprocessmeta], dependencies : [gst_dep, gstbase_dep, gstvideo_dep])

# Extended GstInferenceMeta for VVAS
infermeta_sources = ['gstinferencemeta.c', 'gstinferenceprediction.c']

gstvvasinfermeta = library('gstvvasinfermeta-' + vvas_version,
  infermeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, vvasutils_dep, math_dep, vvascore_dep, gstvvaspreprocessmeta_dep],
)
gstvvasinfermeta_dep = declare_dependency(link_with : [gstvvasinfermeta], dependencies : [gst_dep, gstbase_dep, vvasutils_dep, math_dep, vvascore_dep, gstvvaspreprocessmeta_dep])

#VVAS allocator using XRT
alloc_sources = ['gstvvasallocator.c']

gstvvasalloc = library('gstvvasallocator-' + vvas_version,
  alloc_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, xrt_dep, gstallocators_dep, vvascore_dep],
)
gstvvasalloc_dep = declare_dependency(link_with : [gstvvasalloc], dependencies : [gst_dep, gstbase_dep, gstvideo_dep, xrt_dep, gstallocators_dep, vvascore_dep])

# VVAS Input Inference metadata
'''
inpinfermeta_sources = ['gstvvasinpinfer.c']

gstvvasinpinfermeta = library('gstvvasinpinfermeta-' + vvas_version,
  inpinfermeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, vvascore_dep],
)
gstvvasinpinfermeta_dep = declare_dependency(link_with : [gstvvasinpinfermeta], dependencies : [gst_dep, gstbase_dep, vvascore_dep])
'''

# VVAS Usr metadata
usrmeta_sources = ['gstvvasusrmeta.c']

gstvvasusrmeta = library('gstvvasusrmeta-' + vvas_version,
  usrmeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
)
gstvvasusrmeta_dep = declare_dependency(link_with : [gstvvasusrmeta], dependencies : [gst_dep, gstbase_dep])

# VVAS HDR metadata
hdrmeta_sources = ['gstvvashdrmeta.c']

gstvvashdrmeta = library('gstvvashdrmeta-' + vvas_version,
  hdrmeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
)
gstvvashdrmeta_dep = declare_dependency(link_with : [gstvvashdrmeta], dependencies : [gst_dep, gstbase_dep])

# VVAS overlay metadata
overlaymeta_sources = ['gstvvasoverlaymeta.c']

gstvvasoverlaymeta = library('gstvvasoverlaymeta-' + vvas_version,
  overlaymeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstvvasinfermeta_dep, vvascore_dep],
)
gstvvasoverlaymeta_dep = declare_dependency(link_with : [gstvvasoverlaymeta], dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstvvasinfermeta_dep, vvascore_dep])

# VVAS opticalflow metadata
ofmeta_sources = ['gstvvasofmeta.c']

gstvvasofmeta = library('gstvvasofmeta-' + vvas_version,
  ofmeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstvvasinfermeta_dep],
)
gstvvasofmeta_dep = declare_dependency(link_with : [gstvvasofmeta], dependencies : [gst_dep, gstbase_dep, vvasutils_dep, math_dep])

# VVAS SRC ID metadata
srcidmeta_sources = ['gstvvassrcidmeta.c']

gstvvassrcidmeta = library('gstvvassrcidmeta-' + vvas_version,
  srcidmeta_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
)
gstvvassrcidmeta_dep = declare_dependency(link_with : [gstvvassrcidmeta], dependencies : [gst_dep, gstbase_dep])

gstvvasutils = library('gstvvasutils-' + vvas_version, 'gstvvasutils.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc],
  version : libversion,
  soversion : soversion,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep],
  install : true,
)
gstvvasutils_dep = declare_dependency(link_with : [gstvvasutils], dependencies : [gst_dep, gstbase_dep, gstvideo_dep])

# VVAS / VART -> GST_DEBUG logging bridge.
log_bridge_vart = get_option('log_bridge_vart')
bridge_cpp_args = gst_plugins_vvas_args
bridge_deps = [gst_dep, vvascore_dep]
if log_bridge_vart
  vartx_dep_for_bridge = dependency('vart-x', version : '>= 0.1',
    required : true)
  bridge_cpp_args += ['-DHAVE_VART_X_LOG_BRIDGE']
  bridge_deps += [vartx_dep_for_bridge]
endif

gstvvaslogbridge = library('gstvvaslogbridge-' + vvas_version,
  'gstvvaslogbridge.cpp',
  cpp_args : bridge_cpp_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : bridge_deps,
)
gstvvaslogbridge_dep = declare_dependency(link_with : [gstvvaslogbridge],
  dependencies : bridge_deps)

gstvvascoreutils = library('gstvvascoreutils-' + vvas_version, 'gstvvascoreutils.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstallocators_dep, vvascore_dep, vvasutils_dep, gstvvasalloc_dep, xrt_dep, gstvvasinfermeta_dep, gstvvasusrmeta_dep],
  install : true,
)
gstvvascoreutils_dep = declare_dependency(link_with : [gstvvascoreutils], dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstallocators_dep, vvascore_dep, vvasutils_dep, gstvvasalloc_dep, xrt_dep, gstvvasinfermeta_dep, gstvvasusrmeta_dep])

#VVAS bufferpool with stride and elevation
vvaspool_sources = ['gstvvasbufferpool.c']

gstvvaspool = library('gstvvasbufferpool-' + vvas_version,
  vvaspool_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstvvasalloc_dep, gstvvascoreutils_dep],
)
gstvvaspool_dep = declare_dependency(link_with : [gstvvaspool], dependencies : [gst_dep, gstvideo_dep, gstbase_dep, gstvvasalloc_dep])

# Immutable tile composition layout shared by compositor, pools and overlay
vvastilecompositionlayout_sources = ['gstvvastilecompositionlayout.c']

gstvvastilecompositionlayout = library('gstvvastilecompositionlayout-' + vvas_version,
  vvastilecompositionlayout_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstvideo_dep],
)
gstvvastilecompositionlayout_dep = declare_dependency(
  link_with : [gstvvastilecompositionlayout],
  dependencies : [gst_dep, gstvideo_dep],
)

# Tile Composition slot/epoch coordinator, immutable leases and lease GstMeta
vvastilecompositioncoordinator_sources = [
  'gstvvastilecompositioncoordinator.c',
  'gstvvastilecompositionleasemeta.c',
]

gstvvastilecompositioncoordinator = library('gstvvastilecompositioncoordinator-' + vvas_version,
  vvastilecompositioncoordinator_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstvideo_dep, gstvvastilecompositionlayout_dep],
)
gstvvastilecompositioncoordinator_dep = declare_dependency(
  link_with : [gstvvastilecompositioncoordinator],
  dependencies : [gst_dep, gstvideo_dep, gstvvastilecompositionlayout_dep],
)

# VVAS master buffer pool for shared-BO tile compositions
vvasmasterpool_sources = ['gstvvasmasterpool.c']

gstvvasmasterpool = library('gstvvasmasterpool-' + vvas_version,
  vvasmasterpool_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstvvasalloc_dep,
                  gstvvastilecompositionlayout_dep],
)
gstvvasmasterpool_dep = declare_dependency(
  link_with : [gstvvasmasterpool],
  dependencies : [gst_dep, gstvideo_dep, gstbase_dep, gstvvasalloc_dep,
                  gstvvastilecompositionlayout_dep],
)

# VVAS producer buffer pool for writable tile views of the master BO
vvasslavepool_sources = ['gstvvasslavepool.c']

gstvvasslavepool = library('gstvvasslavepool-' + vvas_version,
  vvasslavepool_sources,
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, gstallocators_dep,
                  gstvvasmasterpool_dep, gstvvasalloc_dep,
                  gstvvastilecompositioncoordinator_dep, gstvvastilecompositionlayout_dep],
)
gstvvasslavepool_dep = declare_dependency(
  link_with : [gstvvasslavepool],
  dependencies : [gst_dep, gstvideo_dep, gstbase_dep, gstallocators_dep,
                  gstvvasmasterpool_dep, gstvvasalloc_dep,
                  gstvvastilecompositioncoordinator_dep, gstvvastilecompositionlayout_dep],
)

#VVAS GST Headers to install
vvas_gst_headers = ['gstvvaslameta.h',
                    'gstvvasallocator.h',
                    'gstvvasbufferpool.h',
                    'gstvvaspreprocessmeta.h',
                    'gstvvastilecompositioncoordinator.h',
                    'gstvvastilecompositionleasemeta.h',
                    'gstvvastilecompositionlayout.h',
                    'gstvvasmasterpool.h',
                    'gstvvasslavepool.h',
                    'gstinferencemeta.h',
                    'gstinferenceprediction.h',
                    'gstvvasinpinfer.h',
                    'gstvvasusrmeta.h',
                    'gstvvashdrmeta.h',
                    'gstvvasoverlaymeta.h',
                    'gstvvasofmeta.h',
                    'gstvvassrcidmeta.h',
                    'gstvvasutils.h',
                    'gstvvascommon.h',
                    'gstvvaslogbridge.h',
                    'gstvvascoreutils.h']

install_headers(vvas_gst_headers, subdir : 'gstreamer-1.0/gst/vvas/')

if get_option('build_tests')
  executable('test_log_bridge',
    'tests/test_log_bridge.c',
    c_args : gst_plugins_vvas_args,
    include_directories : [configinc, libsinc],
    dependencies : [gst_dep, vvascore_dep, gstvvaslogbridge_dep],
    install : true)
endif

gstvvaspool_dep = declare_dependency(link_with : [gstvvaspool], dependencies : [gst_dep, gstbase_dep])

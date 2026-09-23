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
project('vvas-gst-plugins', 'c', 'cpp',
  version : files('../VERSION'),
  meson_version : '>= 0.57.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

project_version = meson.project_version()
project_version_arr = project_version.split('.')
vvas_version = '@0@.@1@'.format(project_version_arr[0].to_int(),
                                project_version_arr[1].to_int())

gst_version = '1.18.5'
version_arr = gst_version.split('.')
gst_version_major = version_arr[0].to_int()
gst_version_minor = version_arr[1].to_int()
gst_version_micro = version_arr[2].to_int()
 if version_arr.length() == 4
  gst_version_nano = version_arr[3].to_int()
else
  gst_version_nano = 0
endif
gst_version_is_dev = gst_version_minor % 2 == 1 and gst_version_micro < 90

glib_req = '>= 2.44.0'
gst_req = '>= @0@.@1@.0'.format(gst_version_major, gst_version_minor)
xrt_req = '>= 2.6'

glib_deps = [dependency('glib-2.0', version : glib_req, fallback: ['glib', 'libglib_dep']),
             dependency('gobject-2.0', fallback: ['glib', 'libgobject_dep'])]

gst_plugins_vvas_args = ['-DHAVE_CONFIG_H']

configinc = include_directories('.')
libsinc = include_directories('gst-libs')

soversion = 0
# maintaining compatibility with the previous libtool versioning
# current = minor * 100 + micro
curversion = gst_version_minor * 100 + gst_version_micro
libversion = '@0@.@1@.0'.format(soversion, curversion)

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')
plugins = []

cdata = configuration_data()
cc = meson.get_compiler('c')

cdata.set_quoted('VERSION', gst_version)
cdata.set_quoted('PACKAGE', 'gst-plugins-vvas')
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE_BUGREPORT', 'https://github.com/amd/VVAS/issues')
cdata.set_quoted('PACKAGE_NAME', 'GStreamer VVAS Plug-ins')
cdata.set_quoted('GETTEXT_PACKAGE', 'gst-plugins-vvas-' + vvas_version)
cdata.set_quoted('VVAS_API_VERSION', vvas_version)
cdata.set_quoted('GST_LICENSE', 'proprietary')
cdata.set_quoted('LIBDIR', join_paths(get_option('prefix'), get_option('libdir')))
cdata.set_quoted('LOCALEDIR', join_paths(get_option('prefix'), get_option('localedir')))

# Embedded platform
message ('compiling for Embedded platform')
cdata.set('XLNX_EMBEDDED_PLATFORM ', 1)

cdata.set('ENABLE_XRT_GRAPH', 1)
add_project_arguments('-DVVAS_GLIB_UTILS', language : 'c')
add_project_arguments('-DVVAS_GLIB_UTILS', language : 'cpp')

warning_flags = [
  '-Wmissing-declarations',
  '-Wredundant-decls',
  '-Wwrite-strings',
  '-Winit-self',
  '-Wno-multichar',
  '-Wvla',
  '-Wpointer-arith',
]

warning_c_flags = [
  '-Wmissing-prototypes',
  '-Wdeclaration-after-statement',
  '-Wold-style-definition',
  '-Waggregate-return',
]

foreach extra_arg : warning_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
endforeach

foreach extra_arg : warning_c_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
endforeach

check_headers = [
  ['HAVE_STDIO_H', 'stdio.h'],
  ['HAVE_DLFCN_H', 'dlfcn.h'],
]

foreach h : check_headers
  if cc.has_header(h.get(1))
    cdata.set(h.get(0), 1)
  endif
endforeach

# Mandatory GST deps
gst_dep = dependency('gstreamer-1.0', version : gst_req,
  fallback : ['gstreamer', 'gst_dep'])
gstbase_dep = dependency('gstreamer-base-1.0', version : gst_req,
  fallback : ['gstreamer', 'gst_base_dep'])
gstallocators_dep = dependency('gstreamer-allocators-1.0', version : gst_req,
  fallback : ['gst-plugins-base', 'allocators_dep'])
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_req,
    fallback : ['gst-plugins-base', 'video_dep'])
gstallocators_dep = dependency('gstreamer-allocators-1.0', version : gst_req,
  fallback : ['gst-plugins-base', 'allocators_dep'])

# XRT dependency
xrt_dep = dependency('xrt', version : xrt_req, required : true)

#VvasStructure dependency
vvasstructure_dep = dependency('vvas-structure', version : '>= 1.0', required: true)

# External dependency
dl_dep = cc.find_library('dl', required : true)
uuid_dep = cc.find_library('uuid', required : true)
jansson_dep = dependency('jansson', version : '>= 2.7', required: true)

#VVAS Core dependency
vvascore_dep = dependency('vvas-core', version : '>= 1.0', required: true)

#VVAS utility dependency
vvasutils_dep = dependency('vvas-utils', version : '>= 1.0', required: true)
math_dep = cc.find_library('m', required : true)

xrm_dep = []

# PPE (PreProcessing Engine)
# Adds alpha/beta (mean/scale) normalization properties to abrscaler and multicrop.
cdata.set('ENABLE_PPE_SUPPORT ', 1)

plugins_pkgconfig_install_dir = join_paths(plugins_install_dir, 'pkgconfig')
if get_option('default_library') == 'shared'
  # If we don't build static plugins there is no need to generate pc files
  plugins_pkgconfig_install_dir = disabler()
endif

config_json_install_dir = join_paths('/etc', 'vvas', 'configs')

python3 = import('python').find_installation()
pkgconfig = import('pkgconfig')

#include directories
subdir('gst-libs')
subdir('gst')
subdir('sys')
subdir('pkgconfig')
if get_option('tests').enabled()
  subdir('tests')
endif

configure_file(output : 'config.h', configuration : cdata,
              install: true, install_dir: 'vvas/')

vvas_config_headers = ['../VERSION']
install_headers(vvas_config_headers, subdir : 'vvas/')

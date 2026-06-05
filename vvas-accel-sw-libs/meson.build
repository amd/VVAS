########################################################################
 # Copyright (C) 2020 - 2022 Xilinx, Inc.
 # Copyright (C) 2022 - 2026 Advanced Micro Devices, Inc.
 #
 # Permission is hereby granted, free of charge, to any person obtaining a
 # copy of this software and associated documentation files (the "Software"),
 # to deal in the Software without restriction, including without limitation the
 # rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 # sell copies of the Software, and to permit persons to whom the Software
 # is furnished to do so, subject to the following conditions:
 # The above copyright notice and this permission notice shall be included in
 # all copies or substantial portions of the Software.
 #
 # THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 # KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 # MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
 # EVENT SHALL XILINX BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 # WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
 # OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 # SOFTWARE. Except as contained in this notice, the name of the Xilinx shall
 # not be used in advertising or otherwise to promote the sale, use or other
 # dealings in this Software without prior written authorization from Xilinx.
########################################################################

project('vvas-accel-sw-libs', 'c', 'cpp',
  version : files('../VERSION'),
  meson_version : '>= 0.57.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

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
xrt_req = '>= 2.6' #XRT 2020.1

glib_deps = [dependency('glib-2.0', version : glib_req, fallback: ['glib', 'libglib_dep']),
             dependency('gobject-2.0', fallback: ['glib', 'libgobject_dep'])]

vvas_accel_sw_libs_args = ['-DHAVE_CONFIG_H']

configinc = include_directories('.')

soversion = 0
# maintaining compatibility with the previous libtool versioning
# current = minor * 100 + micro
curversion = gst_version_minor * 100 + gst_version_micro
libversion = '@0@.@1@.0'.format(soversion, curversion)

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')
plugins = []

cdata = configuration_data()
cc = meson.get_compiler('c')

cdata.set_quoted('LIBDIR', join_paths(get_option('prefix'), get_option('libdir')))
cdata.set_quoted('LOCALEDIR', join_paths(get_option('prefix'), get_option('localedir')))

# Embedded platform
message ('compiling for Embedded platform')
cdata.set('XLNX_EMBEDDED_PLATFORM ', 1)

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
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_req,
    fallback : ['gst-plugins-base', 'video_dep'])

# XRT dependency
xrt_dep = dependency('xrt', version : xrt_req, required : true)

#VVAS Core dependency
vvascore_dep = dependency('vvas-core', version : '>= 1.0', required: true)

#vvasutils dependency
vvasutils_dep = dependency('vvas-utils', version : '>= 1.0', required: true)

#vvasstructure dependency
vvasstructure_dep = dependency('vvas-structure', version : '>= 1.0', required: true)

# External dependency
jansson_dep = dependency('jansson', version : '>= 2.8', required: true)

opencv_dep = dependency('opencv4', version : '>=4.4.0', required: true)

math_dep = cc.find_library('m', required : true)
uuid_dep = cc.find_library('uuid', required : true)

vvas_videolib_install_path = ['/usr/lib/vvas/']

plugins_pkgconfig_install_dir = join_paths(plugins_install_dir, 'pkgconfig')
if get_option('default_library') == 'shared'
  # If we don't build static plugins there is no need to generate pc files
  plugins_pkgconfig_install_dir = disabler()
endif

python3 = import('python').find_installation()
pkgconfig = import('pkgconfig')

foreach kernel : ['vvas_xstub']
  if not get_option(kernel).disabled()
    subdir(kernel)
  endif
endforeach


configure_file(output : 'config.h', configuration : cdata)

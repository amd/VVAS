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

vvas_xstub = library('vvas_xstub',
  'src/vvas_xstub.c',
  c_args : [vvas_accel_sw_libs_args],
  include_directories : [configinc],
  dependencies : [gstvideo_dep, gst_dep, jansson_dep, xrt_dep, vvasutils_dep, vvascore_dep],
  install : true,
)


config_json_install_dir = join_paths('/etc', 'vvas', 'configs')

xfilter_json_install_dir = join_paths(config_json_install_dir, 'xfilter')

jsons = [
    'jsons/softlib_stub.json',
  ]

install_data(jsons, install_dir : xfilter_json_install_dir)


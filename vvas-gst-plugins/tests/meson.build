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

tile_composition_layout_test = executable('test-gstvvastilecompositionlayout',
  'test_gstvvastilecompositionlayout.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  dependencies : [gst_dep, gstvideo_dep, gstvvastilecompositionlayout_dep],
)

test('gstvvastilecompositionlayout', tile_composition_layout_test)

tile_composition_coordinator_test = executable('test-gstvvastilecompositioncoordinator',
  'test_gstvvastilecompositioncoordinator.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  dependencies : [
    gst_dep,
    gstvideo_dep,
    gstvvastilecompositionlayout_dep,
    gstvvastilecompositioncoordinator_dep,
  ],
)

test('gstvvastilecompositioncoordinator', tile_composition_coordinator_test)

tile_composition_lease_meta_test = executable('test-gstvvastilecompositionleasemeta',
  'test_gstvvastilecompositionleasemeta.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  dependencies : [
    gst_dep,
    gstvideo_dep,
    gstvvastilecompositionlayout_dep,
    gstvvastilecompositioncoordinator_dep,
  ],
)

test('gstvvastilecompositionleasemeta', tile_composition_lease_meta_test)

tile_composition_pools_test = executable('test-gstvvastilecompositionpools',
  'test_gstvvastilecompositionpools.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  dependencies : [
    gst_dep,
    gstbase_dep,
    gstvideo_dep,
    gstallocators_dep,
    gstvvasalloc_dep,
    gstvvastilecompositionlayout_dep,
    gstvvastilecompositioncoordinator_dep,
    gstvvasmasterpool_dep,
    gstvvasslavepool_dep,
  ],
)

test('gstvvastilecompositionpools', tile_composition_pools_test)

xtilecompositor_liveness_test = executable('test-gstvvasxtilecompositorliveness',
  'test_gstvvasxtilecompositorliveness.c',
  c_args : gst_plugins_vvas_args,
  include_directories : [configinc, libsinc],
  dependencies : [
    gst_dep,
    gstbase_dep,
    gstvideo_dep,
    gstvvastilecompositionlayout_dep,
    gstvvastilecompositioncoordinator_dep,
  ],
)

test('gstvvasxtilecompositorliveness', xtilecompositor_liveness_test)

xtilecompositor_fronts_test = executable('test-gstvvasxtilecompositorfronts',
  [
    'test_gstvvasxtilecompositorfronts.c',
    '../gst/xtilecompositor/gstvvas_xtilecompositorfronts.c',
  ],
  c_args : gst_plugins_vvas_args,
  include_directories : [
    configinc,
    libsinc,
    include_directories('../gst/xtilecompositor'),
  ],
  dependencies : [
    gst_dep,
    gstvvastilecompositioncoordinator_dep,
  ],
)

test('gstvvasxtilecompositorfronts', xtilecompositor_fronts_test)

#!/bin/bash
########################################################################
# Copyright (C) 2020 - 2022 Xilinx, Inc.
# Copyright (C) 2022 - 2026 Advanced Micro Devices, Inc.
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
########################################################################

# Clean all VVAS build artifacts.
# Usage: ./clean_vvas.sh

BASEDIR=$PWD

echo "Cleaning VVAS build artifacts..."

rm -rf "$BASEDIR/install"
rm -rf "$BASEDIR/vvas-utils/build" "$BASEDIR/vvas-utils/meson.cross"
rm -rf "$BASEDIR/vvas-gst-plugins/build" "$BASEDIR/vvas-gst-plugins/meson.cross"
rm -rf "$BASEDIR/vvas-accel-sw-libs/build" "$BASEDIR/vvas-accel-sw-libs/meson.cross"
rm -rf "$BASEDIR/vvas-examples/build" "$BASEDIR/vvas-examples/meson.cross"

echo "Done."

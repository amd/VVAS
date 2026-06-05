#!/bin/bash
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
########################################################################

# YOLOv5n object detection with bounding box overlay.
# Usage: ./03_detect_yolov5n.sh <input.nv12> <width> <height>

source "$(dirname $0)/common.sh"

if [ $# -lt 3 ]; then
    echo "Usage: $0 <INPUT_FILE.NV12> <WIDTH> <HEIGHT>"
    exit 1
fi

INPUT=$1
WIDTH=$2
HEIGHT=$3
OUTPUT="$OUTPUT_DIR/detected_yoloxm_int8.nv12"

echo "Input:  $INPUT (${WIDTH}x${HEIGHT})"
echo "Output: $OUTPUT"
echo ""

gst-launch-1.0 \
    filesrc location=$INPUT \
    ! rawvideoparse format=nv12 width=$WIDTH height=$HEIGHT \
    ! vvas_xinfer config-file=$INFER_DIR/yoloxm_int8_vart_zerocopy.json \
    ! vvas_xmetaconvert config-location=$METACONVERT \
    ! vvas_xoverlay \
    ! filesink location=$OUTPUT -v

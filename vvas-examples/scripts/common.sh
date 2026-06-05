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

# Shared variables for VVAS example scripts.
# Source this file from example scripts: source "$(dirname $0)/common.sh"

XCLBIN="/run/media/mmcblk0p1/x_plus_ml.xclbin"
CONFIG_DIR="/etc/vvas/configs"
INFER_DIR="${CONFIG_DIR}/infer"
METACONVERT="${CONFIG_DIR}/metaconvert/metaconvert_config.json"
MODEL_DIR="/dev/shm/models"
OUTPUT_DIR="/tmp/output"

mkdir -p "$OUTPUT_DIR"

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

# One-time VEK385 camera/RPU initialization.
#
# This script deliberately stops after isp_media_server startup. Model-specific
# media formats, preprocessing controls, and GStreamer pipelines are owned by
# the model-specific runners.
#
# NUM_CAMERAS / --cameras selects how many ISP paths to program. Do not
# configure unused ISPs: that path registers absent sensors and times out
# waiting for RPU ACK on boards with fewer cameras connected.

set -euo pipefail

FMC="${FMC:-96716A}"
SENSOR=""
NUM_CAMERAS="${NUM_CAMERAS:-4}"
readonly MAX_CAMERAS=4

log() {
  printf '[camera-init] %s\n' "$*"
}

usage() {
  cat <<EOF
Usage: $(basename "$0") --sensor=ox03f10|imx728 [--cameras=1-4]

Initializes the selected camera/RPU paths and starts isp_media_server.
Unused ISPs are left unconfigured. Model-specific media formats and
preprocessing controls are configured later by the run_4cam_* scripts.

Options:
  --sensor=SENSOR  (Required) Sensor type: ox03f10 or imx728
  --cameras=N      Live cameras to initialize, 1-4 (default: ${NUM_CAMERAS})
  -h, --help       Show this help message

Environment:
  NUM_CAMERAS=4    Same as --cameras; the flag overrides the environment
EOF
}

for arg in "$@"; do
  case "$arg" in
    --sensor=*) SENSOR="${arg#--sensor=}" ;;
    --fmc=*) FMC="${arg#--fmc=}" ;;
    --cameras=*) NUM_CAMERAS="${arg#--cameras=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$SENSOR" ]]; then
  echo "ERROR: --sensor argument is required" >&2
  usage >&2
  exit 2
fi

if ! [[ "$NUM_CAMERAS" =~ ^[0-9]+$ ]] ||
   (( NUM_CAMERAS < 1 || NUM_CAMERAS > MAX_CAMERAS )); then
  echo "ERROR: NUM_CAMERAS/--cameras must be between 1 and ${MAX_CAMERAS} (got $NUM_CAMERAS)" >&2
  exit 2
fi

# Set sensor-specific parameters.
case "$SENSOR" in
  ox03f10)
    SENSOR_NAME="ox03f10"
    TUNING_DIR="/usr/share/Tuning_files/OX03F10"
    CALIB_FILE="OX03F10_v1.0.json"
    AUTO_FILE="auto_OX03F10_v1.0.json"
    MANU_FILE="manual_OX03F10_v1.0.json"
    ;;
  imx728)
    SENSOR_NAME="imx728"
    TUNING_DIR="/usr/share/Tuning_files/IMX728"
    CALIB_FILE="IMX728_v1.1.json"
    AUTO_FILE="auto_IMX728_v1.1.json"
    MANU_FILE="manual_IMX728_v1.1.json"
    ;;
  *)
    echo "ERROR: unsupported sensor '$SENSOR' (use ox03f10 or imx728)" >&2
    exit 2
    ;;
esac

if [[ "$(id -u)" -ne 0 ]]; then
  echo "ERROR: run as root" >&2
  exit 1
fi

case "$FMC" in
  9296A)
    FMC_ID=0
    SENSOR_IDS=(2 3 6 7)
    HW_MCM=0
    ;;
  96716A)
    FMC_ID=1
    SENSOR_IDS=(2 3 4 5)
    HW_MCM=1
    ;;
  *)
    echo "ERROR: unsupported FMC '$FMC' (use 9296A or 96716A)" >&2
    exit 2
    ;;
esac

SENSOR_IDS=("${SENSOR_IDS[@]:0:NUM_CAMERAS}")

# Check the host tools before touching the camera or ISP state.
log "Selected FMC=$FMC (fmc_id=$FMC_ID, sensors=${SENSOR_IDS[*]}, hw_mcm=$HW_MCM), sensor=$SENSOR_NAME, cameras=$NUM_CAMERAS"
log "Checking required camera initialization commands"
for cmd in isp_media_server pgrep setsid; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "ERROR: required command not found: $cmd" >&2
    exit 1
  fi
  log "Found command: $cmd"
done

# Confirm only the selected ISP control endpoints are available.
log "Checking ISP control endpoints"
for ((isp = 0; isp < NUM_CAMERAS; isp++)); do
  if [[ ! -e "/proc/vsi/isp_subdev${isp}" ]]; then
    echo "ERROR: missing /proc/vsi/isp_subdev${isp}" >&2
    exit 1
  fi
  log "Found /proc/vsi/isp_subdev${isp}"
done

log "Selecting FMC through visp_lilo"
echo "$FMC_ID" > /sys/module/visp_lilo/parameters/fmc_id
log "Initializing $NUM_CAMERAS camera(s) for FMC=$FMC; unused ISPs are left unconfigured"

# Apply one sensor's identity, virtual channel, and tuning files.
configure_sensor() {
  local isp="$1"
  local sensor_id="$2"
  local vc_id="$3"
  local node="/proc/vsi/isp_subdev${isp}"

  log "Configuring ISP $isp (sensor=$SENSOR_NAME, sensor_id=$sensor_id, vc_id=$vc_id)"
  echo 0 sensor_id="$sensor_id" > "$node"
  echo 0 sensor="$SENSOR_NAME" > "$node"
  if [[ "$HW_MCM" -eq 1 ]]; then
    echo 0 hw_mcm=1 > "$node"
  fi
  echo 0 vc_id="$vc_id" > "$node"
  echo 0 calib="${TUNING_DIR}/${CALIB_FILE}" > "$node"
  echo 0 auto_json="${TUNING_DIR}/${AUTO_FILE}" > "$node"
  echo 0 manu_json="${TUNING_DIR}/${MANU_FILE}" > "$node"
  sleep 1
  log "Configured ISP $isp sensor and tuning files"
}

for ((isp = 0; isp < NUM_CAMERAS; isp++)); do
  configure_sensor "$isp" "${SENSOR_IDS[isp]}" "$((isp % 2))"
done

# Keep an existing media server and validate its process identity.
log "Checking whether isp_media_server is already running"
# Linux truncates the process comm name to 15 characters.
if ISP_SERVER_PID="$(pgrep -xo isp_media_serve)"; then
  log "isp_media_server is already running (pid=$ISP_SERVER_PID)"
else
  # Keep stdout/stderr attached to the invoking terminal for live diagnostics
  # while starting a new session so the server survives terminal hangup.
  log "Starting isp_media_server with terminal-visible diagnostics"
  setsid isp_media_server &
  log "Started isp_media_server; logs remain visible on this terminal"
fi

sleep 5

# Re-discover the process after startup so a failed launch is reported.
if ! ISP_SERVER_PID="$(pgrep -xo isp_media_serve)"; then
  echo "ERROR: isp_media_server is not running; see terminal output" >&2
  exit 1
fi

if ! kill -0 "$ISP_SERVER_PID" 2>/dev/null; then
  echo "ERROR: isp_media_server is not running (pid=$ISP_SERVER_PID)" >&2
  exit 1
fi

log "isp_media_server is alive (pid=$ISP_SERVER_PID)"
log "Camera initialization complete"

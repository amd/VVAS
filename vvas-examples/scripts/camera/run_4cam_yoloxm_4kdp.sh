#!/bin/bash
########################################################################
# Copyright (C) 2026 Advanced Micro Devices, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
########################################################################
#
# N-camera YOLOX-M INT8 VART demo to a 4K DisplayPort composed output.
#
# Preprocess (master, RGBx tensor):
#   v4l2src -> vvas_xinfer -> vvas_xmetaaffixer.sink_master
# ISP (slave, 1920x1080 BGR):
#   v4l2src -> vvas_xmetaaffixer.sink_slave_0 -> vvas_xmetaconvert
#           -> vvas_xtilecompositor -> vvas_xoverlay -> kmssink
#
# vvas_xtilecompositor keeps a fixed 2x2 / 4-pad layout. NUM_CAMERAS selects
# live camera branches; unused pads are black videotestsrc sources so all
# four compositor sinks stay connected.
#
# Edit Section 1 to port this example to another model. Edit Section 7 to
# change the GStreamer graph.

set -euo pipefail

# ---- 1. MODEL SETTINGS — edit these to run a different model ----
MODEL_NAME="YOLOX-M"
LOG_TAG="camera-yoloxm"
readonly TENSOR_WIDTH=640
readonly TENSOR_HEIGHT=640
# Model profile values are RGB; the helper maps them to GBR hardware slots.
# Original YOLOX profile: mean=[0,0,0], scale=[1,1,1],
# quant-scale-factor=0.5.
readonly MEAN_R=0
readonly MEAN_G=0
readonly MEAN_B=0
readonly SCALE_R=1
readonly SCALE_G=1
readonly SCALE_B=1
readonly QUANT_SCALE_FACTOR=0.5
INFER_DIR="${INFER_DIR:-/etc/vvas/configs/infer}"
INFER_CFGS=(
  "$INFER_DIR/yoloxm_int8_vart_extppe_column0.json"
  "$INFER_DIR/yoloxm_int8_vart_extppe_column4.json"
  "$INFER_DIR/yoloxm_int8_vart_extppe_column8.json"
  "$INFER_DIR/yoloxm_int8_vart_extppe_column12.json"
)

# ---- 2. RUNTIME OPTIONS — override from the environment ----
NUM_CAMERAS="${NUM_CAMERAS:-4}"
DRY_RUN="${DRY_RUN:-0}"
NUM_BUFFERS="${NUM_BUFFERS:-0}"
COOLDOWN_SECONDS="${COOLDOWN_SECONDS:-30}"
QUEUE_BUFFERS="${QUEUE_BUFFERS:-3}"
FPS="${FPS:-25}"
SKIP_CAMERA_PROFILE="${SKIP_CAMERA_PROFILE:-0}"
DISPLAY_FPS="${DISPLAY_FPS:-0}"
DISPLAY_DRIVER="${DISPLAY_DRIVER:-mmi-dc}"
DP_CONNECTOR_ID="${DP_CONNECTOR_ID:-46}"
DP_PLANE_ID="${DP_PLANE_ID:-34}"
XCLBIN="${XCLBIN:-/run/media/mmcblk0p1/x_plus_ml.xclbin}"
METACONVERT_CFG="${METACONVERT_CFG:-/etc/vvas/configs/metaconvert/metaconvert_config.json}"

# ---- 3. VEK385 PLATFORM LAYOUT — change only if the board changes ----
readonly MAX_CAMERAS=4
readonly CAMERA_WIDTH=1920
readonly CAMERA_HEIGHT=1080
readonly OUTPUT_WIDTH=3840
readonly OUTPUT_HEIGHT=2160
readonly CAMERA_FORMAT=BGR
readonly TENSOR_FORMAT=RGBx
readonly CAMERA_MEDIA_FORMAT=RBG888_1X24
readonly TENSOR_MEDIA_FORMAT=RGBA8888_1X32
CAMERA_DEVICE_IDS=(0 1 2 3 4 5 6 7)
MEDIA_NODE_IDS=(0 1 2 3)
PREPROCESS_SUBDEVS=(1 3 5 7)
ISP_ENTITIES=(
  visp-isp-subdev-lilo.0
  visp-isp-subdev-lilo.1
  visp-isp-subdev-lilo.2
  visp-isp-subdev-lilo.3
)
PREPROCESS_ENTITIES=(
  b00a0000.preprocess_accel
  b00b0000.preprocess_accel
  b00c0000.preprocess_accel
  b00d0000.preprocess_accel
)
readonly PREPROCESS_HELPER="/etc/vvas/examples/camera/compute_preprocess_params.sh"

# ---- 4. Helpers ----
log() {
  printf '[%s] %s\n' "$LOG_TAG" "$*"
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [--cameras=1-4] [--dry-run]

Initializes the ${MODEL_NAME} camera preprocessing profile and runs N-camera
VART-HW inference to a ${OUTPUT_WIDTH}x${OUTPUT_HEIGHT}@${FPS} DisplayPort output.
Unused compositor pads are filled with black videotestsrc.

Options:
  --cameras=N               Live cameras, 1-${MAX_CAMERAS} (overrides NUM_CAMERAS)
  --display-fps             Log per-camera FPS to the console (overrides DISPLAY_FPS)
  --dry-run                 Print the gst-launch graph and exit
  -h, --help                Show this help message

Environment:
  NUM_CAMERAS=4             Same as --cameras; the flag overrides the environment
  NUM_BUFFERS=0             Continuous run; set a positive bound for testing
  DISPLAY_FPS=1             Same as --display-fps; the flag overrides the environment
  SKIP_CAMERA_PROFILE=1     Do not repeat media/control configuration
  DP_CONNECTOR_ID=46        DRM DisplayPort connector
  DP_PLANE_ID=34            DRM primary plane
EOF
}

parse_args() {
  local arg
  for arg in "$@"; do
    case "$arg" in
      --cameras=*) NUM_CAMERAS="${arg#--cameras=}" ;;
      --display-fps) DISPLAY_FPS=1 ;;
      --dry-run) DRY_RUN=1 ;;
      -h|--help) usage; exit 0 ;;
      *) echo "Unknown argument: $arg" >&2; usage >&2; exit 2 ;;
    esac
  done
}

# ---- 5. Validation and preflight ----
validate_settings() {
  if (( OUTPUT_WIDTH != CAMERA_WIDTH * 2 ||
        OUTPUT_HEIGHT != CAMERA_HEIGHT * 2 )); then
    echo "ERROR: 2x2 layout requires ${CAMERA_WIDTH}x${CAMERA_HEIGHT}" \
      "tiles to compose into ${OUTPUT_WIDTH}x${OUTPUT_HEIGHT}" >&2
    exit 2
  fi
  if ! [[ "$NUM_CAMERAS" =~ ^[0-9]+$ ]] ||
     (( NUM_CAMERAS < 1 || NUM_CAMERAS > MAX_CAMERAS )); then
    echo "ERROR: NUM_CAMERAS/--cameras must be between 1 and ${MAX_CAMERAS} (got $NUM_CAMERAS)" >&2
    exit 2
  fi
  if (( ${#MEDIA_NODE_IDS[@]} != MAX_CAMERAS ||
        ${#CAMERA_DEVICE_IDS[@]} != MAX_CAMERAS * 2 ||
        ${#INFER_CFGS[@]} != MAX_CAMERAS )); then
    echo "ERROR: platform camera tables must describe ${MAX_CAMERAS} cameras" >&2
    exit 2
  fi
}

select_active_cameras() {
  local video_count=$((NUM_CAMERAS * 2))

  CAMERA_DEVICE_IDS=("${CAMERA_DEVICE_IDS[@]:0:video_count}")
  MEDIA_NODE_IDS=("${MEDIA_NODE_IDS[@]:0:NUM_CAMERAS}")
  PREPROCESS_SUBDEVS=("${PREPROCESS_SUBDEVS[@]:0:NUM_CAMERAS}")
  ISP_ENTITIES=("${ISP_ENTITIES[@]:0:NUM_CAMERAS}")
  PREPROCESS_ENTITIES=("${PREPROCESS_ENTITIES[@]:0:NUM_CAMERAS}")
  INFER_CFGS=("${INFER_CFGS[@]:0:NUM_CAMERAS}")
}

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "ERROR: run as root" >&2
    exit 1
  fi
}

check_required_commands() {
  local cmd
  log "Checking required commands"
  for cmd in gst-launch-1.0 gst-inspect-1.0 media-ctl modetest \
                v4l2-ctl awk pgrep; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      echo "ERROR: required command not found: $cmd" >&2
      exit 1
    fi
    log "Found command: $cmd"
  done
}

check_required_files() {
  local path
  log "Checking required model, output, and preprocessing files"
  for path in "${INFER_CFGS[@]}" "$METACONVERT_CFG" \
              "$XCLBIN" "$PREPROCESS_HELPER"; do
    if [[ ! -e "$path" ]]; then
      echo "ERROR: required file is missing: $path" >&2
      exit 1
    fi
    log "Found file: $path"
  done
  if [[ ! -x "$PREPROCESS_HELPER" ]]; then
    echo "ERROR: required helper is not executable: $PREPROCESS_HELPER" >&2
    exit 1
  fi
  log "Preprocess helper is executable: $PREPROCESS_HELPER"
}

check_video_devices() {
  local device_id media_id device
  log "Checking camera capture and media devices"
  for device_id in "${CAMERA_DEVICE_IDS[@]}"; do
    device="/dev/video${device_id}"
    if [[ ! -e "$device" ]]; then
      echo "ERROR: required device is missing: $device" >&2
      exit 1
    fi
    log "Found device: $device"
  done
  for media_id in "${MEDIA_NODE_IDS[@]}"; do
    device="/dev/media${media_id}"
    if [[ ! -e "$device" ]]; then
      echo "ERROR: required device is missing: $device" >&2
      exit 1
    fi
    log "Found device: $device"
  done
}

check_gst_plugins() {
  local plugin
  log "Checking required GStreamer plugins"
  local plugins=(v4l2src videotestsrc vvas_xinfer vvas_xmetaaffixer vvas_xmetaconvert
                  vvas_xoverlay vvas_xtilecompositor kmssink)
  if (( DISPLAY_FPS != 0 )); then
    plugins+=(perf)
  fi
  for plugin in "${plugins[@]}"; do
    if ! gst-inspect-1.0 "$plugin" >/dev/null 2>&1; then
      echo "ERROR: failed to load GStreamer plugin: $plugin" >&2
      exit 1
    fi
    log "Loaded plugin: $plugin"
  done
}

check_preprocess_controls() {
  local subdev
  for subdev in "${PREPROCESS_SUBDEVS[@]}"; do
    log "Checking preprocess_int8_out_ibits on /dev/v4l-subdev${subdev}"
    if ! v4l2-ctl -d "/dev/v4l-subdev${subdev}" --list-ctrls 2>/dev/null |
        awk '$1 == "preprocess_int8_out_ibits" { found=1 }
             END { exit !found }'; then
      echo "ERROR: /dev/v4l-subdev${subdev} lacks preprocess_int8_out_ibits" >&2
      echo "ERROR: target kernel must include the INT8 preprocess control patch" >&2
      exit 1
    fi
    log "Preprocess control available on /dev/v4l-subdev${subdev}"
  done
  log "All preprocess controls validated"
}

check_display_mode() {
  local display_mode="${OUTPUT_WIDTH}x${OUTPUT_HEIGHT}"
  log "Checking ${display_mode} mode on connector $DP_CONNECTOR_ID"
  if ! modetest -M "$DISPLAY_DRIVER" -c 2>/dev/null |
      awk -v target="$DP_CONNECTOR_ID" -v display_mode="$display_mode" '
        $1 == target { in_target=1; next }
        in_target && $0 ~ ("(^|[ \t])" display_mode "([ \t@]|$)") {
          found=1
        }
        in_target && $0 ~ /^[ \t]*[0-9]+[ \t]/ {
          in_target=0
        }
        END { exit !found }'; then
    echo "ERROR: DP connector $DP_CONNECTOR_ID does not advertise ${display_mode}" >&2
    exit 1
  fi
  log "Connector $DP_CONNECTOR_ID advertises ${display_mode}"
}

check_no_conflicting_pipeline() {
  if pgrep -f 'gst-launch-1.0|comp_4cam_concurrent' >/dev/null 2>&1; then
    echo "ERROR: another GStreamer/compositor process is running" >&2
    exit 1
  fi
  log "No conflicting GStreamer or compositor process found"
}

run_preflight() {
  check_required_commands
  check_required_files
  check_video_devices
  check_gst_plugins
  check_preprocess_controls
  check_display_mode
  check_no_conflicting_pipeline
}

# ---- 6. Camera and runtime setup ----
setup_runtime_env() {
  unset LD_PRELOAD
  export LD_LIBRARY_PATH="/usr/lib/python3.12/site-packages/vart_ml/lib:/usr/lib/python3.12/site-packages/vart_x/lib:/usr/lib/python3.12/site-packages/voe/lib:/usr/lib/python3.12/site-packages/flexmlrt/lib:/usr/lib/python3.12/site-packages/onnxruntime/capi${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export XLNX_ENABLE_CACHE=0
  export XRT_ELF_FLOW=1
  export XRT_AIARM=true
  export GST_DEBUG="${GST_DEBUG:-1}"
  export GST_DEBUG_NO_COLOR="${GST_DEBUG_NO_COLOR:-1}"
  log "Using XCLBIN=$XCLBIN"
  log "Using inference configs: ${INFER_CFGS[*]}"
}

configure_camera_profile() {
  local index media subdev

  log "Configuring ${MODEL_NAME} profile: ${CAMERA_WIDTH}x${CAMERA_HEIGHT} ${CAMERA_FORMAT} -> ${TENSOR_WIDTH}x${TENSOR_HEIGHT} ${TENSOR_FORMAT}"
  for index in "${!MEDIA_NODE_IDS[@]}"; do
    media="${MEDIA_NODE_IDS[$index]}"
    log "Camera $((index + 1)): media=/dev/media${media} isp=${ISP_ENTITIES[$index]}"
    log "Camera $((index + 1)): preprocess=${PREPROCESS_ENTITIES[$index]} subdev=/dev/v4l-subdev${PREPROCESS_SUBDEVS[$index]}"
    # pad0 is the sensor Bayer input configured via /proc/vsi — do NOT set via media-ctl.
    media-ctl -d "/dev/media${media}" -V \
      "\"${ISP_ENTITIES[$index]}\":1 [fmt:${CAMERA_MEDIA_FORMAT}/${CAMERA_WIDTH}x${CAMERA_HEIGHT}@1/${FPS} field:none]"
    media-ctl -d "/dev/media${media}" -V \
      "\"${ISP_ENTITIES[$index]}\":2 [fmt:${CAMERA_MEDIA_FORMAT}/${CAMERA_WIDTH}x${CAMERA_HEIGHT}@1/${FPS} field:none]"
    media-ctl -d "/dev/media${media}" -V \
      "\"${PREPROCESS_ENTITIES[$index]}\":0 [fmt:${CAMERA_MEDIA_FORMAT}/${CAMERA_WIDTH}x${CAMERA_HEIGHT} field:none]"
    media-ctl -d "/dev/media${media}" -V \
      "\"${PREPROCESS_ENTITIES[$index]}\":1 [fmt:${TENSOR_MEDIA_FORMAT}/${TENSOR_WIDTH}x${TENSOR_HEIGHT} field:none]"

    subdev="${PREPROCESS_SUBDEVS[$index]}"
    "$PREPROCESS_HELPER" \
      --mean-r="$MEAN_R" \
      --mean-g="$MEAN_G" \
      --mean-b="$MEAN_B" \
      --scale-r="$SCALE_R" \
      --scale-g="$SCALE_G" \
      --scale-b="$SCALE_B" \
      --quant-scale-factor="$QUANT_SCALE_FACTOR" \
      --v4l-subdev="$subdev"
    log "Camera $((index + 1)): media and preprocessing commands completed"
  done
}

wait_cooldown() {
  if (( COOLDOWN_SECONDS > 0 )); then
    log "Waiting ${COOLDOWN_SECONDS}s after the previous camera teardown"
    sleep "$COOLDOWN_SECONDS"
  fi
}

# ---- 7. Pipeline — edit here to change the GStreamer graph ----
# vvas_xtilecompositor keeps a fixed 2x2 3840x2160 layout and requires four
# sink pads. Live cameras occupy sink_0 .. sink_{N-1}. Remaining pads are
# black videotestsrc sources.
append_queue() {
  gst_args+=(
    ! queue max-size-buffers="$QUEUE_BUFFERS" max-size-bytes=0 max-size-time=0
  )
}

append_live_camera_branch() {
  local index="$1"
  local tensor_dev="/dev/video${CAMERA_DEVICE_IDS[index * 2]}"
  local display_dev="/dev/video${CAMERA_DEVICE_IDS[index * 2 + 1]}"
  local ma="ma${index}"

  gst_args+=(
    v4l2src device="$tensor_dev" io-mode=dmabuf-import
  )
  gst_args+=("${SOURCE_LIMIT[@]}")
  gst_args+=(
    ! video/x-raw,format="$TENSOR_FORMAT",width="$TENSOR_WIDTH",height="$TENSOR_HEIGHT",framerate="${FPS}/1"
    ! vvas_xinfer name="infer${index}" config-file="${INFER_CFGS[index]}"
  )
  append_queue
  gst_args+=(
    ! "${ma}.sink_master"
    "${ma}.src_master" ! fakesink async=false
    v4l2src device="$display_dev" io-mode=dmabuf-import
  )
  gst_args+=("${SOURCE_LIMIT[@]}")
  gst_args+=(
    ! video/x-raw,format="$CAMERA_FORMAT",width="$CAMERA_WIDTH",height="$CAMERA_HEIGHT",framerate="${FPS}/1"
  )
  append_queue
  gst_args+=(
    ! "${ma}.sink_slave_0"
    "${ma}.src_slave_0"
  )
  append_queue
  gst_args+=(
    ! vvas_xmetaconvert config-location="$METACONVERT_CFG"
  )
  append_queue
  if (( DISPLAY_FPS != 0 )); then
    gst_args+=(
      ! perf name="CAM${index}"
    )
  fi
  gst_args+=(
    ! "comp.sink_${index}"
  )
}

append_black_tile_branch() {
  local index="$1"

  log "Filling unused tile sink_${index} with black videotestsrc"
  gst_args+=(
    videotestsrc pattern=black is-live=true
  )
  gst_args+=("${SOURCE_LIMIT[@]}")
  gst_args+=(
    ! video/x-raw,format="$CAMERA_FORMAT",width="$CAMERA_WIDTH",height="$CAMERA_HEIGHT",framerate="${FPS}/1"
  )
  append_queue
  gst_args+=(
    ! "comp.sink_${index}"
  )
}

build_pipeline() {
  local index
  local filler_count=$((MAX_CAMERAS - NUM_CAMERAS))
  SOURCE_LIMIT=()
  if (( NUM_BUFFERS > 0 )); then
    SOURCE_LIMIT=("num-buffers=${NUM_BUFFERS}")
    log "Starting bounded ${MODEL_NAME} run: ${NUM_BUFFERS} buffers per source"
  else
    log "Starting continuous ${NUM_CAMERAS}-camera ${MODEL_NAME} run; Ctrl+C sends EOS"
  fi
  if (( filler_count > 0 )); then
    log "Connecting ${filler_count} black videotestsrc pad(s) so all ${MAX_CAMERAS} compositor sinks stay attached"
  fi

  gst_args=(
    gst-launch-1.0 -e
    vvas_xtilecompositor name=comp probe-downstream-layout=true
      xclbin-location="$XCLBIN"
      ! video/x-raw,format="$CAMERA_FORMAT",width="$OUTPUT_WIDTH",height="$OUTPUT_HEIGHT",framerate="${FPS}/1"
      ! vvas_xoverlay
      ! kmssink driver-name="$DISPLAY_DRIVER"
          connector-id="$DP_CONNECTOR_ID" plane-id="$DP_PLANE_ID"
          force-modesetting=true restore-crtc=false
          show-preroll-frame=false sync=false
  )

  for ((index = 0; index < NUM_CAMERAS; index++)); do
    gst_args+=(vvas_xmetaaffixer name="ma${index}" sync=false timeout=-1)
  done
  for ((index = 0; index < NUM_CAMERAS; index++)); do
    append_live_camera_branch "$index"
  done
  for ((index = NUM_CAMERAS; index < MAX_CAMERAS; index++)); do
    append_black_tile_branch "$index"
  done
}

# ---- 8. Entry point ----
main() {
  parse_args "$@"
  validate_settings
  select_active_cameras
  if (( DRY_RUN != 0 )); then
    build_pipeline
    log "Dry-run ${NUM_CAMERAS} live camera(s), $((MAX_CAMERAS - NUM_CAMERAS)) black tile(s):"
    printf '%s\n' "${gst_args[*]}"
    return 0
  fi
  require_root
  log "Starting ${MODEL_NAME} pipeline (cameras=$NUM_CAMERAS, camera=${CAMERA_WIDTH}x${CAMERA_HEIGHT}, tensor=${TENSOR_WIDTH}x${TENSOR_HEIGHT}, output=${OUTPUT_WIDTH}x${OUTPUT_HEIGHT}, fps=$FPS)"
  log "Runtime settings: buffers=$NUM_BUFFERS queue-buffers=$QUEUE_BUFFERS cooldown=${COOLDOWN_SECONDS}s"
  log "Display settings: driver=$DISPLAY_DRIVER connector=$DP_CONNECTOR_ID plane=$DP_PLANE_ID"
  run_preflight
  setup_runtime_env
  if (( SKIP_CAMERA_PROFILE == 0 )); then
    configure_camera_profile
  else
    log "Skipping camera profile because SKIP_CAMERA_PROFILE=$SKIP_CAMERA_PROFILE"
  fi
  wait_cooldown
  build_pipeline
  log "Launching composed output: ${OUTPUT_WIDTH}x${OUTPUT_HEIGHT} DisplayPort via $DISPLAY_DRIVER ($NUM_CAMERAS live camera(s), $((MAX_CAMERAS - NUM_CAMERAS)) black tile(s))"
  "${gst_args[@]}"
  log "gst-launch exited with status 0"
}

main "$@"

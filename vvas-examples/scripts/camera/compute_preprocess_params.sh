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

# Compute ISP preprocess accelerator parameters in Q23 fixed-point format
# and program them into a v4l2 subdevice.
#
# Converts floating-point mean/scale/quant-scale-factor into the integer
# register values expected by the v4l2 preprocess_param_alpha/beta controls
# and the preprocess_int8_out_ibits setting.
#
# Q23 format: value_q23 = round(value * 2^23)
#
# Model profile values use RGB order. The preprocess IP controls use GBR
# order: control 1 is G, control 2 is B, and control 3 is R.
#
# quant-scale-factor mapping to int8_out_ibits:
#   factor  ibits        factor  ibits
#     1       8           32       3
#     2       7           64       2
#     4       6          128       1
#     8       5          256       0
#    16       4
#
# For factors >= 1, ibits is rounded to the nearest representable
# power-of-two quantization step. Exact mapping is guaranteed only for the
# powers-of-two values listed above; other factors are approximations.
#
# If quant-scale-factor < 1, the hardware cannot represent it via ibits, so
# the factor is folded into scale-r/g/b and ibits is set to 8.
#
# Usage:
#   # Compute and apply to v4l-subdev1
#   /etc/vvas/examples/camera/compute_preprocess_params.sh --v4l-subdev=1
#
#   # Print computed values only (no hardware writes)
#   /etc/vvas/examples/camera/compute_preprocess_params.sh --dry-run
#
#   # Output shell variable assignments (for eval)
#   eval "$(/etc/vvas/examples/camera/compute_preprocess_params.sh --export)"

# Stop when a command returns nonzero.
set -euo pipefail

# Model preprocessing profile, expressed in RGB channel order.
MEAN_R=123.675
MEAN_G=116.28
MEAN_B=103.53
SCALE_R=0.017124
SCALE_G=0.017507
SCALE_B=0.017429
QUANT_SCALE=32.0
V4L_SUBDEV=""
EXPORT_MODE=0
DRY_RUN=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  --mean-r=VAL              Red channel mean     (default: $MEAN_R)
  --mean-g=VAL              Green channel mean   (default: $MEAN_G)
  --mean-b=VAL              Blue channel mean    (default: $MEAN_B)
  --scale-r=VAL             Red channel scale    (default: $SCALE_R)
  --scale-g=VAL             Green channel scale  (default: $SCALE_G)
  --scale-b=VAL             Blue channel scale   (default: $SCALE_B)
  --quant-scale-factor=VAL  Quantization scale   (default: $QUANT_SCALE)
  --v4l-subdev=N            V4L subdevice number to program (required unless
                            --export or --dry-run is used)
  --dry-run                 Print computed values without writing to hardware
  --export                  Output shell variable assignments (for eval)
  -h, --help                Show this help
EOF
  exit 0
}

# Parse the profile and select hardware, dry-run, or export mode.
for arg in "$@"; do
  case "$arg" in
    --mean-r=*)              MEAN_R="${arg#--mean-r=}" ;;
    --mean-g=*)              MEAN_G="${arg#--mean-g=}" ;;
    --mean-b=*)              MEAN_B="${arg#--mean-b=}" ;;
    --scale-r=*)             SCALE_R="${arg#--scale-r=}" ;;
    --scale-g=*)             SCALE_G="${arg#--scale-g=}" ;;
    --scale-b=*)             SCALE_B="${arg#--scale-b=}" ;;
    --quant-scale-factor=*)  QUANT_SCALE="${arg#--quant-scale-factor=}" ;;
    --v4l-subdev=*)          V4L_SUBDEV="${arg#--v4l-subdev=}" ;;
    --export)                EXPORT_MODE=1 ;;
    --dry-run)               DRY_RUN=1 ;;
    -h|--help)               usage ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

# Hardware writes require an explicit target subdevice.
if [ "$EXPORT_MODE" -eq 0 ] && [ "$DRY_RUN" -eq 0 ] && [ -z "$V4L_SUBDEV" ]; then
  echo "ERROR: --v4l-subdev=N is required (or use --dry-run / --export)" >&2
  exit 1
fi

# Calculate Q0.23 values from the RGB model profile.
read -r ALPHA_R ALPHA_G ALPHA_B BETA_R BETA_G BETA_B IBITS <<< "$(awk \
  -v mr="$MEAN_R" -v mg="$MEAN_G" -v mb="$MEAN_B" \
  -v sr="$SCALE_R" -v sg="$SCALE_G" -v sb="$SCALE_B" \
  -v qs="$QUANT_SCALE" '
BEGIN {
  Q23 = 2 ^ 23

  if (qs < 1.0) {
    ibits = 8
    sr *= qs
    sg *= qs
    sb *= qs
  } else {
    ibits = 8 - log(qs) / log(2)
    ibits = int(ibits + 0.5)
    if (ibits < 0) ibits = 0
    if (ibits > 8) ibits = 8
  }

  printf "%d %d %d %d %d %d %d\n",
    int(mr * Q23 + 0.5),
    int(mg * Q23 + 0.5),
    int(mb * Q23 + 0.5),
    int(sr * Q23 + 0.5),
    int(sg * Q23 + 0.5),
    int(sb * Q23 + 0.5),
    ibits
}')"

# Export semantic RGB values and their GBR hardware-slot equivalents.
if [ "$EXPORT_MODE" -eq 1 ]; then
  cat <<EOF
# Model values are RGB; hardware control slots are GBR.
ALPHA_R=$ALPHA_R
ALPHA_G=$ALPHA_G
ALPHA_B=$ALPHA_B
BETA_R=$BETA_R
BETA_G=$BETA_G
BETA_B=$BETA_B
ALPHA_1=$ALPHA_G
ALPHA_2=$ALPHA_B
ALPHA_3=$ALPHA_R
BETA_1=$BETA_G
BETA_2=$BETA_B
BETA_3=$BETA_R
IBITS=$IBITS
EOF
  exit 0
fi

# Print the computed hardware values without writing controls.
if [ "$DRY_RUN" -eq 1 ]; then
  cat <<EOF
Preprocess parameters (Q23 fixed-point):
  alpha_1 (mean-g):    $ALPHA_G
  alpha_2 (mean-b):    $ALPHA_B
  alpha_3 (mean-r):    $ALPHA_R
  beta_1  (scale-g):   $BETA_G
  beta_2  (scale-b):   $BETA_B
  beta_3  (scale-r):   $BETA_R
  int8_out_ibits:      $IBITS

Input values:
  mean:  R=$MEAN_R  G=$MEAN_G  B=$MEAN_B
  scale: R=$SCALE_R  G=$SCALE_G  B=$SCALE_B
  quant-scale-factor: $QUANT_SCALE
EOF
  exit 0
fi

# Program the preprocess IP controls in GBR slot order. A successful command
# means the driver accepted the write; use v4l2-ctl read-back for verification.
DEV="/dev/v4l-subdev${V4L_SUBDEV}"
printf '[preprocess] Programming %s (RGB profile -> GBR controls)\n' "$DEV"
printf '[preprocess] alpha_1/2/3 (G/B/R): %s/%s/%s\n' \
  "$ALPHA_G" "$ALPHA_B" "$ALPHA_R"
printf '[preprocess] beta_1/2/3 (G/B/R): %s/%s/%s\n' \
  "$BETA_G" "$BETA_B" "$BETA_R"
printf '[preprocess] int8_out_ibits: %s\n' "$IBITS"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_param_alpha_1="$ALPHA_G"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_param_alpha_2="$ALPHA_B"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_param_alpha_3="$ALPHA_R"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_param_beta_1="$BETA_G"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_param_beta_2="$BETA_B"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_param_beta_3="$BETA_R"
v4l2-ctl -d "$DEV" --set-ctrl=preprocess_int8_out_ibits="$IBITS"
printf '[preprocess] Control commands completed for %s\n' "$DEV"

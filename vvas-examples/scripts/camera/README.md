# Four-Camera Zero-Copy VART GStreamer Pipelines

End-to-end zero-copy GStreamer pipelines using `v4l2src` DMABUF-import
camera buffers, VART inference, tile composition, and `kmssink` DisplayPort
output. The runners and initializer accept `--cameras=1..4` (or `NUM_CAMERAS`)
so a board with fewer cameras connected can still run the 4K 2x2 output.

## Installation

Camera examples are installed by the local embedded VVAS build:

```bash
./build_install_vvas.sh
```

The top-level build script always enables the `mipi-camera` Meson option for
the local build. When enabled, the scripts are installed to:

```text
/etc/vvas/examples/camera/
```

The matching inference configs are installed to:

```text
/etc/vvas/configs/infer/
```

For Yocto recipes or direct Meson configuration, `mipi-camera=false` remains
the default; set that option to `true` when camera examples are required.

## Scripts

| Script | Purpose |
|--------|---------|
| `camera_init_4cam.sh` | One-time FMC, sensor, ISP, and `isp_media_server` initialization for 1-4 cameras. |
| `compute_preprocess_params.sh` | Converts model preprocessing profiles to Q0.23 hardware controls. |
| `run_4cam_yoloxm_4kdp.sh` | N-camera YOLOX-M INT8 VART inference to DisplayPort. |
| `run_4cam_resnet50_4kdp.sh` | N-camera ResNet50 INT8 VART inference to DisplayPort. |

## Prerequisites

- VEK385 camera hardware (1-4 cameras) and the required ISP/RPU setup.
- VVAS runtime, `x_plus_ml.xclbin`, and the corresponding compiled VART
  model artifacts.
- The `preprocess_int8_out_ibits` V4L2 control from the matching kernel patch.
- A VEK385 platform image with matching camera, ISP, and preprocess hardware
  support.
- A configured `mmi-dc` DisplayPort output.
- Run the scripts as root.

## Usage

Run camera initialization once after boot. The supported FMC is `96716A`.
Default is four cameras. On a board with only two cameras connected, pass
`--cameras=2` so unused ISPs are left unconfigured (configuring absent
sensors causes RPU register/ACK failures):

```bash
/etc/vvas/examples/camera/camera_init_4cam.sh --sensor=ox03f10
/etc/vvas/examples/camera/camera_init_4cam.sh --sensor=ox03f10 --cameras=2
```

Run one model pipeline with its default four-camera settings:

```bash
/etc/vvas/examples/camera/run_4cam_yoloxm_4kdp.sh
/etc/vvas/examples/camera/run_4cam_resnet50_4kdp.sh
```

Run the same 4K 2x2 composed output with fewer live cameras. Unused compositor
pads are filled with `videotestsrc pattern=black`:

```bash
/etc/vvas/examples/camera/run_4cam_yoloxm_4kdp.sh --cameras=2
/etc/vvas/examples/camera/run_4cam_resnet50_4kdp.sh --cameras=2
NUM_CAMERAS=2 /etc/vvas/examples/camera/run_4cam_yoloxm_4kdp.sh
```

Print the gst-launch graph without touching the board:

```bash
/etc/vvas/examples/camera/run_4cam_yoloxm_4kdp.sh --cameras=2 --dry-run
```

`NUM_BUFFERS` is unset by default, which runs continuously. Set it to a
positive value to bound the number of buffers per source for a test run.
Set `SKIP_CAMERA_PROFILE=1` when the camera media formats and preprocessing
controls have already been configured.

To port a runner to another model, edit the **Model settings** block at the
top of the script (tensor size, mean/scale, infer JSON list). Runtime knobs
such as `NUM_CAMERAS` are the next block. Change the pipeline functions only
if the GStreamer graph itself changes.

## Logging

The scripts print stage, configuration, validation, per-camera setup, and
pipeline lifecycle messages to the terminal.
`GST_DEBUG` remains user-controlled and defaults to level 1; set it explicitly
to a higher level only when GStreamer or inference debug output is needed.

## Pipeline and formats

Both runners use `v4l2src` with DMABUF import and one inference branch per
live camera. Each camera has separate preprocess/tensor and ISP/display input
paths:

```text
Preprocess path (master, RGBx tensor):
v4l2src → vvas_xinfer → vvas_xmetaaffixer.sink_master

ISP path (slave, 1920x1080 BGR):
v4l2src → vvas_xmetaaffixer.sink_slave_0 → vvas_xmetaconvert
        → vvas_xtilecompositor → vvas_xoverlay → kmssink
```

The two paths are repeated for each live camera and joined by
`vvas_xmetaaffixer` so inference metadata is applied to the corresponding ISP
frame. The composed output stays `3840x2160` with all four compositor pads
connected. Unused pads are black `videotestsrc` sources.

The preprocess/tensor sizes are:

- YOLOX-M: `640x640`
- ResNet50: `224x224`

The composed output is `3840x2160` at the configured FPS (default 25) on the
`mmi-dc` DisplayPort connector. The default DRM connector and plane IDs are
`46` and `34`; override them with `DP_CONNECTOR_ID` and `DP_PLANE_ID` when
required by the display.

The runners configure their model-specific media formats and invoke the helper
installed at `/etc/vvas/examples/camera/compute_preprocess_params.sh` by its
absolute rootfs path. Model profile values are supplied in RGB order; the
helper programs the preprocess controls in GBR order: control 1 is G, control
2 is B, and control 3 is R. YOLOX-M uses zero-mean, unit-scale values, while
ResNet50 uses ImageNet normalization.

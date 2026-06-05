# VVAS Examples

Example scripts and configuration files for running inference pipelines on
AMD embedded platforms using VVAS GStreamer plugins.

## Prerequisites

1. **Board setup** — VVAS tarball installed, FPGA bitstream programmed,
   environment variables set.

2. **Models** — Copy models to `/dev/shm/models/` on the board. Each model
   directory should contain:
   ```
   /dev/shm/models/resnet50/
   ├── ResNet50.onnx              # ONNX model (for ONNXRT backend)
   ├── ResNet50/                   # Compiled model (for VART backend)
   │   └── vaiml_par_0/
   └── vitisai_config.json         # VitisAI EP configuration
   ```

3. **Input video** — A raw NV12 file for pipeline testing.

## Quick Start

Run classification on synthetic video — no input file needed:
```bash
cd /etc/vvas/examples
./01_quickstart.sh
```

## Example Scripts

All scripts are installed to `/etc/vvas/examples/` on the board.

| Script | Description | Input |
|--------|-------------|-------|
| `01_quickstart.sh` | ResNet50 classification with overlay output (ONNXRT). | NV12 file |
| `02_classify_resnet50.sh` | ResNet50 classification with overlay output. | NV12 file |
| `03_detect_yolov5n.sh` | YOLOv5n object detection with bounding boxes. | NV12 file |
| `04_cascaded_pipeline.sh` | Detection (YOLOv5n) → Classification (ResNet50) chain. | NV12 file |
| `05_external_preprocessing.sh` | External preprocessing using `vvas_xabrscaler` with `vvas_xmetaaffixer` for overlay on original resolution. | NV12 file |

### Usage

Scripts that require input take 3 arguments:
```bash
./<script>.sh <INPUT_FILE.NV12> <WIDTH> <HEIGHT>
```

Example:
```bash
./02_classify_resnet50.sh /dev/shm/test_1920x1080.nv12 1920 1080
```

Output files are written to `/tmp/output/`.

### common.sh

All scripts source `common.sh` which defines shared variables:

| Variable | Value |
|----------|-------|
| `$XCLBIN` | `/run/media/mmcblk0p1/x_plus_ml.xclbin` |
| `$INFER_DIR` | `/etc/vvas/configs/infer` |
| `$METACONVERT` | `/etc/vvas/configs/metaconvert/metaconvert_config.json` |
| `$MODEL_DIR` | `/dev/shm/models` |
| `$OUTPUT_DIR` | `/tmp/output` |

To customize paths for your setup, edit `common.sh` instead of modifying
each script individually.

### Switching Backends

Each script uses a default config file. To switch between ONNXRT and VART
backends, edit the `config-file` path in the script or copy the script and
change the config. For example, to run classification with VART instead of
ONNXRT, change:
```
config-file=$INFER_DIR/resnet50_onnxrt.json
```
to:
```
config-file=$INFER_DIR/resnet50_vart.json
```

## Configuration Files

All configs are installed to `/etc/vvas/configs/` on the board.

### Inference Configs (`/etc/vvas/configs/infer/`)

#### Classification (ResNet)

| Config | Backend | Preprocessing | Postprocess |
|--------|---------|---------------|-------------|
| `resnet50_onnxrt.json` | ONNXRT VitisAI EP | Internal | `libvvascore_postprocess_resnet50` |
| `resnet50_vart.json` | VART | Internal | `libvvascore_postprocess_resnet50` |
| `resnet50_onnxrt_extppe.json` | ONNXRT VitisAI EP | External | `libvvascore_postprocess_resnet50` |
| `resnet50_onnxrt_softmax.json` | ONNXRT VitisAI EP | Internal | `libvvascore_postprocess_vart` (SOFTMAX) |
| `resnet50_vart_softmax.json` | VART | Internal | `libvvascore_postprocess_vart` (SOFTMAX) |
| `resnet50_l2.json` | VART | Internal | `libvvascore_postprocess_resnet50` (cascaded L2) |
| `resnet18_onnxrt.json` | ONNXRT VitisAI EP | Internal | `libvvascore_postprocess_resnet18` |
| `resnet18_vart.json` | VART | Internal | `libvvascore_postprocess_resnet18` |
| `resnet18_onnxrt_extppe.json` | ONNXRT VitisAI EP | External | `libvvascore_postprocess_resnet18` |

#### Detection (YOLOv5n)

| Config | Backend | Postprocess |
|--------|---------|-------------|
| `yolov5n_onnxrt.json` | ONNXRT VitisAI EP | `libvvascore_postprocess_yolo` |
| `yolov5n_vart.json` | VART | `libvvascore_postprocess_yolo` |
| `yolov5n_onnxrt_nms.json` | ONNXRT VitisAI EP | `libvvascore_postprocess_vart` (NMS) |
| `yolov5n_vart_nms.json` | VART | `libvvascore_postprocess_vart` (NMS) |

### Metaconvert Config (`/etc/vvas/configs/metaconvert/`)

| Config | Description |
|--------|-------------|
| `metaconvert_config.json` | Controls overlay rendering — label colors per cascade level |

## Pipeline Patterns

### 1. Internal Preprocessing (default)

Preprocessing (resize, color conversion, normalization) is handled by the
`vvas_xinfer` plugin internally using the HW image processing IP. Simplest
pipeline — single `vvas_xinfer` element does everything.

```
filesrc → rawvideoparse → vvas_xinfer → metaconvert → overlay → filesink
```

### 2. External Preprocessing

Preprocessing is done by `vvas_xabrscaler` upstream of `vvas_xinfer`. The
`vvas_xmetaaffixer` element maps inference metadata back to the original
resolution for overlay rendering.

```
filesrc → rawvideoparse → tee → abrscaler (mean/scale) → xinfer → metaaffixer → fakesink
                           └→ metaaffixer → metaconvert → overlay → filesink
```

Use this pattern when you need to control preprocessing parameters
(normalization values, aspect ratio) independently from inference.

### 3. Cascaded Pipeline

Two inference stages in series — typically detection (L1) followed by
classification (L2). The L2 stage processes each detected bounding box
individually.

```
filesrc → rawvideoparse → xinfer (L1: detect) → xinfer (L2: classify) → metaconvert → overlay → filesink
```

The L2 config must include `"inference-level": 2`.

## Config Fields Reference

Refer to the VVAS documentation for detailed configuration descriptions and advanced options.

### preprocess-config

| Field | Description | Example |
|-------|-------------|---------|
| `xclbin-location` | FPGA bitstream path | `/run/media/mmcblk0p1/x_plus_ml.xclbin` |
| `library-name` | HW preprocessing library | `libvvas_image_process_hw_lib.so` |
| `library-config` | Kernel configuration | `{"kernel-name":"image_processing:{image_processing_1}"}` |
| `mean-r/g/b` | Mean subtraction values | ResNet: 123.675, 116.28, 103.53 |
| `scale-r/g/b` | Scale normalization values | ResNet: 0.017124, 0.017507, 0.017429 |
| `maintain-aspect-ratio` | Preserve aspect ratio (1=yes, 0=no) | YOLOv5: 1, ResNet: 0 |
| `symmetric-padding` | Symmetric padding for letterboxing | YOLOv5: 1, ResNet: 0 |

### infer-config

| Field | Description | Values |
|-------|-------------|--------|
| `inference-level` | Pipeline level | 1 (default) or 2 (cascaded) |
| `model-format` | Input color format | `RGB` |
| `inference-backend-runtime` | Backend selection | `onnxrt` or `vart` |

### onnxrt-config

| Field | Description |
|-------|-------------|
| `onnx-model-path` | Path to .onnx model file |
| `input-tensor-layout` | Tensor layout | `NCHW` |
| `onnxrt-ep` | Execution provider | `vitisai` |
| `vitisai-ep-config.config-file-path` | VitisAI config path |
| `vitisai-ep-config.cache-dir` | Compiled model cache directory (parent of model dir) |
| `vitisai-ep-config.cache-key` | Compiled model subdirectory name |

### vart-config

| Field | Description |
|-------|-------------|
| `vart-model-path` | Path to compiled VART model directory |

### postprocess-config

| Field | Description |
|-------|-------------|
| `library-path` | Postprocess shared library path |
| `library-config` | Library-specific parameters |

#### ResNet postprocess (`libvvascore_postprocess_resnet50/18`)

| Field | Description | Default |
|-------|-------------|---------|
| `topk` | Number of top predictions | 1 |
| `label-file-path` | Path to class labels file | `/etc/vvas/configs/infer/imagenet-classes-1000.txt` |

#### YOLOv5 postprocess (`libvvascore_postprocess_yolo`)

| Field | Description | Default |
|-------|-------------|---------|
| `conf_thresh` | Confidence threshold | 0.25 |
| `iou_thresh` | IoU threshold for NMS | 0.5 |
| `preds_per_location` | Predictions per anchor (4+1+num_classes) | 85 |

## Standard Paths on Board

| What | Path |
|------|------|
| Models | `/dev/shm/models/<model_name>/` |
| FPGA bitstream | `/run/media/mmcblk0p1/x_plus_ml.xclbin` |
| Inference configs | `/etc/vvas/configs/infer/` |
| Metaconvert config | `/etc/vvas/configs/metaconvert/metaconvert_config.json` |
| Label files | `/etc/vvas/configs/infer/imagenet-classes-1000.txt` |
| Example scripts | `/etc/vvas/examples/` |
| Script output | `/tmp/output/` |

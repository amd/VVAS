# Reference Postprocess Libraries

This directory contains sample postprocess libraries for `vvas_xinfer`.
Each sample is a standalone shared object that converts raw model output
tensors into VVAS inference metadata.

The samples can be used directly for matching models or copied as a
starting point for a custom model:

| Source file | Installed library | Model family |
| --- | --- | --- |
| `postprocess_resnet50.cpp` | `libvvascore_postprocess_resnet50-1.0.so` | ImageNet classification |
| `postprocess_resnet18.cpp` | `libvvascore_postprocess_resnet18-1.0.so` | ImageNet classification |
| `postprocess_yolo.cpp` | `libvvascore_postprocess_yolo-1.0.so` | YOLO detection |

## How `vvas_xinfer` Loads Postprocess

Select a postprocess library in the `postprocess-config` section of the
`vvas_xinfer` JSON file. The `library-path` field names the shared object
to load, and `library-config` contains model-specific options passed to
that library.

```json
"postprocess-config": {
  "library-path": "/usr/lib/libvvascore_postprocess_resnet50-1.0.so",
  "library-config": {
    "topk": 1,
    "label-file-path": "/etc/vvas/configs/imagenet-classes-1000.txt"
  }
}
```

At pipeline startup, `vvas_xinfer` loads the shared object through the
VVAS postprocess dispatcher. The dispatcher calls the three exported C
symbols described below.

## Postprocess Library ABI

Every postprocess library must export these symbols from
`<vvas_core/vvas_postprocess.h>`:

```c
void *postprocess_init(char *json_conf,
                       VvasTensorInfo **t_info,
                       uint32_t num_tensors);

VvasReturnType postprocess_run(void *priv,
                               VvasMemory **tensor_memory,
                               uint32_t cur_batch_size,
                               VvasList **res);

VvasReturnType postprocess_deinit(void *priv);
```

### `postprocess_init()`

Called once when `vvas_xinfer` initializes the postprocess stage.

Typical responsibilities:

- Parse `json_conf`, which is the `library-config` object serialized as JSON.
- Read tensor metadata from `t_info`, such as shape, data type, tensor size,
  and quantization scale.
- Allocate and return a private context object used by later calls.

Return `NULL` if the configuration or tensor format is not supported.

### `postprocess_run()`

Called for each inference batch.

Typical responsibilities:

- Map each `VvasMemory` tensor for reading.
- Decode raw tensor values for each frame in the batch.
- Apply model-specific steps such as softmax, sigmoid, bounding-box decode,
  thresholding, top-k selection, or non-maximum suppression.
- Append one or more `VvasInferResult` objects to `res[b]` for each batch
  frame `b`.
- Unmap all mapped memory before returning.

`cur_batch_size` is the number of valid frames in the current inference
batch. `tensor_memory` contains the output tensor memory for the batch.

### `postprocess_deinit()`

Called when the postprocess instance is destroyed.

Free any memory allocated by `postprocess_init()`, including copied tensor
metadata, label lists, scratch buffers, and the private context object.

## Creating a Custom Postprocess Library

To add support for a new model:

1. Choose the closest sample:
   - Use a ResNet sample for simple classification output.
   - Use the YOLO sample for object detection with NMS.
2. Copy the source file and rename it for your model.
3. Define the expected output tensor layout, number of classes, and data
   types.
4. Parse any user-facing options from `library-config`.
5. Implement tensor decoding and result creation in `postprocess_run()`.
6. Add a `library(...)` target for the new source file in `meson.build`.
7. Rebuild and install `vvas-dev`.
8. Set `postprocess-config.library-path` to the installed `.so`.

Minimal C++ structure:

```cpp
struct MyPostprocessContext {
    /* Model-specific options and copied tensor metadata. */
};

extern "C" {

void *postprocess_init(char *json_conf,
                       VvasTensorInfo **t_info,
                       uint32_t num_tensors)
{
    auto *priv = new MyPostprocessContext;
    /* Parse json_conf, validate t_info, populate private state. */
    return priv;
}

VvasReturnType postprocess_run(void *priv,
                               VvasMemory **tensor_memory,
                               uint32_t cur_batch_size,
                               VvasList **res)
{
    /* Map tensors, decode outputs, append VvasInferResult objects. */
    return VVAS_RET_SUCCESS;
}

VvasReturnType postprocess_deinit(void *priv)
{
    delete static_cast<MyPostprocessContext *>(priv);
    return VVAS_RET_SUCCESS;
}

}
```

When writing a new library, validate the tensor shape and data type during
initialization. If the model uses labels, check that the label file exists
and contains enough entries. If the model uses quantized output, use the
tensor scale metadata when converting values to floating point.

## ResNet50 Sample Options

`libvvascore_postprocess_resnet50-1.0.so` expects one output tensor with
1000 ImageNet class scores.

| Field | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `topk` | integer | No | `1` | Number of highest-scoring classes to report. |
| `label-file-path` | string | Yes | None | Text file with one class label per line. |
| `disable-softmax` | boolean | No | `false` | Set to `true` if the model output already contains probabilities. |

Example:

```json
"postprocess-config": {
  "library-path": "/usr/lib/libvvascore_postprocess_resnet50-1.0.so",
  "library-config": {
    "topk": 5,
    "label-file-path": "/etc/vvas/configs/imagenet-classes-1000.txt",
    "disable-softmax": false
  }
}
```

## ResNet18 Sample Options

`libvvascore_postprocess_resnet18-1.0.so` expects one output tensor with
1000 ImageNet class scores. This sample always applies softmax before
selecting the top-k classes.

| Field | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `topk` | integer | No | `1` | Number of highest-scoring classes to report. |
| `label-file-path` | string | Yes | None | Text file with one class label per line. |

Example:

```json
"postprocess-config": {
  "library-path": "/usr/lib/libvvascore_postprocess_resnet18-1.0.so",
  "library-config": {
    "topk": 1,
    "label-file-path": "/etc/vvas/configs/imagenet-classes-1000.txt"
  }
}
```

## YOLO Sample Options

`libvvascore_postprocess_yolo-1.0.so` handles YOLO-style detection output.
It supports common output layouts such as `[predictions, attributes]` and
`[attributes, predictions]`. Detection results are filtered by confidence,
processed with NMS, and emitted as bounding boxes with class labels.

### YOLO `library-config` Fields

| Field | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `conf_thresh` | float | No | `0.25` | Minimum confidence. Must be in `[0.0, 1.0]`. |
| `iou_thresh` | float | No | `0.45` | NMS IoU threshold. Must be in `[0.0, 1.0]`. |
| `class_agnostic` | boolean | No | `false` | If `true`, NMS suppresses boxes across classes. If `false`, NMS is class-aware. |
| `multi_label` | boolean | No | `false` | If `true`, one prediction may emit multiple class detections. |
| `preset` | string | No | None | Applies known defaults for a YOLO family. Explicit fields override preset values. |
| `box_grid_decode` | boolean | No | Preset-dependent | Decode boxes using grid and stride metadata. |
| `preds_per_location` | integer | No | `1` | Number of predictions per grid location. Must be positive. |
| `apply_sigmoid` | boolean | No | Preset-dependent | Apply sigmoid to objectness and class scores. |
| `has_objectness_score` | boolean | No | Preset-dependent | Set `true` when each prediction has a separate objectness score. |
| `grid_strides` | integer array | No | `[8, 16, 32]` | Strides used for grid decode. Used only when `box_grid_decode` is `true`. |
| `input_width` | integer | No | `640` | Model input width used for grid generation. Used only when `box_grid_decode` is `true`. |
| `input_height` | integer | No | `640` | Model input height used for grid generation. Used only when `box_grid_decode` is `true`. |
| `max_detections` | integer | No | `300` | Maximum detections returned after NMS. Must be positive. |
| `class_label_file` | string | No | Built-in COCO labels | Optional label file with one class label per line. |

### YOLO Presets

Preset names are case-insensitive. The `v` after `yolo` is optional, so
`yolov5`, `yolo5`, `YOLOv5`, and `yoloV5` are equivalent.

| Preset families | Defaults |
| --- | --- |
| `yolo[v]5`, `yolo[v]7` | `has_objectness_score=true`, `apply_sigmoid=false`, `box_grid_decode=false`, `preds_per_location=3` |
| `yolo[v]8`, `yolo[v]9`, `yolo[v]11`, `yolo[v]12` | `has_objectness_score=false`, `apply_sigmoid=false`, `box_grid_decode=false`, `preds_per_location=1` |
| `yolo[v]x` | `has_objectness_score=true`, `apply_sigmoid=false`, `box_grid_decode=true`, `preds_per_location=1`, `grid_strides=[8,16,32]` |

Any explicitly provided field overrides the preset value.

### YOLOv8/YOLOv9/YOLOv11/YOLOv12-Style Example

Use this style for outputs shaped like `1 x (4 + classes) x predictions`,
where boxes and class scores are already decoded into model input
coordinates and there is no separate objectness score.

```json
"postprocess-config": {
  "library-path": "/usr/lib/libvvascore_postprocess_yolo-1.0.so",
  "library-config": {
    "preset": "yolov8",
    "conf_thresh": 0.25,
    "iou_thresh": 0.45,
    "max_detections": 300
  }
}
```

### YOLOX-Style Grid Decode Example

Use this style when the model output requires grid/stride decoding.

```json
"postprocess-config": {
  "library-path": "/usr/lib/libvvascore_postprocess_yolo-1.0.so",
  "library-config": {
    "preset": "yolox",
    "conf_thresh": 0.35,
    "iou_thresh": 0.30,
    "input_width": 640,
    "input_height": 640,
    "grid_strides": [8, 16, 32]
  }
}
```

If the model uses additional pyramid levels, set `grid_strides`
accordingly. For example, a P6 model may use `[8, 16, 32, 64]`, and a
P2-P5 model may use `[4, 8, 16, 32]`. The generated grid count must match
the model's prediction count.

## Build and Install

These samples are built as part of `vvas-dev`. After sourcing the target
SDK environment, build and install `vvas-dev` using the standard repository
script:

```bash
./build_install_vvas.sh
```

Install the resulting package on the target root filesystem, then point
`postprocess-config.library-path` at the installed library under `/usr/lib`.

## Troubleshooting Checklist

- Confirm `library-path` points to an installed `.so` on the target.
- Confirm `library-config` uses option names exactly as documented.
- For classification models, confirm `label-file-path` exists and has at
  least as many labels as the model output classes.
- For YOLO models, confirm `has_objectness_score`, `apply_sigmoid`, and
  `box_grid_decode` match the exported model output.
- If using a YOLO preset, remember that explicit JSON fields override preset
  defaults.
- If `box_grid_decode=true`, confirm `input_width`, `input_height`,
  `grid_strides`, and `preds_per_location` generate the same number of
  predictions as the output tensor.
- Confirm threshold values are within the documented ranges.

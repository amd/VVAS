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

project('vvas-examples', 'c',
  version : files('../VERSION'),
  meson_version : '>= 0.57.0')

# Install example inference configs and metaconvert config

infer_config_install_dir = join_paths('/etc', 'vvas', 'configs', 'infer')
metaconvert_config_install_dir = join_paths('/etc', 'vvas', 'configs', 'metaconvert')

infer_configs = [
    'configs/infer/resnet18_onnxrt.json',
    'configs/infer/resnet18_onnxrt_extppe.json',
    'configs/infer/resnet18_vart.json',
    'configs/infer/resnet50_onnxrt.json',
    'configs/infer/resnet50_onnxrt_extppe.json',
    'configs/infer/resnet50_onnxrt_softmax.json',
    'configs/infer/resnet50_vart.json',
    'configs/infer/resnet50_vart_softmax.json',
    'configs/infer/resnet50_l2.json',
    'configs/infer/yolov5n_onnxrt.json',
    'configs/infer/yolov5n_onnxrt_nms.json',
    'configs/infer/yolov5n_vart.json',
    'configs/infer/yolov5n_vart_nms.json',
    'configs/infer/imagenet-classes-1000.txt',
    'configs/infer/resnet50_int8_onnxrt_extppe.json',
    'configs/infer/resnet50_int8_onnxrt_intppe.json',
    'configs/infer/resnet50_int8_vart_zerocopy_extppe.json',
    'configs/infer/resnet50_int8_vart_zerocopy_intppe.json',
    'configs/infer/yoloxm_int8_onnxrt_intppe.json',
    'configs/infer/resnet50_int8_l2.json',
    'configs/infer/yoloxm_int8_vart_zerocopy.json',
    'configs/infer/segmentation_onnxrt_intppe.json',
]

install_data(infer_configs, install_dir : infer_config_install_dir)

mipi_camera_infer_configs = [
    'configs/infer/yoloxm_int8_vart_extppe.json',
    'configs/infer/yoloxm_int8_vart_extppe_column0.json',
    'configs/infer/yoloxm_int8_vart_extppe_column4.json',
    'configs/infer/yoloxm_int8_vart_extppe_column8.json',
    'configs/infer/yoloxm_int8_vart_extppe_column12.json',
    'configs/infer/resnet50_int8_vart_extppe.json',
    'configs/infer/resnet50_int8_vart_extppe_column0.json',
    'configs/infer/resnet50_int8_vart_extppe_column4.json',
    'configs/infer/resnet50_int8_vart_extppe_column8.json',
    'configs/infer/resnet50_int8_vart_extppe_column12.json',
]

if get_option('mipi-camera')
    install_data(mipi_camera_infer_configs, install_dir : infer_config_install_dir)
endif

install_data(
    'configs/metaconvert/metaconvert_config.json',
    'configs/metaconvert/metaconvert_segmentation.json',
    install_dir : metaconvert_config_install_dir,
)

# Install example scripts
scripts_install_dir = join_paths('/etc', 'vvas', 'examples')

example_scripts = [
    'scripts/common.sh',
    'scripts/01_classify_resnet50_int8.sh',
    'scripts/02_detect_yoloxm_int8.sh',
]

install_data(example_scripts,
    install_dir : scripts_install_dir,
    install_mode : 'rwxr-xr-x',
)

if get_option('mipi-camera')
    camera_scripts_install_dir = join_paths(scripts_install_dir, 'camera')
    camera_scripts = [
        'scripts/camera/camera_init_4cam.sh',
        'scripts/camera/compute_preprocess_params.sh',
        'scripts/camera/run_4cam_yoloxm_4kdp.sh',
        'scripts/camera/run_4cam_resnet50_4kdp.sh',
    ]

    install_data(camera_scripts,
        install_dir : camera_scripts_install_dir,
        install_mode : 'rwxr-xr-x',
    )
endif

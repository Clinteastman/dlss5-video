#!/usr/bin/env python3
"""Generate depth and NVIDIA Optical Flow guides from mpv frames in real time."""

from __future__ import annotations

import argparse
import mmap
import struct
import time
from pathlib import Path

import cv2
import numpy as np
import torch
from PIL import Image
from transformers import AutoImageProcessor, AutoModelForDepthEstimation

from NvidiaOpticalFlow import NvidiaOpticalFlow


MODEL_ID = "depth-anything/Depth-Anything-V2-Small-hf"
MAPPING_NAME = "Local\\DLSS5VideoLiveGuides"
CONTROL_MAPPING_NAME = "Local\\DLSS5VideoGuideControl"
HEADER = struct.Struct("<4sIIIIIIIqqIIQ")
CONTROL = struct.Struct("<4sIIIIIIIII")
INPUT_STATE = 24
OUTPUT_STATE = 28
INPUT_SEQUENCE = 32
OUTPUT_SEQUENCE = 40
RESET = 48
PROCESSING_US = 52
GENERATED_COUNT = 56
CONTROL_DEPTH_SIZE = 8
CONTROL_FLOW_PERCENT = 12
CONTROL_FLOW_PERF = 16
CONTROL_REQUEST_SEQUENCE = 20
CONTROL_APPLIED_SEQUENCE = 24
CONTROL_ACTIVE_FLOW_WIDTH = 28
CONTROL_ACTIVE_FLOW_HEIGHT = 32


def set_u32(mapping: mmap.mmap, offset: int, value: int) -> None:
    struct.pack_into("<I", mapping, offset, value)


def set_u64(mapping: mmap.mmap, offset: int, value: int) -> None:
    struct.pack_into("<Q", mapping, offset, value)


def get_u32(mapping: mmap.mmap, offset: int) -> int:
    return struct.unpack_from("<I", mapping, offset)[0]


def get_u64(mapping: mmap.mmap, offset: int) -> int:
    return struct.unpack_from("<Q", mapping, offset)[0]


def initialise_control(
    control: mmap.mmap,
    depth_size: int,
    flow_percent: int,
    flow_perf: int,
) -> None:
    current = CONTROL.unpack_from(control, 0)
    if current[0] == b"D5LC" and current[1] == 1:
        return
    control[:] = bytes(CONTROL.size)
    CONTROL.pack_into(
        control,
        0,
        b"D5LC",
        1,
        depth_size,
        flow_percent,
        flow_perf,
        1,
        0,
        0,
        0,
        0,
    )


def estimate_depth(
    rgb: np.ndarray,
    processor: AutoImageProcessor,
    model: AutoModelForDepthEstimation,
    device: torch.device,
    depth_size: int,
) -> np.ndarray:
    inputs = processor(
        images=Image.fromarray(rgb),
        size={"height": depth_size, "width": depth_size},
        return_tensors="pt",
    )
    inputs = {name: value.to(device) for name, value in inputs.items()}
    with torch.inference_mode():
        prediction = model(**inputs).predicted_depth.unsqueeze(1)
        prediction = torch.nn.functional.interpolate(
            prediction,
            size=rgb.shape[:2],
            mode="bicubic",
            align_corners=False,
        ).squeeze()
    return prediction.float().cpu().numpy().astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    parser.add_argument("--model-cache", required=True, type=Path)
    parser.add_argument("--flow-scale", type=float, default=1.0)
    parser.add_argument("--depth-size", type=int, default=518)
    parser.add_argument("--depth-history", type=float, default=0.25)
    parser.add_argument("--depth-range-history", type=float, default=0.95)
    parser.add_argument("--scene-cut-threshold", type=float, default=0.20)
    parser.add_argument("--max-frames", type=int, default=0)
    args = parser.parse_args()

    if not torch.cuda.is_available():
        raise SystemExit("The live guide service requires an NVIDIA CUDA GPU.")
    device = torch.device("cuda")
    args.model_cache.mkdir(parents=True, exist_ok=True)
    processor = AutoImageProcessor.from_pretrained(
        MODEL_ID, cache_dir=args.model_cache, use_fast=False
    )
    model = AutoModelForDepthEstimation.from_pretrained(
        MODEL_ID, cache_dir=args.model_cache, dtype=torch.float16
    ).to(device).eval()

    depth_size = args.depth_size
    flow_percent = round(args.flow_scale * 100)
    flow_perf = NvidiaOpticalFlow.FAST
    flow_width = max(32, round(args.width * flow_percent / 100))
    flow_height = max(16, round(args.height * flow_percent / 100))
    pixels = args.width * args.height
    frame_bytes = pixels * 4
    guide_bytes = pixels * 8
    mapping_size = HEADER.size + frame_bytes + guide_bytes
    mapping = mmap.mmap(-1, mapping_size, MAPPING_NAME, access=mmap.ACCESS_WRITE)
    control = mmap.mmap(
        -1, CONTROL.size, CONTROL_MAPPING_NAME, access=mmap.ACCESS_WRITE
    )
    initialise_control(control, depth_size, flow_percent, flow_perf)
    current = HEADER.unpack_from(mapping, 0)
    if (
        current[0] != b"D5LV"
        or current[1] != 1
        or current[2] != args.width
        or current[3] != args.height
    ):
        mapping[:] = bytes(mapping_size)
        HEADER.pack_into(
            mapping,
            0,
            b"D5LV",
            1,
            args.width,
            args.height,
            frame_bytes,
            guide_bytes,
            0,
            0,
            0,
            0,
            1,
            0,
            0,
        )
    else:
        # Recover if an earlier service was stopped while it owned either slot.
        set_u32(mapping, INPUT_STATE, 0)
        set_u32(mapping, OUTPUT_STATE, 0)

    previous_gray: np.ndarray | None = None
    previous_depth: np.ndarray | None = None
    previous_flow_frame: torch.Tensor | None = None
    previous_sequence = 0
    depth_low: float | None = None
    depth_high: float | None = None
    generated = 0
    nvof: NvidiaOpticalFlow | None = None
    try:
        estimate_depth(
            np.zeros((args.height, args.width, 3), dtype=np.uint8),
            processor,
            model,
            device,
            depth_size,
        )
        nvof = NvidiaOpticalFlow(
            flow_width, flow_height, perf_level=flow_perf
        )
        set_u32(control, CONTROL_ACTIVE_FLOW_WIDTH, flow_width)
        set_u32(control, CONTROL_ACTIVE_FLOW_HEIGHT, flow_height)
        set_u32(
            control,
            CONTROL_APPLIED_SEQUENCE,
            get_u32(control, CONTROL_REQUEST_SEQUENCE),
        )
        print(
            f"Live guides ready: {args.width}x{args.height} output, "
            f"depth {depth_size}, NVOF {flow_width}x{flow_height} perf {flow_perf}",
            flush=True,
        )
        while args.max_frames == 0 or generated < args.max_frames:
                if get_u32(mapping, INPUT_STATE) != 2:
                    time.sleep(0.001)
                    continue

                requested_sequence = get_u32(
                    control, CONTROL_REQUEST_SEQUENCE
                )
                if requested_sequence != get_u32(
                    control, CONTROL_APPLIED_SEQUENCE
                ):
                    requested_depth = get_u32(control, CONTROL_DEPTH_SIZE)
                    requested_flow_percent = get_u32(
                        control, CONTROL_FLOW_PERCENT
                    )
                    requested_flow_perf = get_u32(control, CONTROL_FLOW_PERF)
                    if requested_depth in (280, 392, 518):
                        depth_size = requested_depth
                    if requested_flow_percent in (25, 50, 100):
                        flow_percent = requested_flow_percent
                    if requested_flow_perf in (
                        NvidiaOpticalFlow.QUALITY,
                        NvidiaOpticalFlow.BALANCED,
                        NvidiaOpticalFlow.FAST,
                    ):
                        flow_perf = requested_flow_perf
                    new_flow_width = max(
                        32, round(args.width * flow_percent / 100)
                    )
                    new_flow_height = max(
                        16, round(args.height * flow_percent / 100)
                    )
                    nvof.close()
                    nvof = NvidiaOpticalFlow(
                        new_flow_width,
                        new_flow_height,
                        perf_level=flow_perf,
                    )
                    flow_width = new_flow_width
                    flow_height = new_flow_height
                    previous_gray = None
                    previous_depth = None
                    previous_flow_frame = None
                    depth_low = None
                    depth_high = None
                    set_u32(control, CONTROL_ACTIVE_FLOW_WIDTH, flow_width)
                    set_u32(control, CONTROL_ACTIVE_FLOW_HEIGHT, flow_height)
                    set_u32(
                        control,
                        CONTROL_APPLIED_SEQUENCE,
                        requested_sequence,
                    )
                    print(
                        f"Guide settings applied: depth {depth_size}, "
                        f"NVOF {flow_width}x{flow_height} perf {flow_perf}",
                        flush=True,
                    )
                sequence = get_u64(mapping, INPUT_SEQUENCE)
                frame = np.frombuffer(
                    mapping[HEADER.size : HEADER.size + frame_bytes],
                    dtype=np.uint8,
                ).reshape(args.height, args.width, 4).copy()
                set_u32(mapping, INPUT_STATE, 0)
                started = time.perf_counter()

                rgb = frame[:, :, :3]
                gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
                reset = previous_gray is None or sequence <= previous_sequence
                if previous_gray is not None:
                    difference = float(
                        np.mean(cv2.absdiff(gray, previous_gray))
                    ) / 255.0
                    reset = reset or difference >= args.scene_cut_threshold

                raw_depth = estimate_depth(
                    rgb, processor, model, device, depth_size
                )
                frame_low, frame_high = np.percentile(raw_depth, (2.0, 98.0))
                if reset or depth_low is None or depth_high is None:
                    depth_low, depth_high = float(frame_low), float(frame_high)
                else:
                    keep = args.depth_range_history
                    depth_low = depth_low * keep + float(frame_low) * (1.0 - keep)
                    depth_high = depth_high * keep + float(frame_high) * (1.0 - keep)
                if depth_high <= depth_low:
                    depth = np.zeros_like(raw_depth, dtype=np.float32)
                else:
                    depth = np.clip(
                        (raw_depth - depth_low) / (depth_high - depth_low),
                        0.0,
                        1.0,
                    ).astype(np.float32)

                small_rgb = cv2.resize(
                    rgb, (flow_width, flow_height), interpolation=cv2.INTER_AREA
                )
                small_bgr = cv2.cvtColor(small_rgb, cv2.COLOR_RGB2BGR)
                abgr = np.empty((flow_height, flow_width, 4), dtype=np.uint8)
                abgr[:, :, 0] = 255
                abgr[:, :, 1:] = small_bgr
                current_flow_frame = torch.from_numpy(abgr).to(device)
                motion = np.zeros((args.height, args.width, 2), dtype=np.float32)
                if previous_flow_frame is not None:
                    flow = nvof.calculate(
                        current_flow_frame,
                        current_flow_frame if reset else previous_flow_frame,
                        reset,
                    ).cpu().numpy()
                    if not reset:
                        motion = cv2.resize(
                            flow,
                            (args.width, args.height),
                            interpolation=cv2.INTER_LINEAR,
                        )
                        motion[:, :, 0] *= args.width / flow_width
                        motion[:, :, 1] *= args.height / flow_height

                if not reset and previous_depth is not None and args.depth_history > 0:
                    grid_x, grid_y = np.meshgrid(
                        np.arange(args.width, dtype=np.float32),
                        np.arange(args.height, dtype=np.float32),
                    )
                    warped = cv2.remap(
                        previous_depth,
                        grid_x + motion[:, :, 0],
                        grid_y + motion[:, :, 1],
                        cv2.INTER_LINEAR,
                        borderMode=cv2.BORDER_REPLICATE,
                    )
                    depth = (
                        depth * (1.0 - args.depth_history)
                        + warped * args.depth_history
                    ).astype(np.float32)

                while get_u32(mapping, OUTPUT_STATE) != 0:
                    time.sleep(0.0005)
                set_u32(mapping, OUTPUT_STATE, 1)
                guide_offset = HEADER.size + frame_bytes
                mapping[guide_offset : guide_offset + pixels * 4] = (
                    np.ascontiguousarray(depth, dtype="<f4").tobytes()
                )
                mapping[
                    guide_offset + pixels * 4 : guide_offset + guide_bytes
                ] = np.ascontiguousarray(motion, dtype="<f2").tobytes()
                generated += 1
                processing_us = round((time.perf_counter() - started) * 1_000_000)
                set_u64(mapping, OUTPUT_SEQUENCE, sequence)
                set_u32(mapping, RESET, int(reset))
                set_u32(mapping, PROCESSING_US, processing_us)
                set_u64(mapping, GENERATED_COUNT, generated)
                set_u32(mapping, OUTPUT_STATE, 2)

                previous_gray = gray
                previous_depth = depth
                previous_flow_frame = current_flow_frame
                previous_sequence = sequence
                print(
                    f"guide={generated} source={sequence} "
                    f"time={processing_us / 1000:.1f}ms reset={int(reset)}",
                    flush=True,
                )
    except KeyboardInterrupt:
        pass
    finally:
        if nvof is not None:
            nvof.close()
        control.close()
        mapping.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

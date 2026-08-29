#!/usr/bin/env python3
"""Estimate temporally smoothed depth and dense motion for decoded video frames."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import cv2
import numpy as np
import torch
from PIL import Image
from transformers import AutoImageProcessor, AutoModelForDepthEstimation


MODEL_ID = "depth-anything/Depth-Anything-V2-Small-hf"
GUIDE_HEADER = struct.Struct("<4sIIII")


def write_guide(path: Path, values: np.ndarray, reset: bool) -> None:
    if values.ndim == 2:
        values = values[:, :, None]
    height, width, channels = values.shape
    contiguous = np.ascontiguousarray(values, dtype="<f4")
    with path.open("wb") as stream:
        stream.write(GUIDE_HEADER.pack(b"D5G1", width, height, channels, int(reset)))
        stream.write(contiguous.tobytes())


def estimated_depth(
    image_rgb: np.ndarray,
    processor: AutoImageProcessor,
    model: AutoModelForDepthEstimation,
    device: torch.device,
) -> np.ndarray:
    inputs = processor(images=Image.fromarray(image_rgb), return_tensors="pt")
    inputs = {name: value.to(device) for name, value in inputs.items()}
    with torch.inference_mode():
        prediction = model(**inputs).predicted_depth.unsqueeze(1)
        prediction = torch.nn.functional.interpolate(
            prediction,
            size=image_rgb.shape[:2],
            mode="bicubic",
            align_corners=False,
        ).squeeze()
    return prediction.float().cpu().numpy().astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--depth-dir", required=True, type=Path)
    parser.add_argument("--motion-dir", required=True, type=Path)
    parser.add_argument("--model-cache", required=True, type=Path)
    parser.add_argument("--scene-cut-threshold", type=float, default=0.20)
    parser.add_argument("--depth-history", type=float, default=0.25)
    parser.add_argument("--depth-range-history", type=float, default=0.95)
    parser.add_argument(
        "--motion-backend", choices=("nvof", "dis"), default="nvof"
    )
    parser.add_argument("--nvof-scale", type=float, default=0.5)
    args = parser.parse_args()

    frames = sorted(args.input_dir.glob("*.ppm"))
    if not frames:
        raise SystemExit("No PPM frames were found.")
    args.depth_dir.mkdir(parents=True, exist_ok=True)
    args.motion_dir.mkdir(parents=True, exist_ok=True)
    args.model_cache.mkdir(parents=True, exist_ok=True)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    dtype = torch.float16 if device.type == "cuda" else torch.float32
    processor = AutoImageProcessor.from_pretrained(
        MODEL_ID, cache_dir=args.model_cache, use_fast=False
    )
    model = AutoModelForDepthEstimation.from_pretrained(
        MODEL_ID, cache_dir=args.model_cache, dtype=dtype
    ).to(device).eval()
    optical_flow = None
    nvof = None
    if args.motion_backend == "nvof":
        if device.type != "cuda":
            raise SystemExit("NVIDIA Optical Flow requires a CUDA device.")
        from NvidiaOpticalFlow import NvidiaOpticalFlow

        sample = cv2.imread(str(frames[0]), cv2.IMREAD_COLOR)
        if sample is None:
            raise RuntimeError(f"Could not read {frames[0]}")
        flow_width = max(32, round(sample.shape[1] * args.nvof_scale))
        flow_height = max(16, round(sample.shape[0] * args.nvof_scale))
        nvof = NvidiaOpticalFlow(flow_width, flow_height)
        print(
            f"NVIDIA Optical Flow: Fast, {flow_width}x{flow_height}, "
            "temporal hints enabled"
        )
    else:
        optical_flow = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
        optical_flow.setUseSpatialPropagation(True)

    previous_gray: np.ndarray | None = None
    previous_depth: np.ndarray | None = None
    previous_flow_frame: torch.Tensor | None = None
    depth_low: float | None = None
    depth_high: float | None = None
    try:
        for index, frame_path in enumerate(frames):
            bgr = cv2.imread(str(frame_path), cv2.IMREAD_COLOR)
            if bgr is None:
                raise RuntimeError(f"Could not read {frame_path}")
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)

            reset = index == 0
            if previous_gray is not None:
                difference = float(np.mean(cv2.absdiff(gray, previous_gray))) / 255.0
                reset = difference >= args.scene_cut_threshold

            raw_depth = estimated_depth(rgb, processor, model, device)
            frame_low, frame_high = np.percentile(raw_depth, (2.0, 98.0))
            if reset or depth_low is None or depth_high is None:
                depth_low, depth_high = float(frame_low), float(frame_high)
            else:
                keep = args.depth_range_history
                depth_low = depth_low * keep + float(frame_low) * (1.0 - keep)
                depth_high = depth_high * keep + float(frame_high) * (1.0 - keep)
            if depth_high <= depth_low:
                current_depth = np.zeros_like(raw_depth, dtype=np.float32)
            else:
                # Depth Anything produces relative inverse depth. Keep its
                # normalisation stable between adjacent video frames.
                current_depth = np.clip(
                    (raw_depth - depth_low) / (depth_high - depth_low), 0.0, 1.0
                ).astype(np.float32)

            motion = np.zeros((*gray.shape, 2), dtype=np.float32)
            current_flow_frame = None
            if nvof is not None:
                small = cv2.resize(
                    bgr, (flow_width, flow_height), interpolation=cv2.INTER_AREA
                )
                abgr = np.empty((flow_height, flow_width, 4), dtype=np.uint8)
                abgr[:, :, 0] = 255
                abgr[:, :, 1:] = small
                current_flow_frame = torch.from_numpy(abgr).to(device)
                if previous_flow_frame is not None:
                    half_flow = nvof.calculate(
                        current_flow_frame,
                        current_flow_frame if reset else previous_flow_frame,
                        reset,
                    ).cpu().numpy()
                    if not reset:
                        motion = cv2.resize(
                            half_flow,
                            (gray.shape[1], gray.shape[0]),
                            interpolation=cv2.INTER_LINEAR,
                        )
                        motion[:, :, 0] *= gray.shape[1] / flow_width
                        motion[:, :, 1] *= gray.shape[0] / flow_height
            elif previous_gray is not None and not reset:
                # DIS(current, previous) produces current-to-previous vectors.
                motion = optical_flow.calc(gray, previous_gray, None).astype(np.float32)

            if not reset and previous_depth is not None and args.depth_history > 0.0:
                grid_x, grid_y = np.meshgrid(
                    np.arange(gray.shape[1], dtype=np.float32),
                    np.arange(gray.shape[0], dtype=np.float32),
                )
                warped_previous = cv2.remap(
                    previous_depth,
                    grid_x + motion[:, :, 0],
                    grid_y + motion[:, :, 1],
                    cv2.INTER_LINEAR,
                    borderMode=cv2.BORDER_REPLICATE,
                )
                current_depth = (
                    current_depth * (1.0 - args.depth_history)
                    + warped_previous * args.depth_history
                ).astype(np.float32)

            guide_name = frame_path.with_suffix(".d5g").name
            write_guide(args.depth_dir / guide_name, current_depth, reset)
            write_guide(args.motion_dir / guide_name, motion, reset)
            previous_gray = gray
            previous_depth = current_depth
            previous_flow_frame = current_flow_frame
            print(
                f"Guides {index + 1}/{len(frames)}: "
                f"{frame_path.name} reset={int(reset)}"
            )
    finally:
        if nvof is not None:
            nvof.close()

    print(f"Guide estimation complete on {device.type}: {len(frames)} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

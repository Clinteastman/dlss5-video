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


def normalized_depth(
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
    depth = prediction.float().cpu().numpy()
    low, high = np.percentile(depth, (2.0, 98.0))
    if high <= low:
        return np.zeros_like(depth, dtype=np.float32)
    # Depth Anything produces relative inverse depth. The DLSS feature is
    # created with its inverted-depth flag, so nearby surfaces remain near 1.
    return np.clip((depth - low) / (high - low), 0.0, 1.0).astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--depth-dir", required=True, type=Path)
    parser.add_argument("--motion-dir", required=True, type=Path)
    parser.add_argument("--model-cache", required=True, type=Path)
    parser.add_argument("--scene-cut-threshold", type=float, default=0.20)
    parser.add_argument("--depth-history", type=float, default=0.25)
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
    optical_flow = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    optical_flow.setUseSpatialPropagation(True)

    previous_gray: np.ndarray | None = None
    previous_depth: np.ndarray | None = None
    for index, frame_path in enumerate(frames):
        bgr = cv2.imread(str(frame_path), cv2.IMREAD_COLOR)
        if bgr is None:
            raise RuntimeError(f"Could not read {frame_path}")
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
        current_depth = normalized_depth(rgb, processor, model, device)

        reset = index == 0
        motion = np.zeros((*gray.shape, 2), dtype=np.float32)
        if previous_gray is not None:
            difference = float(np.mean(cv2.absdiff(gray, previous_gray))) / 255.0
            reset = difference >= args.scene_cut_threshold
            if not reset:
                # DIS(current, previous) gives the current-to-previous vector
                # that temporal reconstruction expects for each current pixel.
                motion = optical_flow.calc(gray, previous_gray, None).astype(np.float32)
                if previous_depth is not None and args.depth_history > 0.0:
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
        print(f"Guides {index + 1}/{len(frames)}: {frame_path.name} reset={int(reset)}")

    print(f"Guide estimation complete on {device.type}: {len(frames)} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

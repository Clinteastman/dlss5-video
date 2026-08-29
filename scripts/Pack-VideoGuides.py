#!/usr/bin/env python3
"""Pack D5G depth and motion frames into a memory-mappable live guide stream."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import numpy as np


GUIDE_HEADER = struct.Struct("<4sIIII")
PACK_HEADER = struct.Struct("<4sIIIIIIQ")


def read_guide(path: Path, expected_channels: int) -> tuple[np.ndarray, bool]:
    with path.open("rb") as stream:
        header = stream.read(GUIDE_HEADER.size)
        magic, width, height, channels, reset = GUIDE_HEADER.unpack(header)
        if magic != b"D5G1" or channels != expected_channels:
            raise ValueError(f"Invalid guide: {path}")
        values = np.frombuffer(stream.read(), dtype="<f4")
    expected = width * height * channels
    if values.size != expected:
        raise ValueError(f"Wrong payload size in {path}")
    return values.reshape(height, width, channels), bool(reset)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--depth-dir", required=True, type=Path)
    parser.add_argument("--motion-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--fps-num", type=int, default=30000)
    parser.add_argument("--fps-den", type=int, default=1001)
    args = parser.parse_args()

    depth_files = sorted(args.depth_dir.glob("*.d5g"))
    motion_files = sorted(args.motion_dir.glob("*.d5g"))
    if not depth_files or len(depth_files) != len(motion_files):
        raise SystemExit("Depth and motion guide counts do not match.")

    first_depth, _ = read_guide(depth_files[0], 1)
    height, width, _ = first_depth.shape
    pixels = width * height
    # Per frame: reset flag, R32 depth, then R16G16 motion.
    frame_stride = 4 + pixels * 4 + pixels * 4
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as stream:
        stream.write(PACK_HEADER.pack(
            b"D5GP", 1, width, height, len(depth_files),
            args.fps_num, args.fps_den, frame_stride
        ))
        for index, (depth_path, motion_path) in enumerate(
            zip(depth_files, motion_files, strict=True), start=1
        ):
            depth, depth_reset = read_guide(depth_path, 1)
            motion, motion_reset = read_guide(motion_path, 2)
            if depth.shape[:2] != (height, width) or motion.shape[:2] != (height, width):
                raise ValueError(f"Guide dimensions changed at frame {index}")
            stream.write(struct.pack("<I", int(depth_reset or motion_reset)))
            stream.write(np.ascontiguousarray(depth[:, :, 0], dtype="<f4").tobytes())
            stream.write(np.ascontiguousarray(motion, dtype="<f2").tobytes())
            print(f"Packed {index}/{len(depth_files)}")

    print(f"Guide pack complete: {args.output} ({width}x{height}, {len(depth_files)} frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

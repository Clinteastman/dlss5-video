# Architecture

## Target pipeline

```text
mpv / D3D11 presentation
          |
          v
 frame acquisition + 10-bit conversion
            |
            v
 shared-memory latest-frame slot
      |                         |
      v                         v
 Depth Anything V2       NVIDIA Optical Flow
      |                         |
      +------------+------------+
                   v
       shared-memory guide slot
            v
 D3D11 -> shared D3D12 resources
            |
            v
      NGX evaluation
            |
            v
  D3D12 -> D3D11 result
            |
            v
 display + overlay + A/B bypass
```

## Why mpv first

mpv already solves codecs, audio/video synchronisation, seeking, subtitles,
colour metadata, HDR handling, windowing, and NVIDIA VSR. Replacing all of that
would delay the only genuinely new part of this project.

## Components

### Playback baseline

- mpv with D3D11 output.
- Explicit NVIDIA VSR on/off control.
- Reproducible test clip and frame captures.

### Frame acquisition and presentation

- The ReShade add-on captures mpv's clean D3D11 backbuffer before UI composition.
- A single-slot asynchronous bridge drops stale capture opportunities rather
  than blocking playback behind depth inference.
- R8G8B8A8 and R10G10B10A2 mpv surfaces are converted to RGB8 for the companion.
- Bypass path must remain available if any experimental stage fails.

### Depth

- Depth Anything V2 Small currently runs through PyTorch CUDA in a companion
  process, at 392x392 by default, then returns a 960x540 guide.
- Motion-compensated temporal smoothing and stable percentile ranges reduce
  frame-to-frame depth flicker.
- TensorRT and direct GPU texture sharing remain performance upgrades.

### Motion

- Prefer NVIDIA Optical Flow for low-latency dense motion vectors.
- The live path uses the Fast preset, half guide resolution, a 1x1 output grid,
  and temporal hints.
- Detect scene cuts and reset temporal state.
- Provide a debug view for vector direction and scale.

### DLSS Neural Rendering

- Adapt the resource sharing and genuine D3D12 NGX evaluation pattern demonstrated
  by dlss5-dx11-bridge.
- Never ship proprietary NVIDIA runtime files.
- Validate each supplied input independently before enabling the full pass.

## Test order

1. Plain mpv output.
2. NVIDIA VSR only.
3. Frame interception with a no-op copy.
4. Depth visualisation.
5. Optical-flow visualisation.
6. NGX transport without Neural Rendering.
7. Neural Rendering at native size.
8. Persistent multi-frame Neural Rendering session.
9. Neural Rendering plus upscaling.
10. VSR coexistence and pass-order tests.

# Architecture

## Target pipeline

```text
mpv / D3D11 presentation
          |
          v
    frame acquisition
      |           |
      v           v
 depth model   optical flow
      |           |
      +-----+-----+
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

- ReShade add-on or a small mpv/libplacebo integration, chosen after the baseline
  confirms which surface contains the video before UI composition.
- Bypass path must remain available if any experimental stage fails.

### Depth

- Begin with Depth Anything V2 Small through ONNX Runtime / DirectML or TensorRT.
- Run below source resolution initially and resize on the GPU.
- Add temporal smoothing because single-frame depth will otherwise flicker.

### Motion

- Prefer NVIDIA Optical Flow for low-latency dense motion vectors.
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

# Existing-project survey

Checked 29 August 2026.

See [nvidia-vfx-findings.md](nvidia-vfx-findings.md) for the official ComfyUI
RTX node audit. It offers a useful CUDA/DLPack frame boundary, but its installed
API is VSR-only; Neural Rendering remains in the supplied Streamline runtime.

## Finding

There is no public project we found that combines live video playback, generated
depth, optical flow, DLSS 5 Neural Rendering, and a user control panel.

Several projects solve substantial parts of it, so those parts should not be
rewritten.

## Projects

### mpv

mpv already exposes NVIDIA RTX Super Resolution through its D3D11 video
processor. It is the best first playback host because it is open, mature, and
scriptable. It also gives us a clean baseline before experimental DLSS work.

Upstream: https://github.com/mpv-player/mpv

### LocalVSR

LocalVSR uses the same D3D11 driver extension used by Chromium and VLC. It has a
careful mechanism for changing the global VSR quality level and restoring the
user's previous setting. It exports files rather than processing live playback,
and its optional frame interpolation is RIFE rather than DLSS Frame Generation.

Upstream: https://github.com/vyomanaut/local-video-upscaler

### RTXVideoProcessor

RTXVideoProcessor connects FFmpeg, CUDA, NVDEC/NVENC, and NVIDIA's RTX Video SDK.
It is useful reference code for offline processing and a future export mode. It
implements RTX VSR and TrueHDR, not DLSS Neural Rendering.

Upstream: https://github.com/DrC0ns0le/RTXVideoProcessor

### dlss5-dx11-bridge

This is the closest technical foundation. It transfers colour, motion vectors,
and depth from D3D11 to a D3D12 NGX session, performs a genuine NGX evaluation,
and copies the result back. Its current input contract comes from a game already
running DLSS. A video host must create that contract and provide its own estimated
motion and depth.

Upstream: https://github.com/NIGos/dlss5-dx11-bridge

### ReShade and RenoDX

These provide mature DirectX interception and overlay infrastructure. They are
useful hosts, but neither currently provides the complete public video pipeline.

Upstream:

- https://github.com/crosire/reshade
- https://github.com/clshortfuse/renodx

## Decision

Create a separate integration repository. Start with mpv rather than writing a
player. Reuse or adapt MIT-licensed bridge code with attribution. Keep NVIDIA
runtime files user-supplied and outside version control.

## Local validation

The installed mpv 0.41 build exposes `d3d11vpp` with NVIDIA scaling. A 640x360
test frame was processed to a 1280x720 D3D11 output successfully.

The supplied Streamline 2.13 package contains `sl.dlss_nr.dll` and
`nvngx_dlssnr.dll` 310.8.0. The Neural Rendering runtime matches the separate
RTX 4000-compatible DLL already tested in a game. Its Authenticode status is a
hash mismatch, consistent with that file having been patched; it must therefore
remain user-supplied and must not be redistributed by this project.

The supplied ReShade add-on identifies Neural Rendering as NGX feature 18. In a
private synthetic D3D12 test it hooked a normal DLAA evaluation, initialized the
310.8.0 Neural Rendering runtime, created feature 18 at 640x360, and completed
the first evaluation successfully on an RTX 4090. The driver capability table
still reports `SuperSamplingDenoising.Available = 0`; the add-on's direct feature
path is therefore essential on this GPU.

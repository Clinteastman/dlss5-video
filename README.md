# DLSS 5 Video Lab

An experimental Windows project for applying NVIDIA video enhancement and
DLSS Neural Rendering to ordinary video, with visible controls and useful
diagnostics.

The project is starting as an integration lab, not a new media player. Existing
open-source software already handles playback, decoding, NVIDIA RTX Video Super
Resolution (VSR), and parts of the NGX bridge. This repository will concentrate
on the missing pipeline:

```text
video frame -> depth estimate + optical flow -> DLSS Neural Rendering -> display
```

## Planned milestones

1. Establish an mpv playback baseline with NVIDIA VSR and an A/B toggle.
2. Capture the displayed D3D11 video frame without modifying protected content.
3. Adapt the existing D3D11-to-D3D12 NGX bridge to make a controlled DLSS call.
4. Supply estimated depth and optical flow.
5. Add a small overlay for style, strength, diagnostics, and bypass comparison.

## Existing work we intend to reuse

- [mpv](https://github.com/mpv-player/mpv) for mature playback and its existing
  NVIDIA VSR path.
- [LocalVSR](https://github.com/vyomanaut/local-video-upscaler) as a reference
  for the D3D11 VSR extension and safe NVIDIA quality-setting restoration.
- [RTXVideoProcessor](https://github.com/DrC0ns0le/RTXVideoProcessor) as a
  reference for NVIDIA RTX Video SDK and GPU video-processing pipelines.
- [dlss5-dx11-bridge](https://github.com/NIGos/dlss5-dx11-bridge) as the closest
  working reference for the D3D11/D3D12 resource bridge and synthetic NGX call.
- [ReShade](https://github.com/crosire/reshade) for injection, presentation,
  and the user overlay where appropriate.

See [docs/research.md](docs/research.md) and
[docs/architecture.md](docs/architecture.md) for the reasoning.

## RTX Video Super Resolution

Turn NVIDIA RTX Video Super Resolution off for the first DLSS Neural Rendering
tests. Otherwise two enhancement passes make image comparisons misleading.
Once the DLSS path works, coexistence and pass ordering can be tested deliberately.

The first baseline can be launched without altering the normal mpv configuration:

```powershell
.\scripts\Start-VideoLab.ps1 'C:\path\to\video.mkv'
```

Press `Ctrl+V` for RTX VSR on/off and `Ctrl+B` to clear video filters.

To inspect a user-supplied Streamline ZIP without adding its DLLs to Git:

```powershell
.\scripts\Inspect-StreamlinePackage.ps1 'C:\path\to\streamline.zip'
```

The native capability probe can be built with Visual Studio and pointed at an
extracted, user-supplied Streamline runtime directory:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\Release\ngx-capability-probe.exe 'C:\path\to\streamline'
```

For the private Neural Rendering test, prepare a Git-ignored harness from a
user-supplied Streamline ZIP, ReShade DLL, and Neural Rendering add-on:

```powershell
.\scripts\Prepare-PrivateHarness.ps1 `
  -StreamlineZip 'C:\path\to\streamline.zip' `
  -ReShadeDll 'C:\path\to\dxgi.dll' `
  -NeuralAddon 'C:\path\to\renodx-dlss5.addon64'

.\scripts\Invoke-NeuralProbe.ps1
```

Once that succeeds, process one decoded frame from a normal, unprotected video:

```powershell
.\scripts\Invoke-NeuralFrame.ps1 `
  -Video 'C:\path\to\video.mp4' `
  -Timestamp '00:00:02' `
  -Output 'C:\path\to\processed-frame.png'
```

Process a short clip while keeping one NGX/ReShade session alive across every
frame (currently an offline proof, not real-time playback):

```powershell
.\scripts\Invoke-NeuralClip.ps1 `
  -Video 'C:\path\to\video.mp4' `
  -Start '00:00:10' `
  -Duration 3 `
  -FrameRate 12 `
  -Output 'C:\path\to\processed-clip.mp4'
```

## Legal and safety boundaries

- No NVIDIA DLLs, SDK files, model weights, or third-party binaries are stored
  in this repository.
- Users must obtain proprietary NVIDIA components from an authorised source.
- The project will not attempt to capture DRM-protected or protected video
  surfaces.
- This project is not affiliated with or endorsed by NVIDIA.

## Status

The project-local mpv launcher and RTX VSR A/B baseline are working. Synthetic
textures, individual decoded frames, and complete short frame sequences have
been intercepted, processed by hidden NGX feature 18, and read back successfully
on an RTX 4090 using the user-supplied patched 310.8.0 runtime. A sequence now
keeps one D3D12 device, NGX feature, ReShade instance, and swapchain alive across
all frames; only the first frame resets temporal state. Depth and motion remain
deterministic zero guides. The next milestone is replacing those placeholders
with estimated depth and optical flow before connecting the persistent session
directly to mpv playback.

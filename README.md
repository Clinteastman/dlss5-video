# DLSS 5 Video Lab

> [!WARNING]
> **Experimental software — under active development.** This is a working
> research prototype, not a finished video player or production-ready filter.
> It requires local setup and user-supplied third-party components; output
> quality varies, some setting combinations may stall, and interfaces may
> change without notice.

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

## Development checkpoints

- [x] Establish an mpv playback baseline with NVIDIA VSR and an A/B toggle.
- [x] Capture the displayed D3D11 video frame without modifying protected
  content.
- [x] Adapt the D3D11-to-D3D12 NGX bridge for a controlled DLSS call.
- [x] Supply real-time estimated depth and NVIDIA Optical Flow.
- [x] Add a ReShade overlay for quality controls, diagnostics, guide views, and
  bypass comparison.
- [ ] Improve temporal guide alignment, colour handling, stability, and
  performance until moving output approaches the frozen-frame quality.

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
  -Scale 2 `
  -Output 'C:\path\to\processed-clip.mp4'
```

`-Scale 1` keeps the source dimensions and uses the native-size/DLAA path.
Values above 1 request a larger DLSS Super Resolution output; for example,
960x540 with `-Scale 2` produces 1920x1080 frames.

## Experimental live mpv renderer

The repository now contains a ReShade add-on that processes mpv's live D3D11
backbuffer through a persistent D3D12 NGX session. It keeps presenting while a
source frame is frozen, so the ReShade overlay remains interactive and Neural
Rendering controls can be compared on exactly the same image.

Build it against a local ReShade source checkout:

```powershell
cmake -S . -B build -A x64 `
  -DRESHADE_SOURCE_DIR='C:\path\to\reshade'
cmake --build build --config Release --target dlss5-video-renderer
```

The resulting `dlss5-video-renderer.addon64` must be loaded by a separately
prepared, Git-ignored ReShade/mpv test harness. No ReShade, NVIDIA, RenoDX, or
patched runtime binaries are redistributed here.

The add-on currently provides:

- live Neural Rendering output or an original/bypass view;
- an interactive frozen-frame comparison without stopping presentation;
- independent estimated-depth and optical-flow controls;
- depth-direction and blank-guide A/B diagnostics;
- depth and motion-vector preview views;
- real-time Depth Anything and NVIDIA Optical Flow guides delivered by a
  non-blocking companion process;
- live ReShade controls for depth detail, optical-flow resolution, and NVIDIA
  Optical Flow quality/performance mode, with measured guide time;
- direct local-file and YouTube URL playback through the live launcher;
- live window-resize rebuilding, with the guide resolution as the NGX input and
  a larger window as the genuine NGX output; and
- explicit NGX evaluation, guide-frame, and GPU-resource binding status.

Start the real-time path after preparing the private harness and Python CUDA
dependencies:

```powershell
.\scripts\Start-LiveVideo.ps1 'C:\path\to\video.mp4'
.\scripts\Start-LiveVideo.ps1 'https://www.youtube.com/watch?v=VIDEO_ID'
```

The default high-quality pipeline produces 1280x720 guides, runs the depth model
internally at 518x518, runs NVIDIA Optical Flow at 1280x720 in Fast mode, and
asks NGX for a 1920x1080 output. The ReShade panel can switch depth between 280,
392, and 518 pixels, flow between quarter, half, and full resolution, and flow
mode between Fast, Balanced, and Quality while the video keeps playing. It also
shows the current guide-processing time. `-InputWidth`, `-InputHeight`,
`-DepthSize`, `-WindowWidth`, and `-WindowHeight` expose the launch-time
trade-offs. Quality flow mode is experimental because full-resolution NVOF and
NGX can stall one another on the current driver. Use `-Loop` for short test
clips.

Precompute guides from decoded image frames, then pack them into the stream read
by the live add-on:

```powershell
python .\scripts\Estimate-VideoGuides.py `
  --input-dir C:\path\to\frames `
  --depth-dir C:\path\to\depth `
  --motion-dir C:\path\to\motion `
  --model-cache C:\path\to\model-cache

python .\scripts\Pack-VideoGuides.py `
  --depth-dir C:\path\to\depth `
  --motion-dir C:\path\to\motion `
  --output C:\path\to\guides.d5gp

$env:DLSS5_VIDEO_GUIDE_PACK = 'C:\path\to\guides.d5gp'
```

`Estimate-VideoGuides.py` currently uses Depth Anything V2 Small and NVIDIA
Optical Flow at half resolution, with the fast preset and temporal hints. OpenCV
DIS remains available as a fallback with `--motion-backend dis`. Model files are
downloaded to the caller-supplied cache and must not be committed.

## Legal and safety boundaries

- No NVIDIA DLLs, SDK files, model weights, or third-party binaries are stored
  in this repository.
- Users must obtain proprietary NVIDIA components from an authorised source.
- The project will not attempt to capture DRM-protected or protected video
  surfaces.
- This project is not affiliated with or endorsed by NVIDIA.

## Status

The project-local mpv launcher and RTX VSR A/B baseline work. Synthetic textures,
decoded frames, offline clips, and live mpv frames have been processed by hidden
NGX feature 18 and read back successfully on an RTX 4090 with a user-supplied
patched 310.8.0 runtime. The live path keeps one D3D12 device and NGX feature
active, synchronises its D3D11/D3D12 copies, and continuously evaluates at the
source frame rate while guide generation runs asynchronously. A shared-memory
companion calculates depth and NVIDIA Optical Flow from the frames being
presented, so arbitrary local files and YouTube videos no longer depend on a
precomputed guide pack. On the RTX 4090 test machine, the current high-quality
default of 518x518 depth and full 1280x720 Fast optical flow usually takes about
60-75 ms per new guide, with occasional larger spikes, while mpv keeps
presenting. Resizing above the guide resolution rebuilds the NGX feature with
separate input and output sizes, so NGX generates the larger image instead of
the player stretching a native-size result.

This is still an experimental checkpoint, not a finished video filter. The
current output can show grey or shadowed faces and an under-converged, textured
or rippling appearance. Frozen-frame motion is now suppressed, which removes a
large repeated-warp instability, but the remaining quality does not yet match
the reference OBS prototype. The same user-supplied runtime gives clean results
in games on the test machine, which points to our guide and colour inputs rather
than the runtime itself. The guide path now matches the reference's NVIDIA
Optical Flow temporal-hint approach, with selectable quarter, half, and full
resolutions and Fast, Balanced, and Quality modes. The largest remaining
differences are temporally stable video depth, colour/exposure handling,
guide-to-video latency when inference exceeds one presentation interval, and
source material. The next milestone is GPU-shared frame transport and
delayed-frame alignment, followed by controlled colour-contract testing.

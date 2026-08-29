# NVIDIA VFX / ComfyUI findings

The official ComfyUI RTX node is useful for frame transport, but it is not the
DLSS Neural Rendering implementation.

## What it contains

- The node calls the proprietary `nvidia-vfx` Python wheel.
- The wheel exposes only `VideoSuperRes` in version 0.1.0.1 / VFX SDK 1.2.0.
- Frames enter and leave as CUDA tensors through DLPack, avoiding a CPU copy.
- Input is RGB float32 in channels-first layout, with values from 0 to 1.
- The returned DLPack image must be cloned before the next effect call.
- VSR includes ordinary upscale, same-resolution denoise/deblur, and clean-source
  high-bitrate modes. The ComfyUI node currently exposes only the four ordinary
  quality levels.

## What it does not contain

- `nvngx_dlssnr.dll`
- the hidden NGX feature-18 interface
- depth estimation
- optical-flow generation

Neural Rendering still comes from the user-supplied patched Streamline package.
The reusable lesson is the GPU-resident frame boundary: a future decoder or
depth model can hand frames around as DLPack/CUDA data until they are shared with
the D3D12 NGX stage.

The Python wheel and its bundled NVIDIA binaries are proprietary and must not be
copied into this repository.

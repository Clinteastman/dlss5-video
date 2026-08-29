#!/usr/bin/env python3
"""Small Windows/PyTorch wrapper around NVIDIA's public CUDA Optical Flow API."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path

import torch


API_VERSION = (2 << 4) | 0
SUCCESS = 0
CUDA_DEVICE_TO_DEVICE = 3


class InitParams(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("out_grid_size", ctypes.c_int),
        ("hint_grid_size", ctypes.c_int),
        ("mode", ctypes.c_int),
        ("perf_level", ctypes.c_int),
        ("enable_external_hints", ctypes.c_int),
        ("enable_output_cost", ctypes.c_int),
        ("private_data", ctypes.c_void_p),
        ("disparity_range", ctypes.c_int),
        ("enable_roi", ctypes.c_int),
    ]


class BufferDescriptor(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("usage", ctypes.c_int),
        ("format", ctypes.c_int),
    ]


class ExecuteInput(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("input_frame", ctypes.c_void_p),
        ("reference_frame", ctypes.c_void_p),
        ("external_hints", ctypes.c_void_p),
        ("disable_temporal_hints", ctypes.c_int),
        ("padding", ctypes.c_uint32),
        ("private_data", ctypes.c_void_p),
        ("padding2", ctypes.c_uint32),
        ("num_rois", ctypes.c_uint32),
        ("roi_data", ctypes.c_void_p),
    ]


class ExecuteOutput(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("output_buffer", ctypes.c_void_p),
        ("output_cost_buffer", ctypes.c_void_p),
        ("private_data", ctypes.c_void_p),
    ]


class BufferStride(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("stride_x_bytes", ctypes.c_uint32),
        ("stride_y_bytes", ctypes.c_uint32),
    ]


class BufferStrideInfo(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("planes", BufferStride * 3),
        ("num_planes", ctypes.c_uint32),
    ]


CALL = getattr(ctypes, "WINFUNCTYPE", ctypes.CFUNCTYPE)
CreateSession = CALL(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
Initialize = CALL(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(InitParams))
CreateBuffer = CALL(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(BufferDescriptor),
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_void_p),
)
GetDevicePointer = CALL(ctypes.c_uint64, ctypes.c_void_p)
GetStride = CALL(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(BufferStrideInfo))
Execute = CALL(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(ExecuteInput),
    ctypes.POINTER(ExecuteOutput),
)
DestroyBuffer = CALL(ctypes.c_int, ctypes.c_void_p)
DestroySession = CALL(ctypes.c_int, ctypes.c_void_p)
GetCaps = CALL(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_uint32),
    ctypes.POINTER(ctypes.c_uint32),
)


class FunctionList(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("create_session", CreateSession),
        ("initialize", Initialize),
        ("create_buffer", CreateBuffer),
        ("get_array", ctypes.c_void_p),
        ("get_device_pointer", GetDevicePointer),
        ("get_stride", GetStride),
        ("set_streams", ctypes.c_void_p),
        ("execute", Execute),
        ("destroy_buffer", DestroyBuffer),
        ("destroy_session", DestroySession),
        ("get_last_error", ctypes.c_void_p),
        ("get_caps", GetCaps),
    ]


def _cuda_runtime() -> ctypes.WinDLL:
    candidates: list[Path] = []
    cuda_path = os.environ.get("CUDA_PATH")
    if cuda_path:
        candidates.extend(Path(cuda_path, "bin").glob("cudart64_*.dll"))
    candidates.extend(Path(torch.__file__).parent.joinpath("lib").glob("cudart64_*.dll"))
    if not candidates:
        raise RuntimeError("CUDA runtime DLL was not found.")
    return ctypes.WinDLL(str(sorted(candidates)[-1]))


def _check(result: int, operation: str) -> None:
    if result != SUCCESS:
        raise RuntimeError(f"{operation} failed with NVIDIA Optical Flow status {result}.")


class NvidiaOpticalFlow:
    """Generate current-to-reference pixel vectors using NVOFA temporal hints."""

    INPUT = 1
    OUTPUT = 2
    ABGR8 = 3
    SHORT2 = 5
    DEVICE_POINTER = 2
    OPTICAL_FLOW = 1
    QUALITY = 5
    BALANCED = 10
    FAST = 20
    CAP_OUTPUT_GRIDS = 0

    def __init__(
        self,
        width: int,
        height: int,
        device: int = 0,
        perf_level: int = FAST,
    ) -> None:
        if os.name != "nt":
            raise RuntimeError("This NVIDIA Optical Flow wrapper currently supports Windows only.")
        self.width = width
        self.height = height
        self.output_width = width
        self.output_height = height
        self.buffers: list[ctypes.c_void_p] = []
        self.session = ctypes.c_void_p()

        torch.cuda.set_device(device)
        torch.empty(1, device=f"cuda:{device}")
        driver = ctypes.WinDLL("nvcuda.dll")
        driver.cuCtxGetCurrent.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        context = ctypes.c_void_p()
        _check(driver.cuCtxGetCurrent(ctypes.byref(context)), "cuCtxGetCurrent")
        if not context:
            raise RuntimeError("PyTorch did not provide a current CUDA context.")

        self.library = ctypes.WinDLL("nvofapi64.dll")
        create_api = self.library.NvOFAPICreateInstanceCuda
        create_api.argtypes = [ctypes.c_uint32, ctypes.POINTER(FunctionList)]
        create_api.restype = ctypes.c_int
        self.api = FunctionList()
        _check(create_api(API_VERSION, ctypes.byref(self.api)), "NvOFAPICreateInstanceCuda")
        _check(self.api.create_session(context, ctypes.byref(self.session)), "create session")

        supported = self._supported_grids()
        if 1 not in supported:
            raise RuntimeError(f"The driver does not support a 1x1 flow grid: {supported}")
        params = InitParams()
        params.width = width
        params.height = height
        params.out_grid_size = 1
        params.mode = self.OPTICAL_FLOW
        params.perf_level = perf_level
        _check(self.api.initialize(self.session, ctypes.byref(params)), "initialize session")

        self.current = self._create_buffer(width, height, self.INPUT, self.ABGR8)
        self.reference = self._create_buffer(width, height, self.INPUT, self.ABGR8)
        self.output = self._create_buffer(width, height, self.OUTPUT, self.SHORT2)
        self.cuda = _cuda_runtime()
        self.cuda.cudaMemcpy2D.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        self.cuda.cudaMemcpy2D.restype = ctypes.c_int

    def _supported_grids(self) -> list[int]:
        count = ctypes.c_uint32()
        _check(
            self.api.get_caps(
                self.session, self.CAP_OUTPUT_GRIDS, None, ctypes.byref(count)
            ),
            "query output grids",
        )
        values = (ctypes.c_uint32 * count.value)()
        _check(
            self.api.get_caps(
                self.session,
                self.CAP_OUTPUT_GRIDS,
                values,
                ctypes.byref(count),
            ),
            "read output grids",
        )
        return list(values[: count.value])

    def _create_buffer(self, width: int, height: int, usage: int, format_: int):
        descriptor = BufferDescriptor(width, height, usage, format_)
        handle = ctypes.c_void_p()
        _check(
            self.api.create_buffer(
                self.session,
                ctypes.byref(descriptor),
                self.DEVICE_POINTER,
                ctypes.byref(handle),
            ),
            "create buffer",
        )
        self.buffers.append(handle)
        return handle

    def _stride(self, buffer) -> int:
        info = BufferStrideInfo()
        _check(self.api.get_stride(buffer, ctypes.byref(info)), "get buffer stride")
        return info.planes[0].stride_x_bytes

    def _copy_tensor_to_buffer(self, tensor: torch.Tensor, buffer) -> None:
        tensor = tensor.contiguous()
        row_bytes = tensor.shape[1] * tensor.shape[2] * tensor.element_size()
        result = self.cuda.cudaMemcpy2D(
            self.api.get_device_pointer(buffer),
            self._stride(buffer),
            tensor.data_ptr(),
            row_bytes,
            row_bytes,
            tensor.shape[0],
            CUDA_DEVICE_TO_DEVICE,
        )
        _check(result, "copy frame to NVOFA")

    def _copy_output(self) -> torch.Tensor:
        result = torch.empty(
            (self.output_height, self.output_width, 2),
            dtype=torch.int16,
            device="cuda",
        )
        row_bytes = result.shape[1] * result.shape[2] * result.element_size()
        status = self.cuda.cudaMemcpy2D(
            result.data_ptr(),
            row_bytes,
            self.api.get_device_pointer(self.output),
            self._stride(self.output),
            row_bytes,
            result.shape[0],
            CUDA_DEVICE_TO_DEVICE,
        )
        _check(status, "copy flow from NVOFA")
        return result

    def calculate(
        self,
        current_abgr: torch.Tensor,
        reference_abgr: torch.Tensor,
        reset_temporal_hints: bool = False,
    ) -> torch.Tensor:
        self._copy_tensor_to_buffer(current_abgr, self.current)
        self._copy_tensor_to_buffer(reference_abgr, self.reference)
        inputs = ExecuteInput()
        inputs.input_frame = self.current
        inputs.reference_frame = self.reference
        inputs.disable_temporal_hints = int(reset_temporal_hints)
        outputs = ExecuteOutput()
        outputs.output_buffer = self.output
        _check(
            self.api.execute(self.session, ctypes.byref(inputs), ctypes.byref(outputs)),
            "calculate optical flow",
        )
        return self._copy_output().to(torch.float32).div_(32.0)

    def close(self) -> None:
        if getattr(self, "api", None):
            for buffer in self.buffers:
                self.api.destroy_buffer(buffer)
            self.buffers.clear()
            if self.session:
                self.api.destroy_session(self.session)
                self.session = ctypes.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

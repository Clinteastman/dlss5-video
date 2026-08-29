// SPDX-License-Identifier: MIT
//
// Small, read-only NGX capability probe. The NGX declaration order and the
// version-negotiation approach are adapted from NIGos/dlss5-dx11-bridge (MIT).

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
using NgxResult = int;
constexpr NgxResult NgxSuccess = 1;
constexpr unsigned NvidiaVendorId = 0x10DE;

// This order mirrors NVIDIA's public nvsdk_ngx.h ABI.
struct NgxParameter
{
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;

    virtual NgxResult Get(const char*, unsigned long long*) const = 0;
    virtual NgxResult Get(const char*, float*) const = 0;
    virtual NgxResult Get(const char*, double*) const = 0;
    virtual NgxResult Get(const char*, unsigned int*) const = 0;
    virtual NgxResult Get(const char*, int*) const = 0;
    virtual NgxResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NgxResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NgxResult Get(const char*, void**) const = 0;

    virtual void Reset() = 0;
};

struct NgxHandle
{
    unsigned int id;
};

using InitExt = NgxResult (*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using InitProjectId = NgxResult (*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using GetCapabilities = NgxResult (*)(NgxParameter**);
using AllocateParameters = NgxResult (*)(NgxParameter**);
using DestroyParameters = NgxResult (*)(NgxParameter*);
using Shutdown = NgxResult (*)(ID3D12Device*);
using CreateFeature = NgxResult (*)(ID3D12GraphicsCommandList*, int, NgxParameter*, NgxHandle**);
using EvaluateFeature = NgxResult (*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameter*, void*);
using ReleaseFeature = NgxResult (*)(NgxHandle*);
constexpr NgxResult NgxException = 0x7FFFFFFF;

struct RgbImage
{
    UINT width = 0;
    UINT height = 0;
    std::vector<unsigned char> pixels;
};

std::optional<std::string> ReadPpmToken(std::istream& stream)
{
    std::string token;
    for (;;)
    {
        stream >> std::ws;
        if (!stream.good()) return std::nullopt;
        if (stream.peek() != '#') break;
        stream.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
    }
    if (!(stream >> token)) return std::nullopt;
    return token;
}

std::optional<RgbImage> LoadPpm(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        std::wcerr << L"Could not open input frame: " << path << L'\n';
        return std::nullopt;
    }

    const auto magic = ReadPpmToken(stream);
    const auto widthToken = ReadPpmToken(stream);
    const auto heightToken = ReadPpmToken(stream);
    const auto maximumToken = ReadPpmToken(stream);
    if (!magic || !widthToken || !heightToken || !maximumToken || *magic != "P6")
    {
        std::cerr << "Input must be a binary PPM (P6) image.\n";
        return std::nullopt;
    }

    RgbImage image;
    try
    {
        const unsigned long width = std::stoul(*widthToken);
        const unsigned long height = std::stoul(*heightToken);
        if (std::stoul(*maximumToken) != 255 || width == 0 || height == 0 ||
            width > 8192 || height > 8192)
            throw std::invalid_argument("unsupported PPM dimensions or range");
        image.width = static_cast<UINT>(width);
        image.height = static_cast<UINT>(height);
    }
    catch (const std::exception&)
    {
        std::cerr << "Invalid PPM header. Only 8-bit P6 images are supported.\n";
        return std::nullopt;
    }

    stream.get(); // consume the single whitespace byte before binary pixels
    const size_t byteCount = static_cast<size_t>(image.width) * image.height * 3;
    image.pixels.resize(byteCount);
    stream.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(byteCount));
    if (stream.gcount() != static_cast<std::streamsize>(byteCount))
    {
        std::cerr << "The PPM pixel data is incomplete.\n";
        return std::nullopt;
    }
    return image;
}

bool SavePpm(const std::filesystem::path& path, const RgbImage& image)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    stream.write(
        reinterpret_cast<const char*>(image.pixels.data()),
        static_cast<std::streamsize>(image.pixels.size()));
    return stream.good();
}

NgxResult SafeInitExt(
    InitExt function,
    unsigned long long applicationId,
    const wchar_t* path,
    ID3D12Device* device,
    int version,
    DWORD* exceptionCode)
{
    *exceptionCode = 0;
    __try
    {
        return function(applicationId, path, device, version, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *exceptionCode = GetExceptionCode();
        return NgxException;
    }
}

NgxResult SafeInitProject(
    InitProjectId function,
    const wchar_t* path,
    ID3D12Device* device,
    int version,
    DWORD* exceptionCode)
{
    *exceptionCode = 0;
    __try
    {
        return function(
            "a0f57b54-1daf-4934-90ae-c4035c19df04",
            0,
            "1.0",
            path,
            device,
            version,
            nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *exceptionCode = GetExceptionCode();
        return NgxException;
    }
}

NgxResult SafeGetCapabilities(
    GetCapabilities function,
    NgxParameter** output,
    DWORD* exceptionCode)
{
    *exceptionCode = 0;
    __try
    {
        return function(output);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *exceptionCode = GetExceptionCode();
        return NgxException;
    }
}

NgxResult SafeCreateFeature(
    CreateFeature function,
    ID3D12GraphicsCommandList* commandList,
    NgxParameter* parameters,
    NgxHandle** handle,
    DWORD* exceptionCode)
{
    *exceptionCode = 0;
    __try
    {
        return function(commandList, 1, parameters, handle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *exceptionCode = GetExceptionCode();
        return NgxException;
    }
}

NgxResult SafeEvaluateFeature(
    EvaluateFeature function,
    ID3D12GraphicsCommandList* commandList,
    const NgxHandle* handle,
    const NgxParameter* parameters,
    DWORD* exceptionCode)
{
    *exceptionCode = 0;
    __try
    {
        return function(commandList, handle, parameters, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *exceptionCode = GetExceptionCode();
        return NgxException;
    }
}

template <typename T>
T Import(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

std::wstring WindowsError(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring message = length != 0 && buffer != nullptr ? std::wstring(buffer, length) : L"unknown error";
    if (buffer != nullptr) LocalFree(buffer);
    return message;
}

std::optional<std::filesystem::path> FindDriverNgxCore()
{
    wchar_t windowsDirectory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory)) == 0) return std::nullopt;

    const auto repository = std::filesystem::path(windowsDirectory) /
        L"System32" / L"DriverStore" / L"FileRepository";
    std::error_code iteratorError;
    for (const auto& entry : std::filesystem::directory_iterator(repository, iteratorError))
    {
        std::error_code entryError;
        if (!entry.is_directory(entryError)) continue;
        const auto name = entry.path().filename().wstring();
        if (!name.starts_with(L"nv_dispi.inf_amd64_")) continue;
        const auto candidate = entry.path() / L"_nvngx.dll";
        if (std::filesystem::is_regular_file(candidate, entryError)) return candidate;
    }
    return std::nullopt;
}

ComPtr<IDXGIAdapter1> FindNvidiaAdapter()
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return {};

    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) continue;

        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) &&
            description.VendorId == NvidiaVendorId &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
        {
            std::wcout << L"GPU: " << description.Description << L'\n';
            return adapter;
        }
    }
    return {};
}

bool LoadRuntimeModule(const std::filesystem::path& runtimeDirectory, const wchar_t* name)
{
    const auto path = runtimeDirectory / name;
    if (!std::filesystem::is_regular_file(path))
    {
        std::wcerr << L"Missing runtime file: " << path << L'\n';
        return false;
    }

    if (LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) == nullptr)
    {
        const DWORD error = GetLastError();
        std::wcerr << L"Could not load " << path << L": " << WindowsError(error) << L'\n';
        return false;
    }
    std::wcout << L"Loaded: " << name << L'\n';
    return true;
}

void PrintCapability(const NgxParameter* capabilities, const char* name)
{
    int value = 0;
    const NgxResult result = capabilities->Get(name, &value);
    std::cout << "  " << std::left << std::setw(42) << name;
    if (result == NgxSuccess) std::cout << value << '\n';
    else std::cout << "unavailable (NGX 0x" << std::hex << result << std::dec << ")\n";
}

ComPtr<ID3D12Resource> CreateTexture(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue = nullptr)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    const HRESULT result = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &description,
        initialState,
        clearValue,
        IID_PPV_ARGS(&resource));
    if (FAILED(result))
        std::cerr << "CreateCommittedResource failed: 0x" << std::hex << result << std::dec << '\n';
    return resource;
}

ComPtr<ID3D12Resource> CreateBuffer(
    ID3D12Device* device,
    UINT64 size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    const HRESULT result = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &description,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (FAILED(result))
        std::cerr << "CreateCommittedResource(buffer) failed: 0x"
                  << std::hex << result << std::dec << '\n';
    return resource;
}

bool ExecuteAndWait(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* list);

bool UploadRgbAsHalf(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* destination,
    const RgbImage& image,
    ComPtr<ID3D12Resource>& upload)
{
    const auto description = destination->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(
        &description, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    upload = CreateBuffer(
        device, totalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!upload) return false;

    unsigned char* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)))) return false;
    for (UINT y = 0; y < image.height; ++y)
    {
        auto* row = reinterpret_cast<uint16_t*>(mapped + footprint.Offset +
            static_cast<size_t>(y) * footprint.Footprint.RowPitch);
        for (UINT x = 0; x < image.width; ++x)
        {
            const size_t source = (static_cast<size_t>(y) * image.width + x) * 3;
            const size_t target = static_cast<size_t>(x) * 4;
            row[target + 0] = DirectX::PackedVector::XMConvertFloatToHalf(
                image.pixels[source + 0] / 255.0f);
            row[target + 1] = DirectX::PackedVector::XMConvertFloatToHalf(
                image.pixels[source + 1] / 255.0f);
            row[target + 2] = DirectX::PackedVector::XMConvertFloatToHalf(
                image.pixels[source + 2] / 255.0f);
            row[target + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
        }
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = destination;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = destination;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    return true;
}

std::optional<RgbImage> ReadHalfAsRgb(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* allocator,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* source,
    UINT width,
    UINT height)
{
    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) return std::nullopt;

    const auto description = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(
        &description, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    auto readback = CreateBuffer(
        device, totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return std::nullopt;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    if (!ExecuteAndWait(device, queue, list)) return std::nullopt;

    void* mappedRaw = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    if (FAILED(readback->Map(0, &readRange, &mappedRaw)))
        return std::nullopt;
    const auto* mapped = static_cast<const unsigned char*>(mappedRaw);

    RgbImage image{width, height, std::vector<unsigned char>(static_cast<size_t>(width) * height * 3)};
    for (UINT y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(mapped + footprint.Offset +
            static_cast<size_t>(y) * footprint.Footprint.RowPitch);
        for (UINT x = 0; x < width; ++x)
        {
            const size_t sourceIndex = static_cast<size_t>(x) * 4;
            const size_t targetIndex = (static_cast<size_t>(y) * width + x) * 3;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const float value = DirectX::PackedVector::XMConvertHalfToFloat(row[sourceIndex + channel]);
                image.pixels[targetIndex + channel] = static_cast<unsigned char>(
                    (std::clamp)(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);
    return image;
}

bool ExecuteAndWait(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* list)
{
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr) return false;
    const UINT64 value = 1;
    bool okay = SUCCEEDED(queue->Signal(fence.Get(), value)) &&
        SUCCEEDED(fence->SetEventOnCompletion(value, eventHandle)) &&
        WaitForSingleObject(eventHandle, 10000) == WAIT_OBJECT_0;
    CloseHandle(eventHandle);
    return okay;
}

LRESULT CALLBACK ProbeWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RunFrameSession(
    ID3D12Device* device,
    HMODULE core,
    AllocateParameters allocateParameters,
    const std::vector<std::filesystem::path>& inputPaths,
    const std::vector<std::filesystem::path>& outputPaths,
    float outputScale)
{
    const auto createFeature = Import<CreateFeature>(core, "NVSDK_NGX_D3D12_CreateFeature");
    const auto evaluateFeature = Import<EvaluateFeature>(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    const auto releaseFeature = Import<ReleaseFeature>(core, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (createFeature == nullptr || evaluateFeature == nullptr || allocateParameters == nullptr)
    {
        std::cerr << "The NGX frame functions are unavailable.\n";
        return false;
    }

    std::optional<RgbImage> firstImage;
    if (!inputPaths.empty())
    {
        firstImage = LoadPpm(inputPaths.front());
        if (!firstImage) return false;
    }
    const RgbImage* inputImage = firstImage ? &*firstImage : nullptr;
    const UINT width = inputImage != nullptr ? inputImage->width : 640;
    const UINT height = inputImage != nullptr ? inputImage->height : 360;
    const UINT outputWidth = inputImage != nullptr
        ? static_cast<UINT>(std::lround(static_cast<double>(width) * outputScale))
        : width;
    const UINT outputHeight = inputImage != nullptr
        ? static_cast<UINT>(std::lround(static_cast<double>(height) * outputScale))
        : height;
    if (outputWidth == 0 || outputHeight == 0 || outputWidth > 8192 || outputHeight > 8192)
    {
        std::cerr << "The scaled output dimensions are unsupported.\n";
        return false;
    }
    std::cout << "DLSS dimensions: " << width << 'x' << height << " -> "
              << outputWidth << 'x' << outputHeight << '\n';
    constexpr DXGI_FORMAT colorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        std::cerr << "Could not create the D3D12 command objects.\n";
        return false;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ProbeWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DLSS5VideoLabProbe";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"DLSS 5 Video Lab Probe",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(outputWidth),
        static_cast<int>(outputHeight),
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr)
    {
        std::cerr << "Could not create the probe window.\n";
        return false;
    }

    ComPtr<IDXGIFactory4> swapchainFactory;
    ComPtr<IDXGISwapChain1> swapchain;
    DXGI_SWAP_CHAIN_DESC1 swapchainDescription{};
    swapchainDescription.Width = outputWidth;
    swapchainDescription.Height = outputHeight;
    swapchainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchainDescription.SampleDesc.Count = 1;
    swapchainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDescription.BufferCount = 2;
    swapchainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&swapchainFactory))) ||
        FAILED(swapchainFactory->CreateSwapChainForHwnd(
            queue.Get(), window, &swapchainDescription, nullptr, nullptr, &swapchain)))
    {
        std::cerr << "Could not create the probe swapchain.\n";
        DestroyWindow(window);
        return false;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    swapchain->Present(0, 0);
    Sleep(100);

    D3D12_CLEAR_VALUE colorClear{};
    colorClear.Format = colorFormat;
    colorClear.Color[0] = 0.18f;
    colorClear.Color[1] = 0.42f;
    colorClear.Color[2] = 0.72f;
    colorClear.Color[3] = 1.0f;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_R32_FLOAT;
    D3D12_CLEAR_VALUE motionClear{};
    motionClear.Format = DXGI_FORMAT_R16G16_FLOAT;

    auto color = inputImage != nullptr
        ? CreateTexture(
            device, width, height, colorFormat,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST)
        : CreateTexture(
            device, width, height, colorFormat,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &colorClear);
    auto output = CreateTexture(
        device, outputWidth, outputHeight, colorFormat,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto depth = CreateTexture(
        device, width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &depthClear);
    auto motion = CreateTexture(
        device, width, height, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &motionClear);
    if (!color || !output || !depth || !motion) return false;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> colorUpload;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = inputImage != nullptr ? 2 : 3;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&rtvHeap)))) return false;
    const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (inputImage != nullptr)
    {
        if (!UploadRgbAsHalf(device, list.Get(), color.Get(), *inputImage, colorUpload)) return false;
    }
    else
    {
        device->CreateRenderTargetView(color.Get(), nullptr, rtv);
        list->ClearRenderTargetView(rtv, colorClear.Color, 0, nullptr);
        rtv.ptr += descriptorSize;
    }
    device->CreateRenderTargetView(depth.Get(), nullptr, rtv);
    list->ClearRenderTargetView(rtv, depthClear.Color, 0, nullptr);
    rtv.ptr += descriptorSize;
    device->CreateRenderTargetView(motion.Get(), nullptr, rtv);
    list->ClearRenderTargetView(rtv, motionClear.Color, 0, nullptr);

    NgxParameter* parameters = nullptr;
    if (allocateParameters(&parameters) != NgxSuccess || parameters == nullptr)
    {
        std::cerr << "NGX parameter allocation failed.\n";
        return false;
    }
    parameters->Set("Width", width);
    parameters->Set("Height", height);
    parameters->Set("OutWidth", outputWidth);
    parameters->Set("OutHeight", outputHeight);
    parameters->Set("PerfQualityValue", 5); // DLAA/native-size path
    parameters->Set("DLSS.Feature.Create.Flags", 107);
    parameters->Set("DLSS.Enable.Output.Subrects", 1);
    parameters->Set("CreationNodeMask", 1u);
    parameters->Set("VisibilityNodeMask", 1u);
    parameters->Set("RTXValue", 0);

    NgxHandle* handle = nullptr;
    DWORD exceptionCode = 0;
    const NgxResult createResult = SafeCreateFeature(
        createFeature, list.Get(), parameters, &handle, &exceptionCode);
    std::cout << "DLSS transport CreateFeature -> ";
    if (exceptionCode != 0)
        std::cout << "exception 0x" << std::hex << exceptionCode << std::dec << '\n';
    else
        std::cout << "NGX 0x" << std::hex << createResult << std::dec << '\n';
    if (exceptionCode != 0 || createResult != NgxSuccess || handle == nullptr) return false;
    if (!ExecuteAndWait(device, queue.Get(), list.Get())) return false;

    if (FAILED(allocator->Reset()) ||
        FAILED(list->Reset(allocator.Get(), nullptr))) return false;
    std::vector<D3D12_RESOURCE_BARRIER> guideBarriers;
    auto addGuideBarrier = [&](ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        guideBarriers.push_back(barrier);
    };
    if (inputImage == nullptr) addGuideBarrier(color.Get());
    addGuideBarrier(depth.Get());
    addGuideBarrier(motion.Get());
    list->ResourceBarrier(static_cast<UINT>(guideBarriers.size()), guideBarriers.data());

    parameters->Set("Color", color.Get());
    parameters->Set("Output", output.Get());
    parameters->Set("Depth", depth.Get());
    parameters->Set("MotionVectors", motion.Get());
    parameters->Set("DLSS.Render.Subrect.Dimensions.Width", width);
    parameters->Set("DLSS.Render.Subrect.Dimensions.Height", height);
    parameters->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    parameters->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    parameters->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    parameters->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
    parameters->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
    parameters->Set("DLSS.Output.Subrect.Base.X", 0u);
    parameters->Set("DLSS.Output.Subrect.Base.Y", 0u);
    parameters->Set("MV.Scale.X", -1.0f);
    parameters->Set("MV.Scale.Y", -1.0f);
    parameters->Set("Jitter.Offset.X", 0.0f);
    parameters->Set("Jitter.Offset.Y", 0.0f);
    parameters->Set("Sharpness", 0.0f);
    parameters->Set("DLSS.Pre.Exposure", 1.0f);
    parameters->Set("DLSS.Exposure.Scale", 1.0f);
    parameters->Set("Reset", 1);

    exceptionCode = 0;
    const NgxResult evaluateResult = SafeEvaluateFeature(
        evaluateFeature, list.Get(), handle, parameters, &exceptionCode);
    std::cout << "DLSS transport EvaluateFeature -> ";
    if (exceptionCode != 0)
        std::cout << "exception 0x" << std::hex << exceptionCode << std::dec << '\n';
    else
        std::cout << "NGX 0x" << std::hex << evaluateResult << std::dec << '\n';
    if (exceptionCode != 0 || evaluateResult != NgxSuccess) return false;
    if (!ExecuteAndWait(device, queue.Get(), list.Get())) return false;

    if (!outputPaths.empty())
    {
        const auto resultImage = ReadHalfAsRgb(
            device, queue.Get(), allocator.Get(), list.Get(), output.Get(), outputWidth, outputHeight);
        if (!resultImage || !SavePpm(outputPaths.front(), *resultImage))
        {
            std::wcerr << L"Could not save processed frame: " << outputPaths.front() << L'\n';
            return false;
        }
        std::wcout << L"Processed frame 1/" << inputPaths.size()
                   << L": " << outputPaths.front() << L'\n';
    }

    for (size_t frameIndex = 1; frameIndex < inputPaths.size(); ++frameIndex)
    {
        const auto frame = LoadPpm(inputPaths[frameIndex]);
        if (!frame || frame->width != width || frame->height != height)
        {
            std::wcerr << L"Every sequence frame must have the same dimensions: "
                       << inputPaths[frameIndex] << L'\n';
            return false;
        }
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;

        D3D12_RESOURCE_BARRIER reuseBarriers[2]{};
        reuseBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        reuseBarriers[0].Transition.pResource = color.Get();
        reuseBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        reuseBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        reuseBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        reuseBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        reuseBarriers[1].Transition.pResource = output.Get();
        reuseBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        reuseBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        reuseBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(2, reuseBarriers);

        ComPtr<ID3D12Resource> frameUpload;
        if (!UploadRgbAsHalf(device, list.Get(), color.Get(), *frame, frameUpload)) return false;
        parameters->Set("Reset", 0);

        exceptionCode = 0;
        const NgxResult frameResult = SafeEvaluateFeature(
            evaluateFeature, list.Get(), handle, parameters, &exceptionCode);
        if (exceptionCode != 0 || frameResult != NgxSuccess)
        {
            std::cerr << "DLSS transport failed on frame " << frameIndex + 1 << ": ";
            if (exceptionCode != 0)
                std::cerr << "exception 0x" << std::hex << exceptionCode << std::dec << '\n';
            else
                std::cerr << "NGX 0x" << std::hex << frameResult << std::dec << '\n';
            return false;
        }
        if (!ExecuteAndWait(device, queue.Get(), list.Get())) return false;

        const auto resultImage = ReadHalfAsRgb(
            device, queue.Get(), allocator.Get(), list.Get(), output.Get(), outputWidth, outputHeight);
        if (!resultImage || frameIndex >= outputPaths.size() ||
            !SavePpm(outputPaths[frameIndex], *resultImage))
        {
            std::wcerr << L"Could not save processed frame: "
                       << (frameIndex < outputPaths.size() ? outputPaths[frameIndex] : L"missing path")
                       << L'\n';
            return false;
        }

        swapchain->Present(0, 0);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::wcout << L"Processed frame " << frameIndex + 1 << L'/' << inputPaths.size()
                   << L": " << outputPaths[frameIndex] << L'\n';
    }

    if (releaseFeature != nullptr) releaseFeature(handle);
    swapchain.Reset();
    DestroyWindow(window);
    return true;
}
}

int wmain(int argc, wchar_t** argv)
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::wcout << std::unitbuf;
    std::wcerr << std::unitbuf;

    if (argc < 2 || argc > 6)
    {
        std::wcerr
            << L"Usage:\n"
            << L"  ngx-capability-probe.exe <runtime> [--evaluate]\n"
            << L"  ngx-capability-probe.exe <runtime> --frame <input.ppm> <output.ppm> [scale]\n"
            << L"  ngx-capability-probe.exe <runtime> --sequence <input-dir> <output-dir> [scale]\n";
        return 2;
    }

    const bool evaluateSynthetic = argc == 3 && std::wstring_view(argv[2]) == L"--evaluate";
    const bool evaluateFrame = (argc == 5 || argc == 6) && std::wstring_view(argv[2]) == L"--frame";
    const bool evaluateSequence = (argc == 5 || argc == 6) && std::wstring_view(argv[2]) == L"--sequence";
    if (argc != 2 && !evaluateSynthetic && !evaluateFrame && !evaluateSequence)
    {
        std::wcerr << L"Invalid arguments. Use --evaluate, --frame, or --sequence.\n";
        return 2;
    }

    float outputScale = 1.0f;
    if (argc == 6)
    {
        try
        {
            outputScale = std::stof(argv[5]);
        }
        catch (const std::exception&)
        {
            std::wcerr << L"Scale must be a number from 1.0 through 4.0.\n";
            return 2;
        }
        if (!std::isfinite(outputScale) || outputScale < 1.0f || outputScale > 4.0f)
        {
            std::wcerr << L"Scale must be a number from 1.0 through 4.0.\n";
            return 2;
        }
    }

    std::vector<std::filesystem::path> inputPaths;
    std::vector<std::filesystem::path> outputPaths;
    if (evaluateFrame)
    {
        inputPaths.push_back(std::filesystem::absolute(argv[3]));
        outputPaths.push_back(std::filesystem::absolute(argv[4]));
        std::wcout << L"Input frame: " << inputPaths.front() << L'\n';
    }
    else if (evaluateSequence)
    {
        const auto inputDirectory = std::filesystem::absolute(argv[3]);
        const auto outputDirectory = std::filesystem::absolute(argv[4]);
        if (!std::filesystem::is_directory(inputDirectory) ||
            !std::filesystem::is_directory(outputDirectory))
        {
            std::wcerr << L"Both sequence paths must be existing directories.\n";
            return 2;
        }
        for (const auto& entry : std::filesystem::directory_iterator(inputDirectory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".ppm")
                inputPaths.push_back(entry.path());
        }
        std::sort(inputPaths.begin(), inputPaths.end());
        if (inputPaths.empty())
        {
            std::wcerr << L"The sequence directory contains no .ppm frames.\n";
            return 2;
        }
        for (const auto& inputPath : inputPaths)
            outputPaths.push_back(outputDirectory / inputPath.filename());
        std::wcout << L"Input sequence: " << inputPaths.size() << L" frames from "
                   << inputDirectory << L'\n';
    }

    const auto runtimeDirectory = std::filesystem::absolute(argv[1]);
    std::wcout << L"Runtime: " << runtimeDirectory << L'\n';

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    const DLL_DIRECTORY_COOKIE runtimeCookie = AddDllDirectory(runtimeDirectory.c_str());
    if (runtimeCookie == nullptr)
    {
        std::wcerr << L"Could not add the runtime search path.\n";
        return 3;
    }

    // Load both feature implementations explicitly. They remain user-supplied;
    // the probe only receives a directory path and never copies them.
    if (!LoadRuntimeModule(runtimeDirectory, L"nvngx_dlss.dll") ||
        !LoadRuntimeModule(runtimeDirectory, L"nvngx_dlssnr.dll"))
    {
        RemoveDllDirectory(runtimeCookie);
        return 4;
    }

    const auto corePath = FindDriverNgxCore();
    if (!corePath)
    {
        std::wcerr << L"NVIDIA's driver NGX core (_nvngx.dll) was not found.\n";
        RemoveDllDirectory(runtimeCookie);
        return 5;
    }
    std::wcout << L"NGX core: " << *corePath << L'\n';

    HMODULE core = LoadLibraryExW(corePath->c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (core == nullptr)
    {
        std::wcerr << L"Could not load the driver NGX core: " << WindowsError(GetLastError()) << L'\n';
        RemoveDllDirectory(runtimeCookie);
        return 6;
    }

    const auto initExt = Import<InitExt>(core, "NVSDK_NGX_D3D12_Init_Ext");
    const auto initProject = Import<InitProjectId>(core, "NVSDK_NGX_D3D12_Init_ProjectID");
    const auto getCapabilities = Import<GetCapabilities>(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    const auto allocateParameters = Import<AllocateParameters>(core, "NVSDK_NGX_D3D12_AllocateParameters");
    const auto destroyParameters = Import<DestroyParameters>(core, "NVSDK_NGX_D3D12_DestroyParameters");
    const auto shutdown = Import<Shutdown>(core, "NVSDK_NGX_D3D12_Shutdown1");
    if (initExt == nullptr || getCapabilities == nullptr)
    {
        std::cerr << "The driver NGX core does not expose the required D3D12 functions.\n";
        FreeLibrary(core);
        RemoveDllDirectory(runtimeCookie);
        return 7;
    }

    const auto adapter = FindNvidiaAdapter();
    if (!adapter)
    {
        std::cerr << "No NVIDIA graphics adapter was found.\n";
        FreeLibrary(core);
        RemoveDllDirectory(runtimeCookie);
        return 8;
    }

    ComPtr<ID3D12Device> device;
    const HRESULT deviceResult = D3D12CreateDevice(
        adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(deviceResult))
    {
        std::cerr << "D3D12CreateDevice failed: 0x" << std::hex << deviceResult << std::dec << '\n';
        FreeLibrary(core);
        RemoveDllDirectory(runtimeCookie);
        return 9;
    }

    bool initialized = false;
    int acceptedVersion = 0;
    for (int version = 0x13; version <= 0x16 && !initialized; ++version)
    {
        DWORD exceptionCode = 0;
        NgxResult result = SafeInitExt(
            initExt, 0x1000000ULL, runtimeDirectory.c_str(), device.Get(), version, &exceptionCode);
        if (exceptionCode != 0)
        {
            std::cout << "Init_Ext SDK 0x" << std::hex << version
                      << " raised exception 0x" << exceptionCode << std::dec << '\n';
            break;
        }
        std::cout << "Init_Ext SDK 0x" << std::hex << version << " -> 0x" << result << std::dec << '\n';
        if (result == NgxSuccess)
        {
            initialized = true;
            acceptedVersion = version;
            break;
        }

        if (initProject != nullptr)
        {
            result = SafeInitProject(
                initProject, runtimeDirectory.c_str(), device.Get(), version, &exceptionCode);
            if (exceptionCode != 0)
            {
                std::cout << "Init_ProjectID SDK 0x" << std::hex << version
                          << " raised exception 0x" << exceptionCode << std::dec << '\n';
                break;
            }
            std::cout << "Init_ProjectID SDK 0x" << std::hex << version << " -> 0x" << result << std::dec << '\n';
            if (result == NgxSuccess)
            {
                initialized = true;
                acceptedVersion = version;
            }
        }
    }

    if (!initialized)
    {
        std::cerr << "NGX refused every tested SDK version.\n";
        FreeLibrary(core);
        RemoveDllDirectory(runtimeCookie);
        return 10;
    }
    std::cout << "NGX D3D12 initialized with SDK 0x" << std::hex << acceptedVersion << std::dec << ".\n";

    NgxParameter* capabilities = nullptr;
    DWORD capabilityException = 0;
    const NgxResult capabilityResult = SafeGetCapabilities(
        getCapabilities, &capabilities, &capabilityException);
    if (capabilityException != 0)
    {
        std::cerr << "Capability query raised exception 0x" << std::hex
                  << capabilityException << std::dec << '\n';
        if (shutdown != nullptr) shutdown(device.Get());
        FreeLibrary(core);
        RemoveDllDirectory(runtimeCookie);
        return 11;
    }
    if (capabilityResult != NgxSuccess || capabilities == nullptr)
    {
        std::cerr << "Capability query failed: 0x" << std::hex << capabilityResult << std::dec << '\n';
        if (shutdown != nullptr) shutdown(device.Get());
        FreeLibrary(core);
        RemoveDllDirectory(runtimeCookie);
        return 11;
    }

    std::cout << "NGX capabilities:\n";
    PrintCapability(capabilities, "SuperSampling.Available");
    PrintCapability(capabilities, "SuperSampling.NeedsUpdatedDriver");
    PrintCapability(capabilities, "SuperSampling.MinDriverVersionMajor");
    PrintCapability(capabilities, "SuperSampling.MinDriverVersionMinor");
    PrintCapability(capabilities, "SuperSamplingDenoising.Available");

    if (evaluateSynthetic || evaluateFrame || evaluateSequence)
    {
        const bool evaluated = RunFrameSession(
            device.Get(),
            core,
            allocateParameters,
            inputPaths,
            outputPaths,
            outputScale);
        std::cout << (evaluateSequence ? "Input sequence: " :
                      (evaluateFrame ? "Input frame: " : "Synthetic frame: "))
                  << (evaluated ? "completed" : "failed") << '\n';
        if (!evaluated) return 12;
    }

    // NGX teardown is intentionally left to process exit in this diagnostic.
    // Driver/runtime combinations seen in the wild have faulted while tearing
    // down an otherwise successful capability-only session. No resources are
    // retained after this short-lived process exits.
    static_cast<void>(destroyParameters);
    static_cast<void>(shutdown);
    static_cast<void>(core);
    static_cast<void>(runtimeCookie);
    return 0;
}

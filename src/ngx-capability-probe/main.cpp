// SPDX-License-Identifier: MIT
//
// Small, read-only NGX capability probe. The NGX declaration order and the
// version-negotiation approach are adapted from NIGos/dlss5-dx11-bridge (MIT).

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
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

bool RunSyntheticFrame(
    ID3D12Device* device,
    HMODULE core,
    AllocateParameters allocateParameters)
{
    const auto createFeature = Import<CreateFeature>(core, "NVSDK_NGX_D3D12_CreateFeature");
    const auto evaluateFeature = Import<EvaluateFeature>(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    const auto releaseFeature = Import<ReleaseFeature>(core, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (createFeature == nullptr || evaluateFeature == nullptr || allocateParameters == nullptr)
    {
        std::cerr << "The NGX frame functions are unavailable.\n";
        return false;
    }

    constexpr UINT width = 640;
    constexpr UINT height = 360;
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
        static_cast<int>(width),
        static_cast<int>(height),
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
    swapchainDescription.Width = width;
    swapchainDescription.Height = height;
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

    auto color = CreateTexture(
        device, width, height, colorFormat,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &colorClear);
    auto output = CreateTexture(
        device, width, height, colorFormat,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto depth = CreateTexture(
        device, width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto motion = CreateTexture(
        device, width, height, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!color || !output || !depth || !motion) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
    rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDescription.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDescription, IID_PPV_ARGS(&rtvHeap)))) return false;
    device->CreateRenderTargetView(color.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
    list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), colorClear.Color, 0, nullptr);

    NgxParameter* parameters = nullptr;
    if (allocateParameters(&parameters) != NgxSuccess || parameters == nullptr)
    {
        std::cerr << "NGX parameter allocation failed.\n";
        return false;
    }
    parameters->Set("Width", width);
    parameters->Set("Height", height);
    parameters->Set("OutWidth", width);
    parameters->Set("OutHeight", height);
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
    std::cout << "Synthetic DLSS CreateFeature -> ";
    if (exceptionCode != 0)
        std::cout << "exception 0x" << std::hex << exceptionCode << std::dec << '\n';
    else
        std::cout << "NGX 0x" << std::hex << createResult << std::dec << '\n';
    if (exceptionCode != 0 || createResult != NgxSuccess || handle == nullptr) return false;
    if (!ExecuteAndWait(device, queue.Get(), list.Get())) return false;

    if (FAILED(allocator->Reset()) ||
        FAILED(list->Reset(allocator.Get(), nullptr))) return false;
    D3D12_RESOURCE_BARRIER colorBarrier{};
    colorBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    colorBarrier.Transition.pResource = color.Get();
    colorBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    colorBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    colorBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &colorBarrier);

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
    std::cout << "Synthetic DLSS EvaluateFeature -> ";
    if (exceptionCode != 0)
        std::cout << "exception 0x" << std::hex << exceptionCode << std::dec << '\n';
    else
        std::cout << "NGX 0x" << std::hex << evaluateResult << std::dec << '\n';
    if (exceptionCode != 0 || evaluateResult != NgxSuccess) return false;
    if (!ExecuteAndWait(device, queue.Get(), list.Get())) return false;

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

    if (argc < 2 || argc > 3)
    {
        std::wcerr << L"Usage: ngx-capability-probe.exe <Streamline runtime directory> [--evaluate]\n";
        return 2;
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

    if (argc == 3 && std::wstring_view(argv[2]) == L"--evaluate")
    {
        const bool evaluated = RunSyntheticFrame(device.Get(), core, allocateParameters);
        std::cout << "Synthetic frame: " << (evaluated ? "completed" : "failed") << '\n';
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

// SPDX-License-Identifier: MIT

#include "live_ngx.hpp"

#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <reshade.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
using NgxResult = int;
constexpr NgxResult NgxSuccess = 1;

#pragma pack(push, 1)
struct GuidePackHeader
{
    char magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t frameCount;
    uint32_t fpsNumerator;
    uint32_t fpsDenominator;
    uint64_t frameStride;
};

struct LiveGuideHeader
{
    char magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t frameBytes;
    uint32_t guideBytes;
    volatile LONG inputState;
    volatile LONG outputState;
    volatile LONG64 inputSequence;
    volatile LONG64 outputSequence;
    uint32_t reset;
    uint32_t processingMicroseconds;
    uint64_t generatedCount;
};
#pragma pack(pop)

static_assert(sizeof(GuidePackHeader) == 36);
static_assert(sizeof(LiveGuideHeader) == 64);

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

struct NgxHandle { unsigned int id; };
using InitExt = NgxResult (*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using AllocateParameters = NgxResult (*)(NgxParameter**);
using CreateFeature = NgxResult (*)(ID3D12GraphicsCommandList*, int, NgxParameter*, NgxHandle**);
using EvaluateFeature = NgxResult (*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameter*, void*);
using ReleaseFeature = NgxResult (*)(NgxHandle*);
using DestroyParameters = NgxResult (*)(NgxParameter*);

template <typename T>
T import(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

NgxResult safe_init(
    InitExt function, const wchar_t* path, ID3D12Device* device, int version, DWORD& exception)
{
    exception = 0;
    __try { return function(0x1000000ULL, path, device, version, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        exception = GetExceptionCode();
        return 0;
    }
}

NgxResult safe_create(
    CreateFeature function, ID3D12GraphicsCommandList* list,
    NgxParameter* parameters, NgxHandle** handle, DWORD& exception)
{
    exception = 0;
    __try { return function(list, 1, parameters, handle); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        exception = GetExceptionCode();
        return 0;
    }
}

NgxResult safe_evaluate(
    EvaluateFeature function, ID3D12GraphicsCommandList* list,
    NgxHandle* handle, NgxParameter* parameters, DWORD& exception)
{
    exception = 0;
    __try { return function(list, handle, parameters, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        exception = GetExceptionCode();
        return 0;
    }
}

NgxResult safe_release(ReleaseFeature function, NgxHandle* handle)
{
    __try { return function(handle); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

NgxResult safe_destroy_parameters(DestroyParameters function, NgxParameter* parameters)
{
    __try { return function(parameters); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

struct Session
{
    bool preloaded = false;
    bool ngxInitialized = false;
    bool attempted = false;
    bool ready = false;
    bool liveGuides = false;
    bool liveGuideReady = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    DXGI_FORMAT sharedFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t evaluations = 0;
    HANDLE guideFile = INVALID_HANDLE_VALUE;
    HANDLE guideMapping = nullptr;
    const uint8_t* guideData = nullptr;
    uint64_t guideSize = 0;
    HANDLE liveMapping = nullptr;
    uint8_t* liveData = nullptr;
    uint64_t liveSize = 0;
    uint64_t publishedSequence = 0;
    uint64_t consumedSequence = 0;
    uint64_t droppedFrames = 0;
    GuidePackHeader guideHeader{};
    uint32_t currentGuideFrame = std::numeric_limits<uint32_t>::max();
    bool holdingGuide = false;
    bool lastUseEstimatedDepth = true;
    bool lastUseEstimatedMotion = true;
    bool lastReverseEstimatedDepth = false;
    bool uploadedReverseEstimatedDepth = false;
    std::string guideState = "no guide pack configured";
    std::string guideBindingState = "not checked";
    std::string state = "waiting";
    ULONGLONG preloadTime = 0;
    std::filesystem::path runtimeDirectory;
    HMODULE core = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE eventHandle = nullptr;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> motion;
    ComPtr<ID3D12Resource> zeroDepth;
    ComPtr<ID3D12Resource> zeroMotion;
    ComPtr<ID3D12Resource> depthUpload;
    ComPtr<ID3D12Resource> motionUpload;
    uint8_t* depthUploadData = nullptr;
    uint8_t* motionUploadData = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFootprint{};
    ComPtr<ID3D11Texture2D> color11;
    ComPtr<ID3D11Texture2D> output11;
    ComPtr<ID3D11Texture2D> guidePreview11;
    ComPtr<ID3D11Texture2D> liveReadback11;
    std::vector<uint32_t> guidePreview;
    std::vector<float> liveDepth;
    std::vector<uint16_t> liveMotion;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> inputScalerEnumerator;
    ComPtr<ID3D11VideoProcessor> inputScaler;
    ComPtr<ID3D11VideoProcessorEnumerator> previewScalerEnumerator;
    ComPtr<ID3D11VideoProcessor> previewScaler;
    ComPtr<ID3D11Query> copyQuery;
    HANDLE colorShared = nullptr;
    HANDLE outputShared = nullptr;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    NgxParameter* parameters = nullptr;
    NgxHandle* feature = nullptr;
    CreateFeature createFeature = nullptr;
    EvaluateFeature evaluateFeature = nullptr;
    ReleaseFeature releaseFeature = nullptr;
    DestroyParameters destroyParameters = nullptr;
};

Session session;

void log_error(const char* message)
{
    session.state = message;
    reshade::log::message(reshade::log::level::error, message);
}

ComPtr<ID3D12Resource> make_texture(
    uint32_t width, uint32_t height, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    if (FAILED(session.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&result))))
        return {};
    return result;
}

bool make_upload_buffer(
    uint64_t size,
    ComPtr<ID3D12Resource>& resource,
    uint8_t*& mapped)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(session.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&resource)))) return false;
    return SUCCEEDED(resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
}

bool make_shared_texture(
    ID3D11Device* hostDevice,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    bool unorderedAccess,
    ComPtr<ID3D12Resource>& texture12,
    ComPtr<ID3D11Texture2D>& texture11,
    HANDLE& sharedHandle)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        (unorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    HRESULT result = session.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&texture12));
    if (SUCCEEDED(result))
        result = session.device->CreateSharedHandle(
            texture12.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
    ComPtr<ID3D11Device1> device1;
    if (SUCCEEDED(result))
        result = hostDevice->QueryInterface(IID_PPV_ARGS(&device1));
    if (SUCCEEDED(result))
        result = device1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&texture11));
    if (SUCCEEDED(result)) return true;

    texture11.Reset();
    texture12.Reset();
    if (sharedHandle != nullptr)
    {
        CloseHandle(sharedHandle);
        sharedHandle = nullptr;
    }

    // Some drivers reject D3D12-owned simultaneous-access textures when they
    // are opened by D3D11. Try the equally valid reverse ownership direction.
    D3D11_TEXTURE2D_DESC desc11{};
    desc11.Width = width;
    desc11.Height = height;
    desc11.MipLevels = 1;
    desc11.ArraySize = 1;
    desc11.Format = format;
    desc11.SampleDesc.Count = 1;
    desc11.Usage = D3D11_USAGE_DEFAULT;
    desc11.BindFlags = D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_RENDER_TARGET |
        (unorderedAccess ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                       D3D11_RESOURCE_MISC_SHARED;
    result = hostDevice->CreateTexture2D(&desc11, nullptr, &texture11);
    ComPtr<IDXGIResource1> dxgiResource;
    if (SUCCEEDED(result)) result = texture11.As(&dxgiResource);
    if (SUCCEEDED(result))
        result = dxgiResource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &sharedHandle);
    if (SUCCEEDED(result))
        result = session.device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&texture12));
    return SUCCEEDED(result);
}

void barrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER value{};
    value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition.pResource = resource;
    value.Transition.StateBefore = before;
    value.Transition.StateAfter = after;
    value.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    session.list->ResourceBarrier(1, &value);
}

bool submit_and_wait()
{
    if (FAILED(session.list->Close())) return false;
    ID3D12CommandList* lists[] = {session.list.Get()};
    session.queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++session.fenceValue;
    if (FAILED(session.queue->Signal(session.fence.Get(), value)) ||
        FAILED(session.fence->SetEventOnCompletion(value, session.eventHandle))) return false;
    return WaitForSingleObject(session.eventHandle, 10000) == WAIT_OBJECT_0;
}

bool wait_for_d3d11_copy()
{
    session.context11->End(session.copyQuery.Get());
    session.context11->Flush();
    const ULONGLONG started = GetTickCount64();
    while (session.context11->GetData(session.copyQuery.Get(), nullptr, 0, 0) == S_FALSE)
    {
        if (GetTickCount64() - started > 10000) return false;
        Sleep(0);
    }
    return true;
}

bool make_video_scaler(
    uint32_t inputWidth,
    uint32_t inputHeight,
    uint32_t outputWidth,
    uint32_t outputHeight,
    ComPtr<ID3D11VideoProcessorEnumerator>& enumerator,
    ComPtr<ID3D11VideoProcessor>& processor)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {30000, 1001};
    desc.InputWidth = inputWidth;
    desc.InputHeight = inputHeight;
    desc.OutputFrameRate = {30000, 1001};
    desc.OutputWidth = outputWidth;
    desc.OutputHeight = outputHeight;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    return SUCCEEDED(session.videoDevice->CreateVideoProcessorEnumerator(
               &desc, &enumerator)) &&
           SUCCEEDED(session.videoDevice->CreateVideoProcessor(
               enumerator.Get(), 0, &processor));
}

bool scale_d3d11_texture(
    ID3D11Texture2D* source,
    ID3D11Texture2D* destination,
    ID3D11VideoProcessorEnumerator* enumerator,
    ID3D11VideoProcessor* processor)
{
    D3D11_TEXTURE2D_DESC sourceDesc{};
    D3D11_TEXTURE2D_DESC destinationDesc{};
    source->GetDesc(&sourceDesc);
    destination->GetDesc(&destinationDesc);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = 0;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(session.videoDevice->CreateVideoProcessorInputView(
            source, enumerator, &inputDesc, &inputView)) ||
        FAILED(session.videoDevice->CreateVideoProcessorOutputView(
            destination, enumerator, &outputDesc, &outputView))) return false;

    RECT sourceRect{0, 0, static_cast<LONG>(sourceDesc.Width),
                    static_cast<LONG>(sourceDesc.Height)};
    RECT destinationRect{0, 0, static_cast<LONG>(destinationDesc.Width),
                         static_cast<LONG>(destinationDesc.Height)};
    session.videoContext->VideoProcessorSetStreamSourceRect(
        processor, 0, TRUE, &sourceRect);
    session.videoContext->VideoProcessorSetStreamDestRect(
        processor, 0, TRUE, &destinationRect);
    session.videoContext->VideoProcessorSetOutputTargetRect(
        processor, TRUE, &destinationRect);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    return SUCCEEDED(session.videoContext->VideoProcessorBlt(
        processor, outputView.Get(), 0, 1, &stream));
}

void reset_transport()
{
    if (session.feature != nullptr && session.releaseFeature != nullptr)
        safe_release(session.releaseFeature, session.feature);
    session.feature = nullptr;
    if (session.parameters != nullptr && session.destroyParameters != nullptr)
        safe_destroy_parameters(session.destroyParameters, session.parameters);
    session.parameters = nullptr;
    if (session.depthUpload != nullptr && session.depthUploadData != nullptr)
        session.depthUpload->Unmap(0, nullptr);
    if (session.motionUpload != nullptr && session.motionUploadData != nullptr)
        session.motionUpload->Unmap(0, nullptr);
    session.depthUploadData = nullptr;
    session.motionUploadData = nullptr;
    if (session.guideData != nullptr) UnmapViewOfFile(session.guideData);
    if (session.liveData != nullptr) UnmapViewOfFile(session.liveData);
    if (session.guideMapping != nullptr) CloseHandle(session.guideMapping);
    if (session.liveMapping != nullptr) CloseHandle(session.liveMapping);
    if (session.guideFile != INVALID_HANDLE_VALUE) CloseHandle(session.guideFile);
    if (session.colorShared != nullptr) CloseHandle(session.colorShared);
    if (session.outputShared != nullptr) CloseHandle(session.outputShared);
    if (session.eventHandle != nullptr) CloseHandle(session.eventHandle);

    const bool preloaded = session.preloaded;
    const ULONGLONG preloadTime = session.preloadTime;
    const std::filesystem::path runtimeDirectory = session.runtimeDirectory;
    HMODULE const core = session.core;

    // Release every resource while the NGX-owned D3D12 device is still alive.
    // Some hooked NGX runtimes invalidate their wrapped device during Shutdown1,
    // so allowing Session's generated move assignment to release these objects
    // after shutdown can dereference an invalid COM vtable during ResizeBuffers.
    session.copyQuery.Reset();
    session.previewScaler.Reset();
    session.previewScalerEnumerator.Reset();
    session.inputScaler.Reset();
    session.inputScalerEnumerator.Reset();
    session.videoContext.Reset();
    session.videoDevice.Reset();
    session.guidePreview11.Reset();
    session.liveReadback11.Reset();
    session.output11.Reset();
    session.color11.Reset();
    session.context11.Reset();

    session.rtvHeap.Reset();
    session.motionUpload.Reset();
    session.depthUpload.Reset();
    session.zeroMotion.Reset();
    session.zeroDepth.Reset();
    session.motion.Reset();
    session.depth.Reset();
    session.output.Reset();
    session.color.Reset();
    session.fence.Reset();
    session.list.Reset();
    session.allocator.Reset();
    session.queue.Reset();

    // A resize does not end the NGX process lifetime. Keep the initialized
    // device alive and rebuild only the feature and size-dependent resources.
    // The patched runtime invalidates its wrapped device during Shutdown1,
    // which makes a normal COM Release afterwards unsafe.
    ComPtr<ID3D12Device> device = std::move(session.device);
    const bool ngxInitialized = session.ngxInitialized;

    session = Session{};
    session.preloaded = preloaded;
    session.preloadTime = preloadTime;
    session.runtimeDirectory = runtimeDirectory;
    session.core = core;
    session.device = std::move(device);
    session.ngxInitialized = ngxInitialized;
}

std::filesystem::path addon_directory(HMODULE module)
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

bool environment_enabled(const wchar_t* name)
{
    std::array<wchar_t, 16> value{};
    const DWORD length = GetEnvironmentVariableW(
        name, value.data(), static_cast<DWORD>(value.size()));
    return length != 0 && length < value.size() && value[0] != L'0';
}

uint32_t environment_dimension(const wchar_t* name, uint32_t fallback)
{
    std::array<wchar_t, 32> value{};
    const DWORD length = GetEnvironmentVariableW(
        name, value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size()) return fallback;
    const unsigned long parsed = wcstoul(value.data(), nullptr, 10);
    return parsed >= 64 && parsed <= 4096
        ? static_cast<uint32_t>(parsed) : fallback;
}

bool open_live_guides(uint32_t width, uint32_t height)
{
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    const uint64_t frameBytes = pixels * 4;
    const uint64_t guideBytes = pixels * 8;
    session.liveSize = sizeof(LiveGuideHeader) + frameBytes + guideBytes;
    if (session.liveSize > std::numeric_limits<DWORD>::max()) return false;
    session.liveMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(session.liveSize), L"Local\\DLSS5VideoLiveGuides");
    if (session.liveMapping == nullptr) return false;
    session.liveData = static_cast<uint8_t*>(MapViewOfFile(
        session.liveMapping, FILE_MAP_ALL_ACCESS, 0, 0, session.liveSize));
    if (session.liveData == nullptr) return false;

    auto* const header = reinterpret_cast<LiveGuideHeader*>(session.liveData);
    if (std::memcmp(header->magic, "D5LV", 4) != 0 ||
        header->version != 1 || header->width != width ||
        header->height != height || header->frameBytes != frameBytes ||
        header->guideBytes != guideBytes)
    {
        std::memset(session.liveData, 0, static_cast<size_t>(session.liveSize));
        std::memcpy(header->magic, "D5LV", 4);
        header->version = 1;
        header->width = width;
        header->height = height;
        header->frameBytes = static_cast<uint32_t>(frameBytes);
        header->guideBytes = static_cast<uint32_t>(guideBytes);
    }
    session.liveDepth.resize(static_cast<size_t>(pixels));
    session.liveMotion.resize(static_cast<size_t>(pixels) * 2);
    session.guideHeader.width = width;
    session.guideHeader.height = height;
    session.guideState = "waiting for the real-time depth and optical-flow service";
    return true;
}

bool load_guide_pack(HMODULE module)
{
    if (environment_enabled(L"DLSS5_VIDEO_LIVE_GUIDES"))
    {
        session.liveGuides = true;
        const uint32_t width = environment_dimension(
            L"DLSS5_VIDEO_INPUT_WIDTH", 960);
        const uint32_t height = environment_dimension(
            L"DLSS5_VIDEO_INPUT_HEIGHT", 540);
        return open_live_guides(width, height);
    }
    std::array<wchar_t, 32768> configured{};
    const DWORD configuredLength = GetEnvironmentVariableW(
        L"DLSS5_VIDEO_GUIDE_PACK", configured.data(),
        static_cast<DWORD>(configured.size()));
    std::filesystem::path path;
    if (configuredLength != 0 && configuredLength < configured.size())
        path = std::wstring(configured.data(), configuredLength);
    else
        path = addon_directory(module) / L"guides.d5gp";

    if (!std::filesystem::is_regular_file(path))
    {
        session.guideState = "guide pack not found; using zero guides";
        return true;
    }

    session.guideFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER fileSize{};
    if (session.guideFile == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(session.guideFile, &fileSize)) return false;
    session.guideSize = static_cast<uint64_t>(fileSize.QuadPart);
    session.guideMapping = CreateFileMappingW(
        session.guideFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (session.guideMapping == nullptr) return false;
    session.guideData = static_cast<const uint8_t*>(MapViewOfFile(
        session.guideMapping, FILE_MAP_READ, 0, 0, 0));
    if (session.guideData == nullptr || session.guideSize < sizeof(GuidePackHeader))
        return false;
    std::memcpy(&session.guideHeader, session.guideData, sizeof(GuidePackHeader));
    const uint64_t expectedSize = sizeof(GuidePackHeader) +
        session.guideHeader.frameStride * session.guideHeader.frameCount;
    if (std::memcmp(session.guideHeader.magic, "D5GP", 4) != 0 ||
        session.guideHeader.version != 1 ||
        session.guideHeader.width == 0 ||
        session.guideHeader.height == 0 ||
        session.guideHeader.frameCount == 0 ||
        expectedSize > session.guideSize)
    {
        session.guideState = "guide pack dimensions or format do not match the player";
        return false;
    }
    session.guideState = "precomputed depth and optical flow stream active";
    const std::string guideMessage = "DLSS 5 Video Renderer loaded " +
        std::to_string(session.guideHeader.frameCount) +
        "-frame precomputed guide stream.";
    reshade::log::message(
        reshade::log::level::info,
        guideMessage.c_str());
    return true;
}

float half_to_float(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1Fu;
    uint32_t mantissa = value & 0x3FFu;
    uint32_t bits = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                --exponent;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
        }
    }
    else if (exponent == 0x1Fu)
        bits = sign | 0x7F800000u | (mantissa << 13);
    else
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

const uint8_t* guide_frame_data(uint32_t frameIndex)
{
    return session.guideData + sizeof(GuidePackHeader) +
        session.guideHeader.frameStride * frameIndex;
}

bool upload_guide_frame(uint32_t frameIndex, bool reverseDepth, bool& reset)
{
    reset = false;
    if (session.guideData == nullptr ||
        frameIndex >= session.guideHeader.frameCount) return true;
    if (frameIndex == session.currentGuideFrame &&
        reverseDepth == session.uploadedReverseEstimatedDepth) return true;

    const uint8_t* const frame = guide_frame_data(frameIndex);
    uint32_t packedReset = 0;
    std::memcpy(&packedReset, frame, sizeof(packedReset));
    reset = packedReset != 0 ||
        (session.currentGuideFrame != std::numeric_limits<uint32_t>::max() &&
         frameIndex < session.currentGuideFrame);

    const uint32_t tightPitch = session.width * sizeof(float);
    const uint8_t* const depth = frame + sizeof(uint32_t);
    const uint8_t* const motion = depth +
        static_cast<uint64_t>(session.width) * session.height * sizeof(float);
    for (uint32_t y = 0; y < session.height; ++y)
    {
        uint8_t* const depthRow = session.depthUploadData +
            static_cast<size_t>(y) * session.depthFootprint.Footprint.RowPitch;
        const uint8_t* const sourceDepthRow =
            depth + static_cast<size_t>(y) * tightPitch;
        if (!reverseDepth)
        {
            std::memcpy(depthRow, sourceDepthRow, tightPitch);
        }
        else
        {
            const auto* const sourceValues =
                reinterpret_cast<const float*>(sourceDepthRow);
            auto* const targetValues = reinterpret_cast<float*>(depthRow);
            for (uint32_t x = 0; x < session.width; ++x)
                targetValues[x] = 1.0f - sourceValues[x];
        }
        std::memcpy(
            session.motionUploadData +
                static_cast<size_t>(y) * session.motionFootprint.Footprint.RowPitch,
            motion + static_cast<size_t>(y) * tightPitch, tightPitch);
    }

    barrier(session.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(session.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION depthSource{};
    depthSource.pResource = session.depthUpload.Get();
    depthSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    depthSource.PlacedFootprint = session.depthFootprint;
    D3D12_TEXTURE_COPY_LOCATION depthTarget{};
    depthTarget.pResource = session.depth.Get();
    depthTarget.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION motionSource{};
    motionSource.pResource = session.motionUpload.Get();
    motionSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    motionSource.PlacedFootprint = session.motionFootprint;
    D3D12_TEXTURE_COPY_LOCATION motionTarget{};
    motionTarget.pResource = session.motion.Get();
    motionTarget.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    session.list->CopyTextureRegion(&depthTarget, 0, 0, 0, &depthSource, nullptr);
    session.list->CopyTextureRegion(&motionTarget, 0, 0, 0, &motionSource, nullptr);
    barrier(session.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(session.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    session.currentGuideFrame = frameIndex;
    session.uploadedReverseEstimatedDepth = reverseDepth;
    return true;
}

bool publish_live_frame()
{
    if (!session.liveGuides || session.liveData == nullptr ||
        session.liveReadback11 == nullptr) return true;
    if (session.sharedFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
        session.sharedFormat != DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        session.guideState = "live guides currently require an 8-bit SDR mpv surface";
        return true;
    }

    auto* const header = reinterpret_cast<LiveGuideHeader*>(session.liveData);
    if (InterlockedCompareExchange(&header->inputState, 1, 0) != 0)
    {
        ++session.droppedFrames;
        return true;
    }

    session.context11->CopyResource(
        session.liveReadback11.Get(), session.color11.Get());
    if (!wait_for_d3d11_copy())
    {
        InterlockedExchange(&header->inputState, 0);
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(session.context11->Map(
            session.liveReadback11.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        InterlockedExchange(&header->inputState, 0);
        return false;
    }
    uint8_t* const target = session.liveData + sizeof(LiveGuideHeader);
    const uint32_t rowBytes = session.width * 4;
    for (uint32_t y = 0; y < session.height; ++y)
    {
        uint8_t* const targetRow = target + static_cast<size_t>(y) * rowBytes;
        const uint8_t* const sourceRow =
            static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch;
        if (session.sharedFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
            std::memcpy(targetRow, sourceRow, rowBytes);
        else
        {
            const auto* const packed = reinterpret_cast<const uint32_t*>(sourceRow);
            for (uint32_t x = 0; x < session.width; ++x)
            {
                const uint32_t value = packed[x];
                targetRow[x * 4 + 0] = static_cast<uint8_t>(
                    ((value & 0x3FFu) * 255u + 511u) / 1023u);
                targetRow[x * 4 + 1] = static_cast<uint8_t>(
                    (((value >> 10) & 0x3FFu) * 255u + 511u) / 1023u);
                targetRow[x * 4 + 2] = static_cast<uint8_t>(
                    (((value >> 20) & 0x3FFu) * 255u + 511u) / 1023u);
                targetRow[x * 4 + 3] = 255;
            }
        }
    }
    session.context11->Unmap(session.liveReadback11.Get(), 0);
    const LONG64 sequence = static_cast<LONG64>(++session.publishedSequence);
    InterlockedExchange64(&header->inputSequence, sequence);
    MemoryBarrier();
    InterlockedExchange(&header->inputState, 2);
    return true;
}

bool upload_live_guides(bool reverseDepth, bool& reset, bool& updated)
{
    updated = false;
    reset = false;
    if (!session.liveGuides || session.liveData == nullptr) return true;
    auto* const header = reinterpret_cast<LiveGuideHeader*>(session.liveData);
    if (InterlockedCompareExchange(&header->outputState, 3, 2) != 2)
        return true;

    const uint64_t sequence = static_cast<uint64_t>(header->outputSequence);
    if (sequence <= session.consumedSequence)
    {
        InterlockedExchange(&header->outputState, 0);
        return true;
    }
    const uint64_t pixels = static_cast<uint64_t>(session.width) * session.height;
    const uint8_t* const guide = session.liveData + sizeof(LiveGuideHeader) +
        header->frameBytes;
    std::memcpy(
        session.liveDepth.data(), guide,
        static_cast<size_t>(pixels * sizeof(float)));
    std::memcpy(
        session.liveMotion.data(), guide + pixels * sizeof(float),
        static_cast<size_t>(pixels * sizeof(uint16_t) * 2));
    reset = header->reset != 0 ||
        (session.consumedSequence != 0 &&
         sequence > session.consumedSequence + 2);
    session.consumedSequence = sequence;
    const bool firstLiveGuide = !session.liveGuideReady;
    session.liveGuideReady = true;
    session.guideState = "real-time guides active; generated " +
        std::to_string(header->generatedCount) + ", latest " +
        std::to_string(header->processingMicroseconds / 1000) + " ms, dropped " +
        std::to_string(session.droppedFrames) + " capture frames";
    InterlockedExchange(&header->outputState, 0);
    if (firstLiveGuide)
        reshade::log::message(
            reshade::log::level::info,
            "DLSS 5 Video Renderer consumed its first real-time guide frame.");

    const uint32_t tightPitch = session.width * sizeof(float);
    for (uint32_t y = 0; y < session.height; ++y)
    {
        auto* const targetDepth = reinterpret_cast<float*>(
            session.depthUploadData +
            static_cast<size_t>(y) * session.depthFootprint.Footprint.RowPitch);
        const float* const sourceDepth = session.liveDepth.data() +
            static_cast<size_t>(y) * session.width;
        if (!reverseDepth)
            std::memcpy(targetDepth, sourceDepth, tightPitch);
        else
            for (uint32_t x = 0; x < session.width; ++x)
                targetDepth[x] = 1.0f - sourceDepth[x];
        std::memcpy(
            session.motionUploadData +
                static_cast<size_t>(y) * session.motionFootprint.Footprint.RowPitch,
            session.liveMotion.data() +
                static_cast<size_t>(y) * session.width * 2,
            tightPitch);
    }

    barrier(session.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(session.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION depthSource{};
    depthSource.pResource = session.depthUpload.Get();
    depthSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    depthSource.PlacedFootprint = session.depthFootprint;
    D3D12_TEXTURE_COPY_LOCATION depthTarget{};
    depthTarget.pResource = session.depth.Get();
    depthTarget.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION motionSource{};
    motionSource.pResource = session.motionUpload.Get();
    motionSource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    motionSource.PlacedFootprint = session.motionFootprint;
    D3D12_TEXTURE_COPY_LOCATION motionTarget{};
    motionTarget.pResource = session.motion.Get();
    motionTarget.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    session.list->CopyTextureRegion(&depthTarget, 0, 0, 0, &depthSource, nullptr);
    session.list->CopyTextureRegion(&motionTarget, 0, 0, 0, &motionSource, nullptr);
    barrier(session.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(session.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    session.uploadedReverseEstimatedDepth = reverseDepth;
    updated = true;
    return true;
}

uint32_t pack_preview_pixel(float red, float green, float blue)
{
    red = std::clamp(red, 0.0f, 1.0f);
    green = std::clamp(green, 0.0f, 1.0f);
    blue = std::clamp(blue, 0.0f, 1.0f);
    if (session.sharedFormat == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        const uint32_t r = static_cast<uint32_t>(red * 1023.0f + 0.5f);
        const uint32_t g = static_cast<uint32_t>(green * 1023.0f + 0.5f);
        const uint32_t b = static_cast<uint32_t>(blue * 1023.0f + 0.5f);
        return r | (g << 10) | (b << 20) | (3u << 30);
    }
    const uint32_t r = static_cast<uint32_t>(red * 255.0f + 0.5f);
    const uint32_t g = static_cast<uint32_t>(green * 255.0f + 0.5f);
    const uint32_t b = static_cast<uint32_t>(blue * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

bool show_guide_preview(ID3D11Texture2D* backBuffer, int debugView)
{
    if ((!session.liveGuideReady && session.guideData == nullptr) ||
        session.guidePreview11 == nullptr ||
        (debugView != 2 && debugView != 3)) return false;

    const float* depth = session.liveGuides ? session.liveDepth.data() : nullptr;
    const uint16_t* motion = session.liveGuides ? session.liveMotion.data() : nullptr;
    if (!session.liveGuides)
    {
        if (session.currentGuideFrame == std::numeric_limits<uint32_t>::max())
            return false;
        const uint8_t* const frame = guide_frame_data(session.currentGuideFrame);
        depth = reinterpret_cast<const float*>(frame + sizeof(uint32_t));
        motion = reinterpret_cast<const uint16_t*>(
            frame + sizeof(uint32_t) +
            static_cast<uint64_t>(session.width) * session.height * sizeof(float));
    }
    const size_t pixels = static_cast<size_t>(session.width) * session.height;
    for (size_t index = 0; index < pixels; ++index)
    {
        if (debugView == 2)
        {
            const float value = std::clamp(depth[index], 0.0f, 1.0f);
            session.guidePreview[index] = pack_preview_pixel(value, value, value);
        }
        else
        {
            const float x = half_to_float(motion[index * 2]);
            const float y = half_to_float(motion[index * 2 + 1]);
            const float magnitude = std::sqrt(x * x + y * y);
            session.guidePreview[index] = pack_preview_pixel(
                0.5f + x / 64.0f,
                0.5f + y / 64.0f,
                magnitude / 32.0f);
        }
    }
    session.context11->UpdateSubresource(
        session.guidePreview11.Get(), 0, nullptr,
        session.guidePreview.data(), session.width * sizeof(uint32_t), 0);
    if (session.width == session.outputWidth &&
        session.height == session.outputHeight)
        session.context11->CopyResource(backBuffer, session.guidePreview11.Get());
    else if (!scale_d3d11_texture(
            session.guidePreview11.Get(), backBuffer,
            session.previewScalerEnumerator.Get(), session.previewScaler.Get()))
        return false;
    return wait_for_d3d11_copy();
}

std::filesystem::path find_core()
{
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory));
    const auto root = std::filesystem::path(windowsDirectory) /
        L"System32" / L"DriverStore" / L"FileRepository";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
    {
        if (!entry.is_directory(error)) continue;
        const auto name = entry.path().filename().wstring();
        if (!name.starts_with(L"nv_dispi.inf_amd64_")) continue;
        const auto candidate = entry.path() / L"_nvngx.dll";
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

bool preload_runtime(HMODULE module)
{
    session.runtimeDirectory = addon_directory(module);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(session.runtimeDirectory.c_str());
    for (const wchar_t* name : {L"nvngx_dlss.dll", L"nvngx_dlssnr.dll"})
    {
        if (LoadLibraryExW((session.runtimeDirectory / name).c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) == nullptr)
        {
            log_error("DLSS 5 Video Renderer could not load its private NGX runtime.");
            return false;
        }
    }

    const auto corePath = find_core();
    session.core = corePath.empty() ? nullptr : LoadLibraryExW(
        corePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (session.core == nullptr)
    {
        log_error("DLSS 5 Video Renderer could not load the NVIDIA NGX core.");
        return false;
    }

    session.preloaded = true;
    session.preloadTime = GetTickCount64();
    session.state = "NGX runtime loaded; allowing RenoDX hooks to arm";
    reshade::log::message(reshade::log::level::info,
        "DLSS 5 Video Renderer preloaded NGX and is waiting for hooks to arm.");
    return true;
}

bool initialize(
    ID3D11Device* hostDevice,
    HMODULE module,
    uint32_t backBufferWidth,
    uint32_t backBufferHeight,
    DXGI_FORMAT backBufferFormat)
{
    session.attempted = true;
    if (!load_guide_pack(module))
    {
        log_error("DLSS 5 Video Renderer could not load its guide pack.");
        return false;
    }
    const uint32_t width = (session.guideData != nullptr || session.liveGuides)
        ? session.guideHeader.width : backBufferWidth;
    const uint32_t height = (session.guideData != nullptr || session.liveGuides)
        ? session.guideHeader.height : backBufferHeight;
    session.width = width;
    session.height = height;
    session.outputWidth = backBufferWidth;
    session.outputHeight = backBufferHeight;
    session.sharedFormat = backBufferFormat;
    if (backBufferWidth < width || backBufferHeight < height)
    {
        session.state = "Window is smaller than the guide resolution (" +
            std::to_string(width) + "x" + std::to_string(height) +
            "); showing mpv's native output until the window is enlarged.";
        reshade::log::message(
            reshade::log::level::info, session.state.c_str());
        return true;
    }

    if (session.device == nullptr)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(hostDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
            FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(D3D12CreateDevice(
                adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&session.device))))
        {
            log_error("DLSS 5 Video Renderer could not create its D3D12 device.");
            return false;
        }
    }

    const auto init = import<InitExt>(session.core, "NVSDK_NGX_D3D12_Init_Ext");
    const auto allocate = import<AllocateParameters>(session.core, "NVSDK_NGX_D3D12_AllocateParameters");
    session.createFeature = import<CreateFeature>(session.core, "NVSDK_NGX_D3D12_CreateFeature");
    session.evaluateFeature = import<EvaluateFeature>(session.core, "NVSDK_NGX_D3D12_EvaluateFeature");
    session.releaseFeature = import<ReleaseFeature>(session.core, "NVSDK_NGX_D3D12_ReleaseFeature");
    session.destroyParameters = import<DestroyParameters>(
        session.core, "NVSDK_NGX_D3D12_DestroyParameters");
    if (init == nullptr || allocate == nullptr ||
        session.createFeature == nullptr || session.evaluateFeature == nullptr ||
        session.releaseFeature == nullptr || session.destroyParameters == nullptr)
    {
        log_error("DLSS 5 Video Renderer is missing required NGX functions.");
        return false;
    }

    if (!session.ngxInitialized)
    {
        bool initialized = false;
        for (int version = 0x13; version <= 0x16 && !initialized; ++version)
        {
            DWORD exception = 0;
            const NgxResult result = safe_init(
                init, session.runtimeDirectory.c_str(), session.device.Get(), version, exception);
            if (exception != 0) break;
            initialized = result == NgxSuccess;
        }
        session.ngxInitialized = initialized;
    }
    if (!session.ngxInitialized ||
        allocate(&session.parameters) != NgxSuccess || session.parameters == nullptr)
    {
        log_error("DLSS 5 Video Renderer could not initialize NGX.");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(session.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&session.queue))) ||
        FAILED(session.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&session.allocator))) ||
        FAILED(session.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, session.allocator.Get(), nullptr,
            IID_PPV_ARGS(&session.list))) ||
        FAILED(session.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&session.fence))))
    {
        log_error("DLSS 5 Video Renderer could not create command resources.");
        return false;
    }
    session.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (session.eventHandle == nullptr) return false;

    hostDevice->GetImmediateContext(&session.context11);
    D3D11_QUERY_DESC queryDesc{};
    queryDesc.Query = D3D11_QUERY_EVENT;
    if (!session.context11 || FAILED(hostDevice->CreateQuery(&queryDesc, &session.copyQuery)))
    {
        log_error("DLSS 5 Video Renderer could not create its D3D11 copy synchronization.");
        return false;
    }
    if (FAILED(hostDevice->QueryInterface(IID_PPV_ARGS(&session.videoDevice))) ||
        FAILED(session.context11->QueryInterface(IID_PPV_ARGS(&session.videoContext))) ||
        !make_video_scaler(
            backBufferWidth, backBufferHeight, width, height,
            session.inputScalerEnumerator, session.inputScaler) ||
        !make_video_scaler(
            width, height, backBufferWidth, backBufferHeight,
            session.previewScalerEnumerator, session.previewScaler))
    {
        log_error("DLSS 5 Video Renderer could not create its resize processors.");
        return false;
    }

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_R32_FLOAT;
    D3D12_CLEAR_VALUE motionClear{};
    motionClear.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (!make_shared_texture(
            hostDevice, width, height, backBufferFormat, false,
            session.color, session.color11, session.colorShared) ||
        !make_shared_texture(
            hostDevice, backBufferWidth, backBufferHeight, backBufferFormat, true,
            session.output, session.output11, session.outputShared))
    {
        log_error("DLSS 5 Video Renderer could not share mpv frame textures with D3D12.");
        return false;
    }
    session.depth = make_texture(width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &depthClear);
    session.motion = make_texture(width, height, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &motionClear);
    session.zeroDepth = make_texture(width, height, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &depthClear);
    session.zeroMotion = make_texture(width, height, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &motionClear);
    if (!session.color || !session.output || !session.depth || !session.motion ||
        !session.zeroDepth || !session.zeroMotion)
    {
        log_error("DLSS 5 Video Renderer could not create NGX textures.");
        return false;
    }
    if (session.guideData != nullptr || session.liveGuides)
    {
        const D3D12_RESOURCE_DESC depthDesc = session.depth->GetDesc();
        const D3D12_RESOURCE_DESC motionDesc = session.motion->GetDesc();
        UINT64 depthBytes = 0;
        UINT64 motionBytes = 0;
        session.device->GetCopyableFootprints(
            &depthDesc, 0, 1, 0, &session.depthFootprint,
            nullptr, nullptr, &depthBytes);
        session.device->GetCopyableFootprints(
            &motionDesc, 0, 1, 0, &session.motionFootprint,
            nullptr, nullptr, &motionBytes);
        if (!make_upload_buffer(
                depthBytes, session.depthUpload, session.depthUploadData) ||
            !make_upload_buffer(
                motionBytes, session.motionUpload, session.motionUploadData))
        {
            log_error("DLSS 5 Video Renderer could not create guide upload buffers.");
            return false;
        }

        D3D11_TEXTURE2D_DESC previewDesc{};
        previewDesc.Width = width;
        previewDesc.Height = height;
        previewDesc.MipLevels = 1;
        previewDesc.ArraySize = 1;
        previewDesc.Format = backBufferFormat;
        previewDesc.SampleDesc.Count = 1;
        previewDesc.Usage = D3D11_USAGE_DEFAULT;
        previewDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hostDevice->CreateTexture2D(
                &previewDesc, nullptr, &session.guidePreview11)))
        {
            log_error("DLSS 5 Video Renderer could not create its guide preview texture.");
            return false;
        }
        session.guidePreview.resize(static_cast<size_t>(width) * height);

        if (session.liveGuides)
        {
            D3D11_TEXTURE2D_DESC readbackDesc{};
            readbackDesc.Width = width;
            readbackDesc.Height = height;
            readbackDesc.MipLevels = 1;
            readbackDesc.ArraySize = 1;
            readbackDesc.Format = backBufferFormat;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Usage = D3D11_USAGE_STAGING;
            readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(hostDevice->CreateTexture2D(
                    &readbackDesc, nullptr, &session.liveReadback11)))
            {
                log_error("DLSS 5 Video Renderer could not create its live-frame readback.");
                return false;
            }
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 4;
    if (FAILED(session.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&session.rtvHeap))))
        return false;
    auto rtv = session.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = session.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto pair : {std::pair{session.depth.Get(), depthClear},
                      std::pair{session.motion.Get(), motionClear},
                      std::pair{session.zeroDepth.Get(), depthClear},
                      std::pair{session.zeroMotion.Get(), motionClear}})
    {
        session.device->CreateRenderTargetView(pair.first, nullptr, rtv);
        session.list->ClearRenderTargetView(rtv, pair.second.Color, 0, nullptr);
        rtv.ptr += stride;
    }
    barrier(session.depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(session.motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(session.zeroDepth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(session.zeroMotion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    session.parameters->Set("Width", width);
    session.parameters->Set("Height", height);
    session.parameters->Set("OutWidth", backBufferWidth);
    session.parameters->Set("OutHeight", backBufferHeight);
    session.parameters->Set("PerfQualityValue", 5);
    // SDR input, full-resolution pixel-space motion and inverted relative depth.
    // This mirrors the working game contract without its HDR bit.
    session.parameters->Set("DLSS.Feature.Create.Flags", 74);
    session.parameters->Set("DLSS.Enable.Output.Subrects", 1);
    session.parameters->Set("CreationNodeMask", 1u);
    session.parameters->Set("VisibilityNodeMask", 1u);
    session.parameters->Set("RTXValue", 0);

    DWORD exception = 0;
    const NgxResult createResult = safe_create(
        session.createFeature, session.list.Get(), session.parameters,
        &session.feature, exception);
    if (exception != 0 || createResult != NgxSuccess || session.feature == nullptr ||
        !submit_and_wait())
    {
        log_error("DLSS 5 Video Renderer NGX feature creation failed.");
        return false;
    }

    session.parameters->Set("Color", session.color.Get());
    session.parameters->Set("Output", session.output.Get());
    session.parameters->Set("Depth", session.depth.Get());
    session.parameters->Set("MotionVectors", session.motion.Get());
    session.parameters->Set("DLSS.Render.Subrect.Dimensions.Width", width);
    session.parameters->Set("DLSS.Render.Subrect.Dimensions.Height", height);
    session.parameters->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    session.parameters->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    session.parameters->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    session.parameters->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    session.parameters->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
    session.parameters->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
    session.parameters->Set("DLSS.Output.Subrect.Base.X", 0u);
    session.parameters->Set("DLSS.Output.Subrect.Base.Y", 0u);
    session.parameters->Set("MV.Scale.X", 1.0f);
    session.parameters->Set("MV.Scale.Y", 1.0f);
    session.parameters->Set("Jitter.Offset.X", 0.0f);
    session.parameters->Set("Jitter.Offset.Y", 0.0f);
    session.parameters->Set("Sharpness", 0.0f);
    session.parameters->Set("DLSS.Pre.Exposure", 1.0f);
    session.parameters->Set("DLSS.Exposure.Scale", 1.0f);
    session.ready = true;
    session.state = "NGX transport active: " +
        std::to_string(width) + "x" + std::to_string(height) + " -> " +
        std::to_string(backBufferWidth) + "x" +
        std::to_string(backBufferHeight);
    reshade::log::message(reshade::log::level::info,
        "DLSS 5 Video Renderer created its persistent NGX transport.");
    return true;
}
}

namespace live_ngx
{
bool evaluate_transport(
    ID3D11Device* hostDevice,
    ID3D11Texture2D* backBuffer,
    HMODULE module,
    uint32_t width,
    uint32_t height,
    bool applyOutput,
    int debugView,
    uint64_t presentIndex,
    bool holdGuide,
    bool useEstimatedDepth,
    bool useEstimatedMotion,
    bool reverseEstimatedDepth)
{
    if (!session.preloaded) return preload_runtime(module);
    if (GetTickCount64() - session.preloadTime < 250) return true;
    D3D11_TEXTURE2D_DESC backBufferDesc{};
    backBuffer->GetDesc(&backBufferDesc);
    if (backBufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        backBufferDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        log_error("DLSS 5 Video Renderer encountered an unsupported mpv swapchain format.");
        return false;
    }
    if (session.attempted &&
        (session.outputWidth != width || session.outputHeight != height ||
         session.sharedFormat != backBufferDesc.Format))
    {
        reset_transport();
        reshade::log::message(reshade::log::level::info,
            "DLSS 5 Video Renderer rebuilding after a window resize.");
    }
    if (!session.attempted && !initialize(
            hostDevice, module, width, height, backBufferDesc.Format)) return false;
    if (!session.ready || session.outputWidth != width ||
        session.outputHeight != height ||
        session.sharedFormat != backBufferDesc.Format) return false;
    if (session.width == width && session.height == height)
        session.context11->CopyResource(session.color11.Get(), backBuffer);
    else if (!scale_d3d11_texture(
            backBuffer, session.color11.Get(),
            session.inputScalerEnumerator.Get(), session.inputScaler.Get()))
    {
        log_error("DLSS 5 Video Renderer could not scale mpv's frame for NGX.");
        session.ready = false;
        return false;
    }
    if (!wait_for_d3d11_copy())
    {
        log_error("DLSS 5 Video Renderer timed out copying mpv's frame.");
        session.ready = false;
        return false;
    }
    if (!publish_live_frame())
    {
        log_error("DLSS 5 Video Renderer could not publish a frame for live guides.");
        return false;
    }

    if (FAILED(session.allocator->Reset()) ||
        FAILED(session.list->Reset(session.allocator.Get(), nullptr))) return false;
    bool guideReset = false;
    bool liveGuideUpdated = false;
    const bool holdModeChanged = holdGuide != session.holdingGuide;
    if (session.guideData != nullptr)
    {
        uint32_t guideFrame = static_cast<uint32_t>(
            presentIndex % session.guideHeader.frameCount);
        if (holdGuide && session.holdingGuide &&
            session.currentGuideFrame != std::numeric_limits<uint32_t>::max())
            guideFrame = session.currentGuideFrame;
        if (!upload_guide_frame(
                guideFrame, reverseEstimatedDepth, guideReset))
        {
            log_error("DLSS 5 Video Renderer could not upload its guide frame.");
            session.ready = false;
            return false;
        }
    }
    else if (session.liveGuides &&
             !upload_live_guides(
                 reverseEstimatedDepth, guideReset, liveGuideUpdated))
    {
        log_error("DLSS 5 Video Renderer could not upload its live guides.");
        return false;
    }
    session.holdingGuide = holdGuide;
    barrier(session.color.Get(), D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(session.output.Get(), D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool guidesAvailable = session.guideData != nullptr ||
        session.liveGuideReady;
    ID3D12Resource* const selectedDepth =
        useEstimatedDepth && guidesAvailable
            ? session.depth.Get() : session.zeroDepth.Get();
    ID3D12Resource* const selectedMotion =
        useEstimatedMotion && guidesAvailable && !holdGuide &&
            (!session.liveGuides || liveGuideUpdated)
            ? session.motion.Get() : session.zeroMotion.Get();
    session.parameters->Set("Depth", selectedDepth);
    session.parameters->Set("MotionVectors", selectedMotion);

    ID3D12Resource* boundDepth = nullptr;
    ID3D12Resource* boundMotion = nullptr;
    const bool depthMatches =
        session.parameters->Get("Depth", &boundDepth) == NgxSuccess &&
        boundDepth == selectedDepth;
    const bool motionMatches =
        session.parameters->Get("MotionVectors", &boundMotion) == NgxSuccess &&
        boundMotion == selectedMotion;
    if (depthMatches && motionMatches)
    {
        const bool estimatedDepth =
            useEstimatedDepth && guidesAvailable;
        const bool estimatedMotion =
            useEstimatedMotion && guidesAvailable && !holdGuide &&
            (!session.liveGuides || liveGuideUpdated);
        session.guideBindingState =
            std::string(estimatedDepth
                ? (reverseEstimatedDepth ? "reversed estimated depth" : "estimated depth")
                : "blank depth") +
            " and " + (estimatedMotion ? "estimated motion" : "blank motion") +
            " textures bound" + (holdGuide ? " for frozen frame" : "");
    }
    else
    {
        session.guideBindingState =
            "NGX parameter pointers do not match the selected guide textures";
    }

    const bool guideModeChanged =
        useEstimatedDepth != session.lastUseEstimatedDepth ||
        useEstimatedMotion != session.lastUseEstimatedMotion ||
        reverseEstimatedDepth != session.lastReverseEstimatedDepth;
    session.lastUseEstimatedDepth = useEstimatedDepth;
    session.lastUseEstimatedMotion = useEstimatedMotion;
    session.lastReverseEstimatedDepth = reverseEstimatedDepth;
    session.parameters->Set("Reset",
        (session.evaluations == 0 || guideReset || guideModeChanged ||
         holdModeChanged) ? 1 : 0);
    DWORD exception = 0;
    const NgxResult result = safe_evaluate(
        session.evaluateFeature, session.list.Get(), session.feature,
        session.parameters, exception);
    barrier(session.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COMMON);
    barrier(session.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);
    if (exception != 0 || result != NgxSuccess || !submit_and_wait())
    {
        log_error("DLSS 5 Video Renderer NGX evaluation failed; transport disabled.");
        session.ready = false;
        return false;
    }
    ++session.evaluations;
    if (applyOutput)
    {
        session.context11->CopyResource(backBuffer, session.output11.Get());
        if (!wait_for_d3d11_copy())
        {
            log_error("DLSS 5 Video Renderer timed out applying NGX output to mpv.");
            session.ready = false;
            return false;
        }
    }
    else if ((session.guideData != nullptr || session.liveGuideReady) &&
             (debugView == 2 || debugView == 3) &&
             !show_guide_preview(backBuffer, debugView))
    {
        log_error("DLSS 5 Video Renderer could not display its guide preview.");
        return false;
    }
    if (session.evaluations == 1)
        reshade::log::message(reshade::log::level::info,
            "DLSS 5 Video Renderer completed its first live mpv NGX evaluation.");
    return true;
}

uint64_t evaluation_count() { return session.evaluations; }
uint32_t guide_frame_index()
{
    if (session.liveGuides)
        return guide_frame_count();
    return session.currentGuideFrame == std::numeric_limits<uint32_t>::max()
        ? 0 : session.currentGuideFrame + 1;
}
uint32_t guide_frame_count()
{
    if (session.liveGuides && session.liveData != nullptr)
    {
        const auto* const header =
            reinterpret_cast<const LiveGuideHeader*>(session.liveData);
        return static_cast<uint32_t>(std::min<uint64_t>(
            header->generatedCount, std::numeric_limits<uint32_t>::max()));
    }
    return session.guideHeader.frameCount;
}
const char* guide_status() { return session.guideState.c_str(); }
const char* guide_binding_status() { return session.guideBindingState.c_str(); }
const char* status() { return session.state.c_str(); }
}

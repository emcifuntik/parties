#include "system_diagnostics.h"

#include <parties/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxcore.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace parties::client {
namespace {

std::string wide_to_utf8(const wchar_t* text) {
    if (!text || text[0] == L'\0') return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size,
                        nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string cpu_brand() {
    std::array<int, 4> registers{};
    __cpuid(registers.data(), 0x80000000);
    const unsigned int highest_leaf = static_cast<unsigned int>(registers[0]);
    if (highest_leaf < 0x80000004) return "unknown";

    std::array<char, 49> brand{};
    for (unsigned int leaf = 0; leaf < 3; ++leaf) {
        __cpuid(registers.data(), static_cast<int>(0x80000002 + leaf));
        std::memcpy(brand.data() + leaf * 16, registers.data(), 16);
    }
    std::string result(brand.data());
    const auto first = result.find_first_not_of(' ');
    const auto last = result.find_last_not_of(' ');
    return first == std::string::npos ? "unknown" : result.substr(first, last - first + 1);
}

const char* vendor_name(UINT vendor_id) {
    switch (vendor_id) {
    case 0x1002: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    default: return "unknown";
    }
}

IDXCoreAdapterFactory* dxcore_factory() {
    static Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
    static const bool initialized = [] {
        // Load dynamically so the client still starts on older Windows 10
        // installations which do not ship dxcore.dll.
        const HMODULE module = LoadLibraryW(L"dxcore.dll");
        if (!module) return false;
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        const auto create_factory = reinterpret_cast<CreateFactoryFn>(
            GetProcAddress(module, "DXCoreCreateAdapterFactory"));
        if (!create_factory) return false;
        return SUCCEEDED(create_factory(IID_PPV_ARGS(&factory)));
    }();
    return initialized ? factory.Get() : nullptr;
}

std::string driver_version(const LUID& luid) {
    IDXCoreAdapterFactory* factory = dxcore_factory();
    if (!factory) return "unknown";
    Microsoft::WRL::ComPtr<IDXCoreAdapter> adapter;
    if (FAILED(factory->GetAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
        !adapter->IsPropertySupported(DXCoreAdapterProperty::DriverVersion)) {
        return "unknown";
    }

    uint64_t version = 0;
    if (FAILED(adapter->GetProperty(DXCoreAdapterProperty::DriverVersion,
                                    sizeof(version), &version))) {
        return "unknown";
    }
    const uint32_t high = static_cast<uint32_t>(version >> 32u);
    const uint32_t low = static_cast<uint32_t>(version);
    return std::to_string(HIWORD(high)) + "." +
           std::to_string(LOWORD(high)) + "." +
           std::to_string(HIWORD(low)) + "." +
           std::to_string(LOWORD(low));
}

} // namespace

void log_windows_system_diagnostics() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
                GetProcAddress(ntdll, "RtlGetVersion"))) {
            rtl_get_version(&version);
        }
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    const DWORD logical_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    LOG_INFO("System: Windows {}.{}.{}; CPU='{}'; logical_processors={}; memory_mb={}; remote_session={}",
             version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
             cpu_brand(), logical_processors,
             memory.ullTotalPhys / (1024ull * 1024ull),
             GetSystemMetrics(SM_REMOTESESSION) != 0);

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    const HRESULT factory_result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factory_result)) {
        LOG_ERROR("DXGI diagnostics: CreateDXGIFactory1 failed (HRESULT=0x{:08x})",
                  static_cast<uint32_t>(factory_result));
        return;
    }

    UINT adapter_count = 0;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            LOG_ERROR("DXGI diagnostics: EnumAdapters1({}) failed (HRESULT=0x{:08x})",
                      index, static_cast<uint32_t>(result));
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        ++adapter_count;

        UINT output_count = 0;
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        while (SUCCEEDED(adapter->EnumOutputs(output_count, &output))) {
            ++output_count;
            output.Reset();
        }

        uint64_t local_budget_mb = 0;
        uint64_t local_usage_mb = 0;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory_info{};
        if (SUCCEEDED(adapter.As(&adapter3)) &&
            SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory_info))) {
            local_budget_mb = memory_info.Budget / (1024ull * 1024ull);
            local_usage_mb = memory_info.CurrentUsage / (1024ull * 1024ull);
        }

        LOG_INFO("GPU[{}]: name='{}'; vendor={} (0x{:04x}); device=0x{:04x}; "
                 "dedicated_vram_mb={}; shared_memory_mb={}; budget_mb={}; usage_mb={}; "
                 "outputs={}; driver={}; software={}",
                 index, wide_to_utf8(desc.Description), vendor_name(desc.VendorId),
                 desc.VendorId, desc.DeviceId,
                 desc.DedicatedVideoMemory / (1024ull * 1024ull),
                 desc.SharedSystemMemory / (1024ull * 1024ull),
                 local_budget_mb, local_usage_mb, output_count,
                 driver_version(desc.AdapterLuid),
                 (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);
    }

    if (adapter_count == 0)
        LOG_WARN("DXGI diagnostics: no display adapters were enumerated");
}

} // namespace parties::client

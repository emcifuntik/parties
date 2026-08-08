#include "amf_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <parties/log.h>

#include <mutex>
#include <string>

namespace parties::encdec::amd {

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
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
        result.pop_back();
    return result;
}

class PartiesAmfTraceWriter final : public amf::AMFTraceWriter {
public:
    void AMF_CDECL_CALL Write(const wchar_t* scope, const wchar_t* message) override {
        LOG_WARN("AMF[{}]: {}", wide_to_utf8(scope), wide_to_utf8(message));
    }

    void AMF_CDECL_CALL Flush() override {
        if (auto logger = spdlog::default_logger()) logger->flush();
    }
};

PartiesAmfTraceWriter g_trace_writer;

} // namespace

bool load_amf(amf::AMFFactory*& factory) {
    static std::once_flag load_once;
    static bool ok = false;
    static amf::AMFFactory* cached_factory = nullptr;
    std::call_once(load_once, [] {
        HMODULE dll = LoadLibraryA(AMF_DLL_NAMEA);
        if (!dll) {
            LOG_INFO("AMF runtime is unavailable (LoadLibrary error={})", GetLastError());
            return;
        }

        auto init_fn = reinterpret_cast<AMFInit_Fn>(
            GetProcAddress(dll, AMF_INIT_FUNCTION_NAME));
        if (!init_fn) {
            LOG_ERROR("AMF runtime is missing {} (GetProcAddress error={})",
                      AMF_INIT_FUNCTION_NAME, GetLastError());
            return;
        }

        amf_uint64 runtime_version = 0;
        if (const auto query_version = reinterpret_cast<AMFQueryVersion_Fn>(
                GetProcAddress(dll, AMF_QUERY_VERSION_FUNCTION_NAME))) {
            query_version(&runtime_version);
        }

        const AMF_RESULT result = init_fn(AMF_FULL_VERSION, &cached_factory);
        if (result != AMF_OK || !cached_factory) {
            LOG_ERROR("AMFInit failed: result={}", static_cast<int>(result));
            return;
        }

        amf::AMFTrace* trace = nullptr;
        if (cached_factory->GetTrace(&trace) == AMF_OK && trace) {
            constexpr wchar_t writer_id[] = L"Parties";
            trace->RegisterWriter(writer_id, &g_trace_writer, true);
            trace->SetWriterLevel(writer_id, AMF_TRACE_WARNING);
        }

        LOG_INFO("AMF runtime loaded: {}.{}.{}.{} (compiled against {}.{}.{}.{})",
                 AMF_GET_MAJOR_VERSION(runtime_version),
                 AMF_GET_MINOR_VERSION(runtime_version),
                 AMF_GET_SUBMINOR_VERSION(runtime_version),
                 AMF_GET_BUILD_VERSION(runtime_version),
                 AMF_VERSION_MAJOR, AMF_VERSION_MINOR,
                 AMF_VERSION_RELEASE, AMF_VERSION_BUILD_NUM);
        ok = true;
    });
    factory = cached_factory;
    return ok;
}

} // namespace parties::encdec::amd

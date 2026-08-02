#pragma once

#include <AMF/core/Result.h>

namespace parties::encdec::amd {

// AMF does not take ownership of an input when its queue/surface pool is full.
// Keep the exact buffer alive and submit it again after polling output.
enum class AmfSubmitDisposition {
    Accepted,
    RetrySameInput,
    RetryWithoutInput,
    ResolutionChanged,
    Fatal,
};

constexpr AmfSubmitDisposition classify_amf_submit_result(AMF_RESULT result) {
    switch (result) {
    case AMF_OK:
    case AMF_NEED_MORE_INPUT:
        return AmfSubmitDisposition::Accepted;
    case AMF_INPUT_FULL:
    case AMF_DECODER_NO_FREE_SURFACES:
        return AmfSubmitDisposition::RetrySameInput;
    case AMF_REPEAT:
        return AmfSubmitDisposition::RetryWithoutInput;
    case AMF_RESOLUTION_CHANGED:
        return AmfSubmitDisposition::ResolutionChanged;
    default:
        return AmfSubmitDisposition::Fatal;
    }
}

} // namespace parties::encdec::amd

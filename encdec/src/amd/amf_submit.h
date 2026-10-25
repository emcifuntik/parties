#pragma once

#include <AMF/core/Result.h>

namespace parties::encdec::amd {

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

enum class AmfEncoderSubmit {
    Accepted,
    RetrySameSurface,
    Fatal,
};

constexpr AmfEncoderSubmit classify_amf_encoder_submit(AMF_RESULT result) {
    switch (result) {
    case AMF_OK:
        return AmfEncoderSubmit::Accepted;
    case AMF_INPUT_FULL:
        return AmfEncoderSubmit::RetrySameSurface;
    default:
        return AmfEncoderSubmit::Fatal;
    }
}

}

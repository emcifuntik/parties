#include "amd/amf_decode_state.h"

#include <iostream>

using parties::encdec::amd::AmfSubmitDisposition;
using parties::encdec::amd::classify_amf_submit_result;

namespace {

bool expect(AMF_RESULT result, AmfSubmitDisposition expected, const char* name) {
    const auto actual = classify_amf_submit_result(result);
    if (actual == expected) return true;
    std::cerr << name << ": unexpected AMF submit disposition\n";
    return false;
}

} // namespace

int main() {
    bool ok = true;
    ok &= expect(AMF_OK, AmfSubmitDisposition::Accepted, "AMF_OK");
    ok &= expect(AMF_NEED_MORE_INPUT, AmfSubmitDisposition::Accepted, "AMF_NEED_MORE_INPUT");
    ok &= expect(AMF_INPUT_FULL, AmfSubmitDisposition::RetrySameInput, "AMF_INPUT_FULL");
    ok &= expect(AMF_DECODER_NO_FREE_SURFACES, AmfSubmitDisposition::RetrySameInput,
                 "AMF_DECODER_NO_FREE_SURFACES");
    ok &= expect(AMF_REPEAT, AmfSubmitDisposition::RetryWithoutInput, "AMF_REPEAT");
    ok &= expect(AMF_RESOLUTION_CHANGED, AmfSubmitDisposition::ResolutionChanged,
                 "AMF_RESOLUTION_CHANGED");
    ok &= expect(AMF_FAIL, AmfSubmitDisposition::Fatal, "AMF_FAIL");
    ok &= expect(AMF_INVALID_DATA_TYPE, AmfSubmitDisposition::Fatal, "AMF_INVALID_DATA_TYPE");
    return ok ? 0 : 1;
}

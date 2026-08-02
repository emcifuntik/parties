#include <client/document_router.h>

#include <cstdio>

using parties::client::DocumentRoute;
using parties::client::DocumentRouter;
using parties::client::SettingsSection;

static_assert(static_cast<int>(SettingsSection::AudioVoice) == 0);
static_assert(static_cast<int>(SettingsSection::ScreenShare) == 1);
static_assert(static_cast<int>(SettingsSection::Hotkeys) == 2);
static_assert(static_cast<int>(SettingsSection::AccountKeys) == 3);

namespace {

bool Check(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "Document router test failed: %s\n", message);
    return condition;
}

} // namespace

int main() {
    DocumentRouter router;
    bool success = Check(router.is(DocumentRoute::Room), "initial route is not room");

    router.go(DocumentRoute::Chat);
    success &= Check(router.is(DocumentRoute::Chat), "chat navigation failed");
    success &= Check(!router.is(DocumentRoute::Settings), "chat and settings routes overlap");

    router.open_settings(SettingsSection::AudioVoice);
    success &= Check(router.is(DocumentRoute::Settings), "settings navigation failed");
    success &= Check(!router.is(DocumentRoute::Chat), "settings did not replace chat");
    router.back();
    success &= Check(router.is(DocumentRoute::Chat), "settings did not return to chat");

    router.go(DocumentRoute::Room);
    router.open_share_picker();
    router.open_settings(SettingsSection::ScreenShare);
    success &= Check(router.settings_section() == SettingsSection::ScreenShare,
        "settings section was not retained");
    router.back();
    success &= Check(router.is(DocumentRoute::SharePicker), "nested Back did not return to share picker");
    router.back();
    success &= Check(router.is(DocumentRoute::Room), "nested Back did not return to room");

    router.go(DocumentRoute::Streams);
    router.open_settings();
    router.leave_streams();
    router.back();
    success &= Check(router.is(DocumentRoute::Room), "Back resurrected a finished stream route");

    router.go(DocumentRoute::Chat);
    router.reset();
    success &= Check(router.is(DocumentRoute::Room), "reset did not restore room route");
    success &= Check(router.settings_section() == SettingsSection::AudioVoice,
        "reset did not restore the default settings section");

    return success ? 0 : 1;
}

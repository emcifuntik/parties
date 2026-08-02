#pragma once

#include <client/rml_binding.h>

#include <RmlUi/Core/Types.h>

#include <algorithm>
#include <vector>

namespace parties::client {

// The connected application is a single RML document, so its top-level pages
// must be mutually exclusive. DocumentRouter is the only owner of that page
// state; feature models may retain their own selection state, but never their
// own independent visibility flag.
enum class DocumentRoute {
    Room,
    Chat,
    Settings,
    SharePicker,
    Streams,
};

enum class SettingsSection {
    AudioVoice,
    ScreenShare,
    Hotkeys,
    AccountKeys,
};

class DocumentRouter {
public:
    static const char* name(DocumentRoute route) {
        switch (route) {
        case DocumentRoute::Room:        return "room";
        case DocumentRoute::Chat:        return "chat";
        case DocumentRoute::Settings:    return "settings";
        case DocumentRoute::SharePicker: return "share-picker";
        case DocumentRoute::Streams:     return "streams";
        }
        return "room";
    }

    bool is(DocumentRoute route) const { return current_.get() == name(route); }
    const Rml::String& current() const { return current_.get(); }
    SettingsSection settings_section() const {
        return static_cast<SettingsSection>(settings_section_.get());
    }

    // Primary navigation replaces the page and starts a fresh history chain.
    void go(DocumentRoute route) {
        history_.clear();
        current_ = name(route);
    }

    // Transient full-document pages preserve their caller for close/back.
    void push(DocumentRoute route) {
        if (is(route)) return;
        history_.push_back(route_from_name(current_.get()));
        current_ = name(route);
    }

    void open_settings(SettingsSection section = SettingsSection::AudioVoice) {
        settings_section_ = static_cast<int>(section);
        push(DocumentRoute::Settings);
    }

    void select_settings(SettingsSection section) {
        settings_section_ = static_cast<int>(section);
        if (!is(DocumentRoute::Settings))
            push(DocumentRoute::Settings);
    }

    void open_share_picker() { push(DocumentRoute::SharePicker); }

    void back(DocumentRoute fallback = DocumentRoute::Room) {
        if (history_.empty()) {
            current_ = name(fallback);
            return;
        }
        current_ = name(history_.back());
        history_.pop_back();
    }

    // A stream can end while a transient route is open. Replace it throughout
    // history as well as at the current position so Back never resurrects a
    // page whose media resources no longer exist.
    void leave_streams() {
        std::replace(history_.begin(), history_.end(), DocumentRoute::Streams, DocumentRoute::Room);
        if (is(DocumentRoute::Streams))
            go(DocumentRoute::Room);
    }

    void reset() {
        history_.clear();
        settings_section_ = static_cast<int>(SettingsSection::AudioVoice);
        current_ = name(DocumentRoute::Room);
    }

    rml::Prop<Rml::String>& route_binding() { return current_; }
    rml::Prop<int>& settings_section_binding() { return settings_section_; }

private:
    static DocumentRoute route_from_name(const Rml::String& route) {
        if (route == name(DocumentRoute::Chat))        return DocumentRoute::Chat;
        if (route == name(DocumentRoute::Settings))    return DocumentRoute::Settings;
        if (route == name(DocumentRoute::SharePicker)) return DocumentRoute::SharePicker;
        if (route == name(DocumentRoute::Streams))     return DocumentRoute::Streams;
        return DocumentRoute::Room;
    }

    rml::Prop<Rml::String> current_{name(DocumentRoute::Room)};
    rml::Prop<int> settings_section_{static_cast<int>(SettingsSection::AudioVoice)};
    std::vector<DocumentRoute> history_;
};

} // namespace parties::client

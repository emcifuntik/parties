#pragma once

#include <client/rml_binding.h>

#include <RmlUi/Core/Types.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

namespace rml = parties::rml;

struct TextSegment {
    Rml::String text;
    bool is_url = false;
};

struct ChatAttachment {
    int64_t id = 0;
    Rml::String file_name;
    Rml::String size_str;       // formatted "2.4 MB"
    Rml::String file_ext;       // "PDF", "PNG" etc. for type badge
    bool uploaded = false;
};

struct ChatMessage {
    int64_t id = 0;
    int sender_id = 0;
    Rml::String sender_name;
    Rml::String initials;       // "SK" from "Sara King"
    bool is_own = false;        // true if sent by current user
    Rml::String text;           // raw text
    bool has_url = false;       // true if text contains a URL (for clickable rendering)
    Rml::Vector<TextSegment> segments;  // text split into plain text + URL segments
    Rml::String timestamp_str;  // "10:32 AM"
    Rml::String date_label;     // "Today", "Yesterday", "Mar 17" — set on first msg of each day
    uint64_t raw_timestamp = 0; // unix timestamp for date grouping (not bound to RmlUi)
    bool pinned = false;
    int color_index = 0;
    Rml::Vector<ChatAttachment> attachments;
};

struct TextChannel {
    int id = 0;
    Rml::String name;
    bool has_unread = false;
};

struct ChatCommandDefinition {
    Rml::String name;
    Rml::String description;
    Rml::String usage;
};

struct PendingFile {
    Rml::String name;       // display name
    Rml::String size_str;   // formatted size
    Rml::String path;       // local file path (for reading data on send)
};

class ChatModel : public rml::Model {
public:
    // --- Bound state (Property<T> auto-dirties; arrays via silent()/notify()) ---
    rml::Prop<Rml::Vector<TextChannel>> text_channels;
    rml::Prop<Rml::Vector<ChatCommandDefinition>> commands;
    rml::Prop<int>         active_channel{0};
    rml::Prop<Rml::String> active_channel_name;

    rml::Prop<Rml::Vector<ChatMessage>> messages;
    rml::Prop<bool>        loading_history{false};
    rml::Prop<bool>        has_more_history{false};

    // Compose
    rml::Prop<Rml::String> compose_text;

    // Search
    rml::Prop<bool>        show_search{false};
    rml::Prop<Rml::String> search_query;
    rml::Prop<Rml::Vector<ChatMessage>> search_results;

    // Pinned
    rml::Prop<bool>        show_pinned{false};
    rml::Prop<Rml::Vector<ChatMessage>> pinned_messages;

    // Pending file attachments
    rml::Prop<Rml::Vector<PendingFile>> pending_files;

    // Create text channel
    rml::Prop<bool>        can_manage_channels{false};
    rml::Prop<bool>        show_create_text_channel{false};
    rml::Prop<Rml::String> new_text_channel_name;

    // --- Callbacks ---
    std::function<void()>        on_send_message;
    std::function<void(int)>     on_select_channel;
    std::function<void()>        on_load_more_history;
    std::function<void(int64_t)> on_pin_message;
    std::function<void(int64_t)> on_unpin_message;
    std::function<void(int64_t)> on_delete_message;
    std::function<void(int64_t)> on_message_context_menu;
    std::function<void(int64_t)> on_download_file;
    std::function<void()>        on_do_search;
    std::function<void()>        on_request_pinned;
    std::function<void()>        on_create_text_channel;
    std::function<void(int)>     on_delete_text_channel;
    std::function<void()>        on_attach_file;
    std::function<void(const std::string&)> on_open_url;

protected:
    const char* model_name() const override { return "chat"; }
    void build(rml::Builder& b) override;
};

} // namespace parties::client

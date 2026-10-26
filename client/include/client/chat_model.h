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
    Rml::String size_str;
    Rml::String file_ext;
    bool uploaded = false;
};

struct ChatMessage {
    int64_t id = 0;
    int sender_id = 0;
    Rml::String sender_name;
    Rml::String initials;
    bool is_own = false;
    Rml::String text;
    bool has_url = false;
    Rml::Vector<TextSegment> segments;
    Rml::String timestamp_str;
    Rml::String date_label;
    uint64_t raw_timestamp = 0;
    bool pinned = false;
    int color_index = 0;
    Rml::Vector<ChatAttachment> attachments;
};

struct TextChannel {
    int id = 0;
    Rml::String name;
    bool has_unread = false;
};

struct PendingFile {
    Rml::String name;
    Rml::String size_str;
    Rml::String path;
};

class ChatModel : public rml::Model {
public:
    rml::Prop<Rml::Vector<TextChannel>> text_channels;
    rml::Prop<int>         active_channel{0};
    rml::Prop<Rml::String> active_channel_name;

    rml::Prop<Rml::Vector<ChatMessage>> messages;
    rml::Prop<bool>        loading_history{false};
    rml::Prop<bool>        has_more_history{false};

    rml::Prop<Rml::String> compose_text;

    rml::Prop<bool>        show_search{false};
    rml::Prop<Rml::String> search_query;
    rml::Prop<Rml::Vector<ChatMessage>> search_results;

    rml::Prop<bool>        show_pinned{false};
    rml::Prop<Rml::Vector<ChatMessage>> pinned_messages;

    rml::Prop<Rml::Vector<PendingFile>> pending_files;

    rml::Prop<bool>        can_manage_channels{false};
    rml::Prop<bool>        show_create_text_channel{false};
    rml::Prop<Rml::String> new_text_channel_name;

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

}

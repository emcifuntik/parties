#pragma once

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <functional>

namespace Rml { class Context; }

namespace parties::client {

struct TextSegment {
    Rml::String text;
    bool is_url = false;
};

struct ChatAttachment {
    int64_t id = 0;
    Rml::String file_name;
    Rml::String size_str;       // formatted "2.4 MB"
    bool uploaded = false;
};

struct ChatMessage {
    int64_t id = 0;
    int sender_id = 0;
    Rml::String sender_name;
    Rml::String text;           // raw text
    Rml::Vector<TextSegment> segments;  // text split into plain text + URL segments
    Rml::String timestamp_str;  // "14:32" or "Mar 17, 14:32"
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
    Rml::String name;       // display name
    Rml::String size_str;   // formatted size
    Rml::String path;       // local file path (for reading data on send)
};

class ChatModel {
public:
    ChatModel();
    ~ChatModel();

    bool init(Rml::Context* context);
    void dirty(const Rml::String& variable);

    // --- Bound state ---
    Rml::Vector<TextChannel> text_channels;
    int active_channel = 0;
    Rml::String active_channel_name;

    Rml::Vector<ChatMessage> messages;
    bool loading_history = false;
    bool has_more_history = false;

    // Compose
    Rml::String compose_text;

    // Search
    bool show_search = false;
    Rml::String search_query;
    Rml::Vector<ChatMessage> search_results;

    // Pinned
    bool show_pinned = false;
    Rml::Vector<ChatMessage> pinned_messages;

    // Pending file attachments
    Rml::Vector<PendingFile> pending_files;

    // Create text channel
    bool show_create_text_channel = false;
    Rml::String new_text_channel_name;

    // --- Callbacks ---
    std::function<void()>        on_send_message;
    std::function<void(int)>     on_select_channel;
    std::function<void()>        on_load_more_history;
    std::function<void(int64_t)> on_pin_message;
    std::function<void(int64_t)> on_unpin_message;
    std::function<void(int64_t)> on_delete_message;
    std::function<void(int64_t)> on_download_file;
    std::function<void()>        on_do_search;
    std::function<void()>        on_request_pinned;
    std::function<void()>        on_create_text_channel;
    std::function<void(int)>     on_delete_text_channel;
    std::function<void()>        on_attach_file;
    std::function<void(const std::string&)> on_open_url;

private:
    Rml::DataModelHandle handle_;
};

} // namespace parties::client

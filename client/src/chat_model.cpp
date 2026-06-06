#include <client/chat_model.h>

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace parties::client {

void ChatModel::build(rml::Builder& b) {
    // Register struct types — arrays before structs that contain them.
    b.register_struct<TextSegment>([](auto& s) {
        s.member("text",   &TextSegment::text)
         .member("is_url", &TextSegment::is_url);
    });
    b.register_array<Rml::Vector<TextSegment>>();

    b.register_struct<ChatAttachment>([](auto& s) {
        s.member("id",        &ChatAttachment::id)
         .member("file_name", &ChatAttachment::file_name)
         .member("size_str",  &ChatAttachment::size_str)
         .member("file_ext",  &ChatAttachment::file_ext)
         .member("uploaded",  &ChatAttachment::uploaded);
    });
    b.register_array<Rml::Vector<ChatAttachment>>();

    b.register_struct<ChatMessage>([](auto& s) {
        s.member("id",            &ChatMessage::id)
         .member("sender_id",     &ChatMessage::sender_id)
         .member("sender_name",   &ChatMessage::sender_name)
         .member("initials",      &ChatMessage::initials)
         .member("is_own",        &ChatMessage::is_own)
         .member("text",          &ChatMessage::text)
         .member("has_url",       &ChatMessage::has_url)
         .member("segments",      &ChatMessage::segments)
         .member("timestamp_str", &ChatMessage::timestamp_str)
         .member("date_label",    &ChatMessage::date_label)
         .member("pinned",        &ChatMessage::pinned)
         .member("color_index",   &ChatMessage::color_index)
         .member("attachments",   &ChatMessage::attachments);
    });
    b.register_array<Rml::Vector<ChatMessage>>();

    b.register_struct<TextChannel>([](auto& s) {
        s.member("id",         &TextChannel::id)
         .member("name",       &TextChannel::name)
         .member("has_unread", &TextChannel::has_unread);
    });
    b.register_array<Rml::Vector<TextChannel>>();

    b.register_struct<PendingFile>([](auto& s) {
        s.member("name",     &PendingFile::name)
         .member("size_str", &PendingFile::size_str)
         .member("path",     &PendingFile::path);
    });
    b.register_array<Rml::Vector<PendingFile>>();

    // Bind state.
    b.bind("text_channels",          text_channels)
     .bind("active_channel",         active_channel)
     .bind("active_channel_name",    active_channel_name)
     .bind("messages",               messages)
     .bind("loading_history",        loading_history)
     .bind("has_more_history",       has_more_history)
     .bind("compose_text",           compose_text)
     .bind("show_search",            show_search)
     .bind("search_query",           search_query)
     .bind("search_results",         search_results)
     .bind("show_pinned",            show_pinned)
     .bind("pinned_messages",        pinned_messages)
     .bind("pending_files",          pending_files)
     .bind("can_manage_channels",    can_manage_channels)
     .bind("show_create_text_channel", show_create_text_channel)
     .bind("new_text_channel_name",  new_text_channel_name);

    // Event callbacks.
    b.on("send_message", [this] {
        if (on_send_message) on_send_message();
    });

    b.on_args<int>("select_text_channel", [this](int id) {
        if (on_select_channel) on_select_channel(id);
    });

    b.on("load_more_history", [this] {
        if (on_load_more_history) on_load_more_history();
    });

    b.on_args<int64_t>("pin_message", [this](int64_t id) {
        if (on_pin_message) on_pin_message(id);
    });

    b.on_args<int64_t>("unpin_message", [this](int64_t id) {
        if (on_unpin_message) on_unpin_message(id);
    });

    b.on_args<int64_t>("delete_message", [this](int64_t id) {
        if (on_delete_message) on_delete_message(id);
    });

    b.on_args<int64_t>("download_file", [this](int64_t id) {
        if (on_download_file) on_download_file(id);
    });

    b.on("toggle_search", [this] {
        show_search = !show_search.get();
        if (!show_search.get()) {
            search_query = "";
            search_results.silent().clear();
            search_results.notify();
        }
    });

    b.on("do_search", [this] {
        if (on_do_search) on_do_search();
    });

    b.on("toggle_pinned", [this] {
        show_pinned = !show_pinned.get();
        if (show_pinned.get() && on_request_pinned)
            on_request_pinned();
    });

    b.on_args<Rml::String>("open_url", [this](Rml::String url) {
        // Only open if it looks like a URL
        if (on_open_url && (url.find("http://") == 0 || url.find("https://") == 0))
            on_open_url(std::string(url));
    });

    b.on("attach_file", [this] {
        if (on_attach_file) on_attach_file();
    });

    b.on_args<Rml::String>("remove_pending_file", [this](Rml::String path) {
        auto& files = pending_files.silent();
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (it->path == path) {
                files.erase(it);
                pending_files.notify();
                break;
            }
        }
    });

    b.on("show_create_text_channel_form", [this] {
        show_create_text_channel = true;
        new_text_channel_name = "";
    });

    b.on("cancel_create_text_channel", [this] {
        show_create_text_channel = false;
    });

    b.on("create_text_channel", [this] {
        if (on_create_text_channel) on_create_text_channel();
    });

    b.on_event("compose_keydown", [this](Rml::Event& event, const Rml::VariantList&) {
        if (event.GetParameter("key_identifier", 0) == Rml::Input::KI_RETURN) {
            if (on_send_message) on_send_message();
            // RmlUi data-value binding may not sync cleared model back to input,
            // so explicitly clear the input element's value attribute
            if (auto* el = event.GetCurrentElement())
                el->SetAttribute("value", Rml::String(""));
        }
    });

    b.on_event("search_keydown", [this](Rml::Event& event, const Rml::VariantList&) {
        if (event.GetParameter("key_identifier", 0) == Rml::Input::KI_RETURN) {
            if (on_do_search) on_do_search();
        }
    });

    b.on_event("message_mousedown", [this](Rml::Event& event, const Rml::VariantList& args) {
        // Right-click (button 1) opens context menu for message actions
        if (event.GetParameter("button", 0) == 1 && !args.empty()) {
            if (on_message_context_menu)
                on_message_context_menu(args[0].Get<int64_t>());
        }
    });
}

} // namespace parties::client

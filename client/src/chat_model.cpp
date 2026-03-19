#include <client/chat_model.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace parties::client {

ChatModel::ChatModel() = default;
ChatModel::~ChatModel() = default;

bool ChatModel::init(Rml::Context* context) {
    Rml::DataModelConstructor ctor = context->CreateDataModel("chat");
    if (!ctor)
        return false;

    // Register struct types — arrays before structs that contain them

    if (auto s = ctor.RegisterStruct<TextSegment>()) {
        s.RegisterMember("text",   &TextSegment::text);
        s.RegisterMember("is_url", &TextSegment::is_url);
    }
    ctor.RegisterArray<Rml::Vector<TextSegment>>();

    if (auto s = ctor.RegisterStruct<ChatAttachment>()) {
        s.RegisterMember("id",        &ChatAttachment::id);
        s.RegisterMember("file_name", &ChatAttachment::file_name);
        s.RegisterMember("size_str",  &ChatAttachment::size_str);
        s.RegisterMember("uploaded",  &ChatAttachment::uploaded);
    }
    ctor.RegisterArray<Rml::Vector<ChatAttachment>>();

    if (auto s = ctor.RegisterStruct<ChatMessage>()) {
        s.RegisterMember("id",            &ChatMessage::id);
        s.RegisterMember("sender_id",     &ChatMessage::sender_id);
        s.RegisterMember("sender_name",   &ChatMessage::sender_name);
        s.RegisterMember("text",          &ChatMessage::text);
        s.RegisterMember("segments",      &ChatMessage::segments);
        s.RegisterMember("timestamp_str", &ChatMessage::timestamp_str);
        s.RegisterMember("pinned",        &ChatMessage::pinned);
        s.RegisterMember("color_index",   &ChatMessage::color_index);
        s.RegisterMember("attachments",   &ChatMessage::attachments);
    }
    ctor.RegisterArray<Rml::Vector<ChatMessage>>();

    if (auto s = ctor.RegisterStruct<TextChannel>()) {
        s.RegisterMember("id",         &TextChannel::id);
        s.RegisterMember("name",       &TextChannel::name);
        s.RegisterMember("has_unread", &TextChannel::has_unread);
    }
    ctor.RegisterArray<Rml::Vector<TextChannel>>();

    if (auto s = ctor.RegisterStruct<PendingFile>()) {
        s.RegisterMember("name",     &PendingFile::name);
        s.RegisterMember("size_str", &PendingFile::size_str);
        s.RegisterMember("path",     &PendingFile::path);
    }
    ctor.RegisterArray<Rml::Vector<PendingFile>>();

    // Bind state
    ctor.Bind("text_channels",      &text_channels);
    ctor.Bind("active_channel",     &active_channel);
    ctor.Bind("active_channel_name",&active_channel_name);
    ctor.Bind("messages",           &messages);
    ctor.Bind("loading_history",    &loading_history);
    ctor.Bind("has_more_history",   &has_more_history);
    ctor.Bind("compose_text",       &compose_text);
    ctor.Bind("show_search",        &show_search);
    ctor.Bind("search_query",       &search_query);
    ctor.Bind("search_results",     &search_results);
    ctor.Bind("show_pinned",        &show_pinned);
    ctor.Bind("pinned_messages",    &pinned_messages);
    ctor.Bind("pending_files",             &pending_files);
    ctor.Bind("show_create_text_channel", &show_create_text_channel);
    ctor.Bind("new_text_channel_name",    &new_text_channel_name);

    // Event callbacks
    ctor.BindEventCallback("send_message",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_send_message) on_send_message();
        });

    ctor.BindEventCallback("select_text_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_select_channel)
                on_select_channel(args[0].Get<int>());
        });

    ctor.BindEventCallback("load_more_history",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_load_more_history) on_load_more_history();
        });

    ctor.BindEventCallback("pin_message",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_pin_message)
                on_pin_message(args[0].Get<int64_t>());
        });

    ctor.BindEventCallback("unpin_message",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_unpin_message)
                on_unpin_message(args[0].Get<int64_t>());
        });

    ctor.BindEventCallback("delete_message",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_delete_message)
                on_delete_message(args[0].Get<int64_t>());
        });

    ctor.BindEventCallback("download_file",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_download_file)
                on_download_file(args[0].Get<int64_t>());
        });

    ctor.BindEventCallback("toggle_search",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_search = !show_search;
            dirty("show_search");
            if (!show_search) {
                search_query = "";
                search_results.clear();
                dirty("search_query");
                dirty("search_results");
            }
        });

    ctor.BindEventCallback("do_search",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_do_search) on_do_search();
        });

    ctor.BindEventCallback("toggle_pinned",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_pinned = !show_pinned;
            dirty("show_pinned");
            if (show_pinned && on_request_pinned)
                on_request_pinned();
        });

    ctor.BindEventCallback("open_url",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty() && on_open_url) {
                Rml::String url = args[0].Get<Rml::String>();
                // Only open if it looks like a URL
                if (url.find("http://") == 0 || url.find("https://") == 0)
                    on_open_url(std::string(url));
            }
        });

    ctor.BindEventCallback("attach_file",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_attach_file) on_attach_file();
        });

    ctor.BindEventCallback("remove_pending_file",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) {
                Rml::String path = args[0].Get<Rml::String>();
                for (auto it = pending_files.begin(); it != pending_files.end(); ++it) {
                    if (it->path == path) {
                        pending_files.erase(it);
                        dirty("pending_files");
                        break;
                    }
                }
            }
        });

    ctor.BindEventCallback("show_create_text_channel_form",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_create_text_channel = true;
            new_text_channel_name = "";
            dirty("show_create_text_channel");
            dirty("new_text_channel_name");
        });

    ctor.BindEventCallback("cancel_create_text_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            show_create_text_channel = false;
            dirty("show_create_text_channel");
        });

    ctor.BindEventCallback("create_text_channel",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (on_create_text_channel) on_create_text_channel();
        });

    ctor.BindEventCallback("compose_keydown",
        [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
            if (event.GetParameter("key_identifier", 0) == Rml::Input::KI_RETURN) {
                if (on_send_message) on_send_message();
            }
        });

    ctor.BindEventCallback("search_keydown",
        [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
            if (event.GetParameter("key_identifier", 0) == Rml::Input::KI_RETURN) {
                if (on_do_search) on_do_search();
            }
        });

    handle_ = ctor.GetModelHandle();
    return true;
}

void ChatModel::dirty(const Rml::String& variable) {
    handle_.DirtyVariable(variable);
}

} // namespace parties::client

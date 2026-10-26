#include <client/app.h>
#include <client/app_core.h>
#include <d3dcompiler.h>
#include <client/screen_capture.h>
#include <client/video_encoder.h>
#include <client/video_decoder.h>
#include <client/video_decode_backlog.h>
#include <client/video_element.h>
#include <client/custom_elements.h>
#include <client/pip_geometry.h>
#include "RmlUi_RenderInterface_Extended.h"
#include "pip_window_host.h"
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/profiler.h>
#include <parties/log.h>
#include <parties/thread_scheduling.h>

#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <roapi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace parties::client {

static std::string vk_to_name(int vk) {
    switch (vk) {
    case VK_LBUTTON:  return "Mouse1";
    case VK_RBUTTON:  return "Mouse2";
    case VK_MBUTTON:  return "Mouse3";
    case VK_XBUTTON1: return "Mouse4";
    case VK_XBUTTON2: return "Mouse5";
    case VK_SPACE:    return "Space";
    case VK_RETURN:   return "Enter";
    case VK_TAB:      return "Tab";
    case VK_BACK:     return "Backspace";
    case VK_SHIFT:    return "Shift";
    case VK_CONTROL:  return "Ctrl";
    case VK_MENU:     return "Alt";
    case VK_CAPITAL:  return "CapsLock";
    case VK_LSHIFT:   return "LShift";
    case VK_RSHIFT:   return "RShift";
    case VK_LCONTROL: return "LCtrl";
    case VK_RCONTROL: return "RCtrl";
    case VK_LMENU:    return "LAlt";
    case VK_RMENU:    return "RAlt";
    default: {
        UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
        if (ch > 0 && ch < 128)
            return std::string(1, static_cast<char>(std::toupper(ch)));
        return "Key " + std::to_string(vk);
    }
    }
}

static bool is_modifier_vk(int vk) {
    return vk == VK_SHIFT   || vk == VK_CONTROL  || vk == VK_MENU    ||
           vk == VK_LSHIFT  || vk == VK_RSHIFT   ||
           vk == VK_LCONTROL|| vk == VK_RCONTROL ||
           vk == VK_LMENU   || vk == VK_RMENU;
}

static HotkeyModifiers current_modifiers() {
    HotkeyModifiers modifiers;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers = modifiers.with(HotkeyModifier::Ctrl);
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) modifiers = modifiers.with(HotkeyModifier::Shift);
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) modifiers = modifiers.with(HotkeyModifier::Alt);
    return modifiers;
}

static bool hotkey_held(const HotkeyBinding& binding) {
    if (!(GetAsyncKeyState(binding.primary_key) & 0x8000)) return false;
    if (binding.second_key && !(GetAsyncKeyState(*binding.second_key) & 0x8000)) return false;
    return current_modifiers() == binding.modifiers;
}

// Put UTF-8 text on the clipboard as CF_UNICODETEXT (CF_TEXT would mangle any
// non-ASCII — emoji, accents — which chat messages routinely contain).
static void set_clipboard_text(HWND hwnd, const std::string& utf8) {
    if (utf8.empty() || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                   static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen > 0) {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wlen) + 1) * sizeof(wchar_t));
        if (hMem) {
            auto* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                static_cast<int>(utf8.size()), dst, wlen);
            dst[wlen] = L'\0';
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

static SelectableTextElement* find_selectable(Rml::Element* el) {
    while (el) {
        if (el->GetTagName() == "selectable_text")
            return rmlui_dynamic_cast<SelectableTextElement*>(el);
        el = el->GetParentNode();
    }
    return nullptr;
}

bool App::handle_chat_input(unsigned int msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = ui_.context();
    if (!ctx) return false;
    auto& sel = ChatSelection::instance();

    switch (msg) {
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        Rml::Vector2f fp(static_cast<float>(pt.x), static_cast<float>(pt.y));
        if (auto* se = find_selectable(ctx->GetElementAtPoint(fp))) {
            ChatSelection::Point p;
            se->HitTest(fp, p);
            sel.begin(p);
            chat_drag_start_ = pt;
            chat_drag_moved_ = false;
        } else {
            sel.clear();
        }
        return false;
    }
    case WM_MOUSEMOVE: {
        if (!(wParam & MK_LBUTTON) || !sel.dragging()) return false;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (std::abs(pt.x - chat_drag_start_.x) > 3 || std::abs(pt.y - chat_drag_start_.y) > 3)
            chat_drag_moved_ = true;
        Rml::Vector2f fp(static_cast<float>(pt.x), static_cast<float>(pt.y));
        if (auto* se = find_selectable(ctx->GetElementAtPoint(fp))) {
            ChatSelection::Point p;
            se->HitTest(fp, p);
            sel.extend(p);
        }
        return false;
    }
    case WM_LBUTTONUP: {
        if (sel.dragging()) {
            sel.end_drag();
            if (!chat_drag_moved_) sel.clear();
        }
        return false;
    }
    case WM_KEYDOWN: {
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) && sel.has_selection()) {
            Rml::Element* focus = ctx->GetFocusElement();
            if (focus) {
                const Rml::String& tag = focus->GetTagName();
                if (tag == "input" || tag == "textarea") return false;
            }
            std::string text = sel.selected_text();
            if (!text.empty()) set_clipboard_text(hwnd_, text);
            return true;
        }
        return false;
    }
    }
    return false;
}

App::App() = default;
App::~App() {
    shutdown();
    parties::rml::dirty_hook = {};
}

bool App::handle_tray_message(UINT message, WPARAM w_param, LPARAM l_param) {
    return tray_.handle_message(message, w_param, l_param);
}

void App::on_window_visibility_changed(bool visible) {
    ui_.on_visibility_change(visible);
    frame_requests_.request();
    if (!visible) {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        context_windows_.close();
    }
}

bool App::init(HWND hwnd) {
    hwnd_ = hwnd;
    parties::rml::dirty_hook = {
        [](void* requests) { static_cast<FrameRequests*>(requests)->request(); }, &frame_requests_};

    PlatformBridge bridge;

    bridge.copy_to_clipboard = [this](const std::string& text) {
        set_clipboard_text(hwnd_, text);
    };

    bridge.play_sound = [this](SoundPlayer::Effect e) {
        sound_player_.play(e);
    };
    bridge.set_notification_volume = [this](float v) {
        sound_player_.set_volume(v);
    };

    bridge.show_user_menu = [this](const UserContextWindowRequest& request) {
        ContextWindowManager::UserRequest popup;
        popup.user_id = request.user_id;
        popup.name = request.name;
        popup.channel_name = request.channel_name;
        popup.role = request.role;
        popup.can_manage_roles = request.can_manage_roles;
        popup.can_kick = request.can_kick;
        popup.volume = request.volume;
        popup.music_volume = request.music_volume;
        popup.compression = request.compression;
        popup.compression_target = request.compression_target;

        ContextWindowManager::UserCallbacks callbacks;
        callbacks.set_volume = [this, user_id = request.user_id](float volume) {
            if (core_.model_.on_user_volume_changed)
                core_.model_.on_user_volume_changed(user_id, volume);
        };
        callbacks.set_music_volume = [this, user_id = request.user_id](float volume) {
            if (core_.model_.on_user_music_volume_changed)
                core_.model_.on_user_music_volume_changed(user_id, volume);
        };
        callbacks.set_compression = [this, user_id = request.user_id](bool enabled, float target) {
            if (core_.model_.on_user_compress_changed)
                core_.model_.on_user_compress_changed(user_id, enabled, target);
        };
        callbacks.set_role = [this, user_id = request.user_id](Role role) {
            if (core_.model_.on_set_user_role)
                core_.model_.on_set_user_role(user_id, role);
        };
        callbacks.kick = [this, user_id = request.user_id] {
            if (core_.model_.on_kick_user)
                core_.model_.on_kick_user(user_id);
        };
        context_windows_.show_user(popup, std::move(callbacks));
    };

    bridge.show_channel_menu = [this](int channel_id, std::string name) {
        constexpr int ID_RENAME = 1;
        constexpr int ID_DELETE = 2;
        ContextWindowManager::ActionRequest request;
        request.title = name;
        request.subtitle = "Channel actions";
        request.room_icon = true;
        request.actions = {
            {ID_RENAME, "Rename channel", "Change the channel name", false, false},
            {ID_DELETE, "Delete channel", "Remove it for everyone", true, false},
        };
        context_windows_.show_actions(request, [this, channel_id, name = std::move(name)](int command) {
            if (command == ID_RENAME) {
                core_.model_.rename_channel_id = channel_id;
                core_.model_.rename_channel_name = name;
                core_.model_.new_rename_channel_name = name;
                core_.model_.show_rename_channel = true;
            } else if (command == ID_DELETE) {
                BinaryWriter writer;
                writer.write_u32(static_cast<uint32_t>(channel_id));
                core_.net_.send_message(protocol::ControlMessageType::ADMIN_DELETE_CHANNEL,
                                        writer.data().data(), writer.data().size());
            }
        });
    };
    bridge.show_server_menu = [this](int id) {
        constexpr int ID_NICKNAME = 1;
        constexpr int ID_DELETE   = 2;
        ContextWindowManager::ActionRequest request;
        request.title = "Saved party";
        request.subtitle = "Connection actions";
        request.icon_text = "P";
        request.actions = {
            {ID_NICKNAME, "Change server nickname", "Override your name for this party", false, false},
            {ID_DELETE, "Remove saved party", "Delete this connection", true, false},
        };
        context_windows_.show_actions(request, [this, id](int command) {
            if (command == ID_NICKNAME) {
                if (core_.server_model_.on_edit_server_nickname)
                    core_.server_model_.on_edit_server_nickname(id);
            } else if (command == ID_DELETE) {
                if (core_.server_model_.on_delete_server)
                    core_.server_model_.on_delete_server(id);
            }
        });
    };

    bridge.show_message_menu = [this](int64_t msg_id) {
        constexpr int ID_COPY   = 1;
        constexpr int ID_PIN    = 2;
        constexpr int ID_DELETE = 3;

        std::string text;
        for (const auto& m : core_.chat_model_.messages.get()) {
            if (m.id == msg_id) { text = std::string(m.text); break; }
        }

        ContextWindowManager::ActionRequest request;
        request.title = "Message";
        request.subtitle = "Conversation actions";
        request.icon_text = "M";
        if (!text.empty()) request.actions.push_back({ID_COPY, "Copy text", "Copy to clipboard", false, false});
        request.actions.push_back({ID_PIN, "Pin message", "Keep it visible", false, false});
        request.actions.push_back({ID_DELETE, "Delete message", "Remove it permanently", true, false});
        context_windows_.show_actions(request, [this, msg_id, text = std::move(text)](int command) {
            if (command == ID_COPY) {
                set_clipboard_text(hwnd_, text);
            } else if (command == ID_PIN) {
                if (core_.chat_model_.on_pin_message) core_.chat_model_.on_pin_message(msg_id);
            } else if (command == ID_DELETE) {
                if (core_.chat_model_.on_delete_message) core_.chat_model_.on_delete_message(msg_id);
            }
        });
    };

    bridge.share = PickerShareFlow{
        .open_picker = [this]() { show_share_picker(false); },
        .open_audio_picker = [this]() { show_share_picker(true); },
        .cancel_picker = [this]() {
            if (core_.model_.router.is(DocumentRoute::SharePicker))
                core_.model_.router.back();
            cancel_share_thumbnails();
            capture_targets_.clear();
            if (core_.model_.share_picker_mode == SharePickerMode::Screen && capture_) {
                capture_->shutdown();
                capture_.reset();
            }
        },
    };

    bridge.stop_screen_share = [this]() { stop_screen_share(); };
    bridge.stop_audio_share = [this]() { stop_application_audio_share(); };

    bridge.request_keyframe = [this]() {
        if (encoder_) encoder_->force_keyframe();
        if (sharing_screen_ && encode_running_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            if (encode_ready_slot_ < 0 && encode_active_slot_ < 0) {
                int last = encode_write_slot_;
                for (int i = 0; i < ENCODE_SLOTS; i++) {
                    if (i != encode_write_slot_) { last = i; break; }
                }
                encode_ready_slot_ = last;
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                encode_ready_ts_ = (now.QuadPart - capture_start_qpc_) * 10'000'000LL / qpc_frequency_;
            }
            encode_cv_.notify_one();
        }
    };

    bridge.video = MultiStreamVideo{
        .start_stream = [this](UserId id) { start_video_stream(id); },
        .stop_stream = [this](UserId id) { stop_video_stream(id); },
        .wants_pli_retry = [this](UserId id) {
            std::lock_guard<std::mutex> lock(streams_mutex_);
            auto it = video_streams_.find(id);
            return it != video_streams_.end() && it->second->inbox.wants_pli_retry();
        },
    };

    bridge.clear_video_element = []() {};

    bridge.set_keep_awake = [this](bool keep_awake) {
        call_power_request_.set(keep_awake);
    };

    pip_host_ = std::make_unique<PipWindowHost>();
    bridge.pip = PictureInPicture{
        .show = [this](UserId, const std::string& title) {
            if (!pip_host_->show(title)) pip_open_failed_ = true;
        },
        .hide = [this](UserId, PipCloseReason) { pip_host_->hide(); },
    };
    bridge.show_main_window = [this] {
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
    };

    if (!ui_.init(hwnd)) return false;
    decode_d3d12_device_ = static_cast<ID3D12Device*>(ui_.renderer()->GetD3D12Device());
    {
        PipWindowHost::Actions actions;
        actions.return_to_main = [this] { core_.return_from_pip(); };
        actions.set_volume = [this](float volume) { core_.set_stream_volume(volume); };
        actions.close = [this] { core_.close_pip(PipCloseReason::UserClosed); };
        actions.load_geometry = [this]() -> std::optional<PipRect> {
            const auto saved = core_.settings_.get_pref("window.pip_rect");
            return saved ? pip_rect_from_string(*saved) : std::nullopt;
        };
        actions.save_geometry = [this](const PipRect& rect) {
            if (rect.width > 0.0 && rect.height > 0.0)
                core_.settings_.set_pref("window.pip_rect", pip_rect_to_string(rect));
        };
        pip_host_->init(&ui_mutex_, decode_d3d12_device_, std::move(actions));
    }
    if (!context_windows_.init(hwnd, &ui_mutex_)) {
        LOG_ERROR("Context window manager init failed");
        return false;
    }

    if (!core_.init("parties_client.db", std::move(bridge), ui_.context()))
        return false;

    ChatSelection::instance().get_messages = [this]() {
        std::vector<std::pair<int64_t, std::string>> out;
        for (const auto& m : core_.chat_model_.messages.get())
            out.emplace_back(static_cast<int64_t>(m.id), std::string(m.text));
        return out;
    };

    core_.on_video_frame_received = [this](UserId sender, ParsedVideoFrame&& frame) {
        enqueue_decode_work(sender, std::move(frame));
    };

    core_.model_.on_select_share_target = [this](int index) {
        if (core_.model_.router.is(DocumentRoute::SharePicker))
            core_.model_.router.back();
        cancel_share_thumbnails();
        if (core_.model_.share_picker_mode == SharePickerMode::ApplicationAudio)
            start_application_audio_share(index);
        else
            start_screen_share(index);
    };

    core_.model_.on_share_bitrate_changed = [this](float mbps) {
        core_.settings_.set_pref("video.share_bitrate", std::to_string(mbps));
        uint32_t bps = static_cast<uint32_t>(mbps * 1'000'000.0f);
        bps = (std::max)(bps, VIDEO_MIN_BITRATE);
        bps = (std::min)(bps, VIDEO_MAX_BITRATE);
        core_.set_video_ceiling_bitrate(bps);
    };

    char computer_name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD computer_name_len = MAX_COMPUTERNAME_LENGTH + 1;
    std::string username_hint = "User";
    if (GetComputerNameA(computer_name, &computer_name_len) && computer_name_len > 0)
        username_hint.assign(computer_name, computer_name_len);
    core_.load_or_generate_identity(username_hint);

    core_.load_saved_prefs();

    const bool tray_enabled = core_.settings_.get_pref("window.tray_mode").value_or("1") != "0";
    if (!tray_.init(hwnd, tray_enabled, [this](bool enabled) {
            std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
            if (core_.settings_.set_pref("window.tray_mode", enabled ? "1" : "0"))
                return true;
            LOG_ERROR("Failed to save tray mode preference");
            return false;
        }, [this] { tick_message_thread(); })) {
        LOG_WARN("Tray icon unavailable; closing the window will exit until it is available");
    }

    for (HotkeyAction action : kHotkeyActions)
        assign_hotkey(action, load_hotkey(core_.settings_, action));

    sound_player_.init();

    register_custom_elements(element_registry_);

    doc_ = ui_.load_document("ui/lobby.rml");
    if (doc_) {
        doc_->SetClass("platform-windows", true);
        doc_->SetClass("platform-desktop", true);
        ui_.show_document(doc_);
    }

    core_.refresh_server_list();

    if (core_.has_identity_) {
        core_.server_model_.has_identity = true;
        core_.server_model_.fingerprint  = Rml::String(
            parties::public_key_fingerprint(core_.public_key_));
    }

    render_running_.store(true, std::memory_order_release);
    render_thread_ = std::thread([this] { render_loop(); });

    return true;
}

void App::shutdown() {
    tray_.shutdown();
    cancel_share_thumbnails();
    if (share_thumbnail_thread_.joinable()) {
        share_thumbnail_thread_.request_stop();
        share_thumbnail_cv_.notify_all();
        share_thumbnail_thread_.join();
    }

    render_running_.store(false, std::memory_order_release);
    frame_requests_.request();
    if (render_thread_.joinable())
        render_thread_.join();

    context_windows_.release_documents();
    if (pip_host_) {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        pip_host_->release_documents();
    }
    // WGC activation is performed on a detached worker because Windows may
    // block indefinitely in its out-of-process GraphicsCapture RPC. Dropping
    // our references is safe: an in-flight job owns its ScreenCapture until
    // the system call returns, without retaining App.
    auto abandoned_capture_start = std::move(screen_share_start_job_);
    if (abandoned_capture_start &&
        !abandoned_capture_start->complete.load(std::memory_order_acquire)) {
        capture_.reset();
    }
    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    stop_application_audio_share();
    // Stop producing frames first, but keep the capture D3D11 device alive
    // until the consumer thread and every encoder registration are gone.
    // NVENC/AMF keep references to resources created by this device.
    if (capture_) capture_->stop_frame_delivery();
    if (encode_thread_.joinable()) {
        encode_running_.store(false, std::memory_order_release);
        encode_cv_.notify_one();
        encode_thread_.join();
    }
    if (encoder_ && encode_registered_) encoder_->unregister_inputs();
    for (auto& t : encode_textures_) t.Reset();
    for (auto& rtv : encode_rtvs_) rtv.Reset();
    encode_registered_ = false;
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    if (capture_) ScreenCapture::shutdown_async(std::move(capture_));
    capture_targets_.clear();
    stop_all_video_streams();
    core_.shutdown();
    const auto rml_gone = ui_.shutdown();
    context_windows_.destroy_window(rml_gone);
    if (pip_host_) pip_host_->destroy_window(rml_gone);
}

void App::assign_hotkey(HotkeyAction action, std::optional<HotkeyBinding> binding) {
    auto props = core_.model_.hotkey_props(action);
    props.assigned = binding.has_value();
    props.name = binding
        ? Rml::String(hotkey_label(*binding, [](uint16_t vk) { return vk_to_name(vk); }))
        : Rml::String();
    hotkeys_[hotkey_index(action)].binding = std::move(binding);
}

void App::poll_hotkeys() {
    const bool any_capturing = std::ranges::any_of(kHotkeyActions, [this](HotkeyAction action) {
        return core_.model_.hotkey_props(action).capturing.get();
    });
    if (any_capturing) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            for (HotkeyAction action : kHotkeyActions)
                core_.model_.hotkey_props(action).capturing = false;
            capture_peak_.reset();
        } else {
            int pressed[2] = {0, 0};
            int count = 0;
            for (int vk = 1; vk < 256 && count < 2; vk++) {
                if (is_modifier_vk(vk)) continue;
                if (GetAsyncKeyState(vk) & 0x8000) pressed[count++] = vk;
            }

            if (count > 0) {
                const int peak_count = !capture_peak_ ? 0 : (capture_peak_->second_key ? 2 : 1);
                if (count > peak_count) {
                    capture_peak_ = count >= 2
                        ? hotkey_from_keys(pressed[1], pressed[0], current_modifiers())
                        : hotkey_from_keys(pressed[0], 0, current_modifiers());
                }
            } else if (capture_peak_) {
                for (HotkeyAction action : kHotkeyActions) {
                    auto props = core_.model_.hotkey_props(action);
                    if (!props.capturing.get()) continue;
                    save_hotkey(core_.settings_, action, *capture_peak_);
                    assign_hotkey(action, capture_peak_);
                    props.capturing = false;
                }
                capture_peak_.reset();
            }
        }
    }

    const auto& ptt = hotkeys_[hotkey_index(HotkeyAction::PushToTalk)].binding;
    if (core_.model_.ptt_enabled && ptt && core_.current_channel_ != 0) {
        bool blocked = core_.model_.is_muted || core_.model_.is_deafened;
        bool held = hotkey_held(*ptt);
        auto now = std::chrono::steady_clock::now();
        if (held && !blocked) {
            ptt_held_ = true;
            if (core_.audio_.is_muted()) {
                core_.audio_.set_muted(false);
            }
        } else if (ptt_held_) {
            ptt_held_ = false;
            ptt_release_time_ = now;
        }
        if (!ptt_held_ && !core_.audio_.is_muted()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - ptt_release_time_).count();
            if (elapsed >= static_cast<int64_t>(core_.model_.ptt_delay)) {
                core_.audio_.set_muted(true);
            }
        }
    }

    auto poll_toggle = [this](HotkeyAction action, const std::function<void()>& toggle) {
        auto& slot = hotkeys_[hotkey_index(action)];
        if (!slot.binding || core_.current_channel_ == 0) return;
        const bool held = hotkey_held(*slot.binding);
        if (held && !slot.held && toggle) toggle();
        slot.held = held;
    };
    poll_toggle(HotkeyAction::ToggleMute, core_.model_.on_toggle_mute);
    poll_toggle(HotkeyAction::ToggleDeafen, core_.model_.on_toggle_deafen);
}

void App::render_loop() {
    TracySetThreadName("Render");
    if (parties::raise_thread_to_highest_normal_priority() == parties::ThreadPriorityChange::Failed)
        LOG_WARN("Failed to set the render thread to highest priority");
    while (render_running_.load(std::memory_order_acquire)) {
        if (resize_pending_.exchange(false, std::memory_order_acquire)) {
            int w = pending_w_.load(std::memory_order_relaxed);
            int h = pending_h_.load(std::memory_order_relaxed);
            std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
            ui_.on_resize(w, h);
        }
        if (dpi_pending_.exchange(false, std::memory_order_acquire)) {
            float scale = pending_dpi_.load(std::memory_order_relaxed);
            std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
            ui_.on_dpi_change(scale);
        }
        if (ui_.is_render_suspended()) {
            if (pip_host_ && pip_host_->visible()) {
                render_pip_only();
            } else {
                {
                    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
                    sync_video_router();
                }
                Sleep(16);
            }
            continue;
        }
        const auto now = RenderClock::now();
        const bool titlebar_changed = update_titlebar(now);
        const bool requested = frame_requests_.take();
        bool always_dirty = pip_host_ && pip_host_->visible();
        if (!always_dirty) {
            std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
            always_dirty = context_windows_.is_open();
        }
        const auto deadline = (std::min)(next_ui_update_, fps_last_update_ + std::chrono::seconds(1));
        const auto wait = idle_wait_ms(requested || always_dirty || titlebar_changed, now, deadline);
        // DWM flips an isolated present a varying number of refreshes late, so
        // while video plays the window presents every refresh, reusing the last
        // frame when nothing changed.
        if (wait && !has_video_streams()) {
            {
                std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
                sync_video_router();
            }
            frame_requests_.wait(*wait);
            continue;
        }
        render_frame(requested || always_dirty || now >= next_ui_update_, wait.has_value());
    }
}

bool App::has_video_streams() {
    std::lock_guard<std::mutex> slock(streams_mutex_);
    return !video_streams_.empty();
}

VideoFrameRouter::Surfaces App::video_surfaces() {
    VideoFrameRouter::Surfaces surfaces;
    surfaces.grid = doc_ ? doc_->GetElementById("stream-grid") : nullptr;
    surfaces.pip = pip_host_ ? pip_host_->video_surface() : nullptr;
    return surfaces;
}

void App::sync_video_router() {
    const auto cleared = video_router_.sync(core_.pip_.stream(), video_surfaces());
    if (cleared.grid || cleared.pip) frame_requests_.request();
    if (cleared.grid) ui_.renderer()->ReleaseRetiredResourcesBetweenFrames();
    if (cleared.pip && pip_host_ && pip_host_->renderer())
        pip_host_->renderer()->ReleaseRetiredResourcesBetweenFrames();
}

void App::deliver_video_frames(bool grid_visible) {
    if (!doc_) return;
    ZoneScopedN("App::deliver_video_frames");
    struct PendingUpload {
        uint32_t sharer;
        std::optional<NativeVideoFrame> native;
        std::vector<uint8_t> y, u, v;
        uint32_t w, h, ys, uvs;
        bool nv12;
    };
    const UserId pip_stream = video_router_.pip_stream();
    std::vector<PendingUpload> uploads;
    {
        std::lock_guard<std::mutex> slock(streams_mutex_);
        for (auto& [uid, sp] : video_streams_) {
            VideoStream* s = sp.get();
            if (!grid_visible && s->sharer_id != pip_stream) continue;
            if (!s->new_frame.load(std::memory_order_acquire)) continue;
            std::lock_guard<std::mutex> flock(s->frame_mutex);
            if (!s->new_frame.load(std::memory_order_relaxed)) continue;
            PendingUpload up;
            up.sharer = static_cast<uint32_t>(s->sharer_id);
            up.native = std::exchange(s->native, std::nullopt);
            up.y.swap(s->y); up.u.swap(s->u); up.v.swap(s->v);
            up.w = s->width; up.h = s->height;
            up.ys = s->y_stride; up.uvs = s->uv_stride;
            up.nv12 = s->nv12;
            s->new_frame.store(false, std::memory_order_relaxed);
            uploads.push_back(std::move(up));
        }
    }
    if (uploads.empty()) return;
    const auto surfaces = video_surfaces();
    for (auto& up : uploads) {
        if ((!up.native && up.y.empty()) || up.w == 0 || up.h == 0) continue;
        core_.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);
        VideoElement* ve = video_router_.target(up.sharer, surfaces);
        if (!ve) continue;
        if (up.native)
            ve->UpdateNativeVideoFrame(std::move(*up.native));
        else if (up.nv12)
            ve->UpdateNV12Frame(up.y, up.ys, up.u, up.uvs, up.w, up.h);
        else
            ve->UpdateYUVFrame(up.y, up.ys, up.u, up.v, up.uvs, up.w, up.h);
    }
}

void App::render_pip_only() {
    ZoneScopedN("App::render_pip_only");
    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        sync_video_router();
        deliver_video_frames(false);
        pip_host_->render(core_.model_.stream_volume.get());
    }
    if (FAILED(DwmFlush())) Sleep(16);
}

static Rml::Element* find_share_thumbnail(Rml::ElementDocument* document, int target_index) {
    if (!document) return nullptr;
    const Rml::String element_id = "share-thumbnail-" + Rml::ToString(target_index);
    return document->GetElementById(element_id);
}

bool App::update_titlebar(RenderClock::time_point now) {
    if (now - fps_last_update_ < std::chrono::seconds(1)) return false;
    const float elapsed = std::chrono::duration<float>(now - fps_last_update_).count();
    const int fps = static_cast<int>(fps_frame_count_ / elapsed);
    fps_frame_count_ = 0;
    fps_last_update_ = now;
    if (!doc_) return false;
    bool changed = false;
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    Rml::String fps_text(std::to_string(fps) + " fps");
    if (fps_text != titlebar_fps_last_) {
        if (auto* elem = doc_->GetElementById("titlebar-fps")) {
            elem->SetInnerRML(fps_text);
            titlebar_fps_last_ = std::move(fps_text);
            changed = true;
        }
    }
    Rml::String ping_text = core_.model_.is_connected
        ? Rml::String(std::to_string(core_.model_.ping_ms.get()) + " ms")
        : Rml::String();
    if (ping_text != titlebar_ping_last_) {
        if (auto* elem = doc_->GetElementById("titlebar-ping")) {
            elem->SetInnerRML(ping_text);
            titlebar_ping_last_ = std::move(ping_text);
            changed = true;
        }
    }
    return changed;
}

void App::render_frame(bool counts_toward_fps, bool may_represent) {
    ZoneScopedN("App::render_frame");

    const auto frame_start = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        sync_video_router();
    }

    if (!ui_.render_begin()) {
        frame_requests_.request();
        Sleep(16);
        return;
    }
    if (may_represent) {
        if (frame_requests_.take()) {
            counts_toward_fps = true;
        } else if (ui_.repeat_last_frame()) {
            return;
        }
    }
    const auto begin_complete = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);

        deliver_video_frames(true);

        if (counts_toward_fps) fps_frame_count_++;

        ui_.update();

        if (doc_ && core_.model_.router.is(DocumentRoute::SharePicker)) {
            std::vector<ShareThumbnail> ready;
            {
                std::lock_guard lock(share_thumbnail_mutex_);
                const size_t count = (std::min)(size_t{2}, pending_share_thumbnails_.size());
                ready.reserve(count);
                for (size_t index = 0; index < count; ++index)
                    ready.push_back(std::move(pending_share_thumbnails_[index]));
                pending_share_thumbnails_.erase(
                    pending_share_thumbnails_.begin(),
                    pending_share_thumbnails_.begin() + static_cast<std::ptrdiff_t>(count));
            }

            std::vector<ShareThumbnail> remaining;
            for (auto& thumbnail : ready) {
                auto* element = find_share_thumbnail(doc_, thumbnail.target_index);
                if (element && !thumbnail.rgba.empty()) {
                    static_cast<VideoElement*>(element)->UpdateFrame(
                        std::move(thumbnail.rgba), thumbnail.width, thumbnail.height);
                } else {
                    remaining.push_back(std::move(thumbnail));
                }
            }
            std::lock_guard lock(share_thumbnail_mutex_);
            for (auto& thumbnail : remaining)
                pending_share_thumbnails_.push_back(std::move(thumbnail));
            if (!pending_share_thumbnails_.empty()) frame_requests_.request();
        } else if (!core_.model_.router.is(DocumentRoute::SharePicker)) {
            cancel_share_thumbnails();
        }
        ui_.render_body();
        if (auto* context = ui_.context())
            next_ui_update_ = ui_update_deadline(RenderClock::now(), context->GetNextUpdateDelay());
    }
    const auto body_complete = std::chrono::steady_clock::now();
    ui_.render_end();
    const auto present_complete = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        context_windows_.render();
        if (pip_host_) pip_host_->render(core_.model_.stream_volume.get());
    }
    const auto frame_complete = std::chrono::steady_clock::now();

    const auto milliseconds = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    const double total_ms = milliseconds(frame_complete - frame_start);
    if (total_ms >= 50.0) {
        if (last_render_stall_log_.time_since_epoch().count() == 0 ||
            frame_complete - last_render_stall_log_ >= std::chrono::seconds(5)) {
            LOG_WARN("RENDER_STALL total_ms={:.2f} begin_wait_ms={:.2f} ui_record_ms={:.2f} "
                     "present_ms={:.2f} context_windows_ms={:.2f} "
                     "suppressed_since_previous={}",
                     total_ms, milliseconds(begin_complete - frame_start),
                     milliseconds(body_complete - begin_complete),
                     milliseconds(present_complete - body_complete),
                     milliseconds(frame_complete - present_complete),
                     suppressed_render_stalls_);
            last_render_stall_log_ = frame_complete;
            suppressed_render_stalls_ = 0;
        } else {
            ++suppressed_render_stalls_;
        }
    }
}

void App::tick_message_thread() {
    ZoneScopedN("App::tick_message_thread");

    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);

    poll_screen_share_start();
    // WGC can miss/delay Closed, particularly after asynchronous startup.
    // Native source liveness also catches application exit without a new frame.
    if (sharing_screen_ && capture_ && capture_->target_lost()) {
        LOG_WARN("Capture target lost, stopping screen share");
        stop_screen_share();
    }

    core_.tick();

    if (pip_open_failed_) {
        pip_open_failed_ = false;
        core_.close_pip(PipCloseReason::UserClosed);
    }

    poll_hotkeys();

    if (core_.model_.stream_fullscreen && (GetAsyncKeyState(VK_ESCAPE) & 1))
        core_.model_.stream_fullscreen = false;

    if (ui_.is_fullscreen() != core_.model_.stream_fullscreen)
        ui_.set_fullscreen(core_.model_.stream_fullscreen);
}

void App::defer_resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    pending_w_.store(width, std::memory_order_relaxed);
    pending_h_.store(height, std::memory_order_relaxed);
    resize_pending_.store(true, std::memory_order_release);
    frame_requests_.request();
}

void App::defer_dpi(float scale) {
    pending_dpi_.store(scale, std::memory_order_relaxed);
    dpi_pending_.store(true, std::memory_order_release);
    frame_requests_.request();
}

void App::show_share_picker(bool audio_only) {
    ZoneScopedN("App::show_share_picker");
    if ((!audio_only && sharing_screen_) ||
        (!audio_only && screen_share_start_job_) ||
        (audio_only && application_audio_sharing_.load(std::memory_order_acquire)) ||
        !core_.authenticated_ || core_.current_channel_ == 0) return;
    std::unique_ptr<ScreenCapture> audio_picker_capture;
    ScreenCapture* picker = nullptr;
    if (audio_only) {
        audio_picker_capture = std::make_unique<ScreenCapture>();
        picker = audio_picker_capture.get();
    } else {
        capture_ = std::make_shared<ScreenCapture>();
        picker = capture_.get();
    }

    if (!picker->init()) {
        LOG_ERROR("Screen capture init failed");
        if (!audio_only) capture_.reset();
        return;
    }
    capture_targets_.clear();
    cancel_share_thumbnails();
    auto& monitor_targets = core_.model_.share_monitor_targets.silent();
    auto& application_targets = core_.model_.share_application_targets.silent();
    monitor_targets.clear();
    application_targets.clear();
    core_.model_.selected_share_target = -1;
    core_.model_.share_picker_mode = audio_only ? SharePickerMode::ApplicationAudio : SharePickerMode::Screen;

    if (!audio_only) {
        for (auto& m : picker->enumerate_monitors()) {
            int idx = static_cast<int>(capture_targets_.size());
            ShareTarget st; st.name = Rml::String(m.name); st.index = idx; st.is_monitor = true;
            st.element_id = "share-thumbnail-" + Rml::ToString(idx);
            monitor_targets.push_back(std::move(st));
            capture_targets_.push_back(std::move(m));
        }
    }
    for (auto& w : picker->enumerate_windows()) {
        int idx = static_cast<int>(capture_targets_.size());
        ShareTarget st; st.name = Rml::String(w.name); st.index = idx; st.is_monitor = false;
        st.element_id = "share-thumbnail-" + Rml::ToString(idx);
        application_targets.push_back(std::move(st));
        capture_targets_.push_back(std::move(w));
    }

    core_.model_.share_monitor_targets.notify();
    core_.model_.share_application_targets.notify();
    core_.model_.router.open_share_picker();
    queue_share_thumbnails();
}

void App::queue_share_thumbnails() {
    ZoneScopedN("App::queue_share_thumbnails");
    if (!share_thumbnail_thread_.joinable()) {
        share_thumbnail_thread_ = std::jthread(
            [this](std::stop_token stop_token) { share_thumbnail_worker(stop_token); });
    }

    {
        std::lock_guard lock(share_thumbnail_mutex_);
        ++share_thumbnail_generation_;
        share_thumbnail_job_targets_ = capture_targets_;
        pending_share_thumbnails_.clear();
        share_thumbnail_job_pending_ = true;
    }
    share_thumbnail_cv_.notify_all();
}

void App::cancel_share_thumbnails() {
    std::lock_guard lock(share_thumbnail_mutex_);
    if (!share_thumbnail_job_active_ && !share_thumbnail_job_pending_ &&
        pending_share_thumbnails_.empty())
        return;

    ++share_thumbnail_generation_;
    share_thumbnail_job_targets_.clear();
    pending_share_thumbnails_.clear();
    share_thumbnail_job_pending_ = false;
    // An active WGC call cannot be cancelled. Keep the active flag set until
    // the worker has actually returned so capture startup never overlaps it.
    share_thumbnail_cv_.notify_all();
}

void App::share_thumbnail_worker(std::stop_token stop_token) {
    TracySetThreadName("Share thumbnails");
    while (!stop_token.stop_requested()) {
        std::vector<CaptureTarget> targets;
        uint64_t generation = 0;
        {
            std::unique_lock lock(share_thumbnail_mutex_);
            share_thumbnail_cv_.wait(lock, stop_token, [this] {
                return share_thumbnail_job_pending_;
            });
            if (stop_token.stop_requested())
                break;
            targets = share_thumbnail_job_targets_;
            generation = share_thumbnail_generation_;
            share_thumbnail_job_pending_ = false;
            share_thumbnail_job_active_ = true;
        }

        // GraphicsCapture activation is an out-of-process COM/RPC call. Four
        // concurrent preview sessions were enough to wedge the service on a
        // real machine, after which capture startup blocked the UI thread too.
        // One reusable session keeps the expensive work off the UI thread and
        // avoids saturating the system capture broker.
        ScreenCapture::ThumbnailSession thumbnail_session;
        for (int index = 0; index < static_cast<int>(targets.size()); ++index) {
            if (stop_token.stop_requested())
                break;
            {
                std::lock_guard lock(share_thumbnail_mutex_);
                if (generation != share_thumbnail_generation_)
                    break;
            }

            auto thumbnail = thumbnail_session.capture(targets[index], 320, 180);
            if (!thumbnail)
                continue;

            ShareThumbnail pending;
            pending.target_index = index;
            pending.rgba = std::move(thumbnail.rgba);
            pending.width = thumbnail.width;
            pending.height = thumbnail.height;

            std::lock_guard lock(share_thumbnail_mutex_);
            if (generation != share_thumbnail_generation_)
                break;
            pending_share_thumbnails_.push_back(std::move(pending));
            frame_requests_.request();
        }

        {
            std::lock_guard lock(share_thumbnail_mutex_);
            share_thumbnail_job_active_ = false;
        }
        share_thumbnail_cv_.notify_all();
    }
}

void App::start_application_audio_share(int target_index) {
    ZoneScopedN("App::start_application_audio_share");
    if (target_index < 0 || target_index >= static_cast<int>(capture_targets_.size()) ||
        capture_targets_[target_index].type != CaptureTarget::Type::Window ||
        !core_.authenticated_ || core_.current_channel_ == 0)
        return;

    HWND target_window = static_cast<HWND>(capture_targets_[target_index].handle);
    DWORD target_pid = 0;
    GetWindowThreadProcessId(target_window, &target_pid);
    const std::string target_name = capture_targets_[target_index].name;

    cancel_share_thumbnails();
    capture_targets_.clear();
    if (target_pid == 0) {
        LOG_ERROR("Cannot resolve process for application audio target");
        return;
    }

    stop_application_audio_share();
    auto capture = std::make_unique<StreamAudioCapture>();
    if (!capture->init(CaptureProcessTree{static_cast<uint32_t>(target_pid)},
                       StreamAudioCapture::OutputMode::MonoPcm)) {
        LOG_ERROR("Application audio capture init failed for PID {}", target_pid);
        return;
    }
    capture->on_pcm_frame = [this](const float* pcm, int frame_count) {
        if (!application_audio_sharing_.load(std::memory_order_acquire))
            return;
        core_.audio_.push_secondary_pcm(pcm, frame_count);
    };
    application_audio_sharing_.store(true, std::memory_order_release);
    if (!capture->start()) {
        application_audio_sharing_.store(false, std::memory_order_release);
        LOG_ERROR("Application audio capture start failed for PID {}", target_pid);
        return;
    }
    application_audio_capture_ = std::move(capture);
    core_.model_.is_audio_sharing = true;
    core_.model_.audio_share_target_name = target_name;
    LOG_INFO("Sharing application audio from '{}' (PID {}) over VOICE2", target_name, target_pid);
}

void App::stop_application_audio_share() {
    ZoneScopedN("App::stop_application_audio_share");
    application_audio_sharing_.store(false, std::memory_order_release);
    if (application_audio_capture_) {
        application_audio_capture_->stop();
        application_audio_capture_.reset();
    }
    core_.model_.is_audio_sharing = false;
    core_.model_.audio_share_target_name = "";
}

void App::init_scale_pipeline(ID3D11Device* device) {
    if (scale_pipeline_ready_) return;

    const char* vs_src = R"(
        void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD) {
            uv = float2((id << 1) & 2, id & 2);
            pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
        })";
    const char* ps_src = R"(
        SamplerState samp : register(s0);
        Texture2D tex : register(t0);
        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
            return tex.Sample(samp, uv);
        })";

    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob, ps_blob, err;
    if (FAILED(D3DCompile(vs_src, strlen(vs_src), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &err)) ||
        FAILED(D3DCompile(ps_src, strlen(ps_src), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &err))) {
        LOG_ERROR("Scale shader compile failed"); return;
    }
    device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &scale_vs_);
    device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &scale_ps_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &scale_sampler_);

    scale_pipeline_ready_ = (scale_vs_ && scale_ps_ && scale_sampler_);
}

void App::start_screen_share(int target_index) {
    ZoneScopedN("App::start_screen_share");
    if (sharing_screen_ || screen_share_start_job_ ||
        !core_.authenticated_ || core_.current_channel_ == 0) return;

    if (target_index < 0 || target_index >= static_cast<int>(capture_targets_.size())) {
        capture_targets_.clear();
        if (capture_) ScreenCapture::shutdown_async(std::move(capture_));
        return;
    }
    if (!capture_) return;

    const auto target = capture_targets_[target_index];

    StreamAudioSource audio_source = CaptureAllExceptOwnProcess{};
    if (target.type == CaptureTarget::Type::Window && target.handle) {
        DWORD pid = 0;
        GetWindowThreadProcessId(static_cast<HWND>(target.handle), &pid);
        if (pid != 0) audio_source = CaptureProcessTree{static_cast<uint32_t>(pid)};
    }

    encode_fps_ = frames_per_second(core_.model_.share_fps.get());

    auto job = std::make_shared<ScreenShareStartJob>();
    job->capture = capture_;
    job->target = target;
    job->fps = encode_fps_;
    job->audio_source = audio_source;
    job->channel_id = core_.current_channel_;
    screen_share_start_job_ = std::move(job);
    capture_targets_.clear();

    LOG_INFO("Queued asynchronous screen capture startup for '{}' at {} FPS",
             target.name, encode_fps_);
}

void App::poll_screen_share_start() {
    auto job = screen_share_start_job_;
    if (!job)
        return;

    if (!job->launched) {
        bool thumbnail_capture_idle = false;
        {
            std::lock_guard lock(share_thumbnail_mutex_);
            thumbnail_capture_idle = !share_thumbnail_job_active_ &&
                                     !share_thumbnail_job_pending_;
        }
        if (!thumbnail_capture_idle)
            return;

        job->launched = true;
        std::thread([job] {
            TracySetThreadName("Capture startup");
            const HRESULT apartment_result = RoInitialize(RO_INIT_MULTITHREADED);
            if (FAILED(apartment_result) && apartment_result != RPC_E_CHANGED_MODE) {
                LOG_ERROR("Capture startup COM initialization failed: {:#010x}",
                          static_cast<unsigned>(apartment_result));
                job->complete.store(true, std::memory_order_release);
                return;
            }

            job->succeeded = job->capture->start(job->target, job->fps);
            if (SUCCEEDED(apartment_result))
                RoUninitialize();
            job->complete.store(true, std::memory_order_release);
        }).detach();
        return;
    }

    if (!job->complete.load(std::memory_order_acquire))
        return;

    screen_share_start_job_.reset();
    if (capture_ != job->capture || !core_.authenticated_ ||
        core_.current_channel_ == 0 || core_.current_channel_ != job->channel_id) {
        LOG_WARN("Discarding completed screen capture startup after application state changed");
        if (job->succeeded)
            ScreenCapture::shutdown_async(job->capture);
        if (capture_ == job->capture)
            capture_.reset();
        return;
    }

    if (!job->succeeded || capture_->target_lost()) {
        LOG_ERROR("Failed to start capture for '{}'", job->target.name);
        ScreenCapture::shutdown_async(std::move(capture_));
        return;
    }

    finish_screen_share_start(job->audio_source);
}

void App::finish_screen_share_start(StreamAudioSource audio_source) {
    ZoneScopedN("App::finish_screen_share_start");
    if (!capture_)
        return;

    core_.settings_.set_pref("video.share_bitrate", std::to_string(core_.model_.share_bitrate.get()));
    save_share_encoding(core_.settings_, core_.model_.share_encoding());

    {
        uint32_t bitrate_bps = static_cast<uint32_t>(core_.model_.share_bitrate * 1'000'000.0f);
        bitrate_bps = (std::max)(bitrate_bps, VIDEO_MIN_BITRATE);
        bitrate_bps = (std::min)(bitrate_bps, VIDEO_MAX_BITRATE);
        core_.begin_video_share(bitrate_bps);
    }

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    qpc_frequency_      = freq.QuadPart;
    capture_start_qpc_  = now.QuadPart;
    next_capture_qpc_   = now.QuadPart;
    capture_interval_qpc_ = freq.QuadPart / encode_fps_;

    core_.stream_frame_count_.store(0, std::memory_order_relaxed);

    auto on_encoded_cb = [this](const uint8_t* data, size_t len, bool keyframe) {
        if (!sharing_screen_ || !encoder_) return;
        core_.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

        const uint16_t w = static_cast<uint16_t>(encoder_->width());
        const uint16_t h = static_cast<uint16_t>(encoder_->height());
        const VideoCodecId codec = encoder_->codec();

        const auto r = core_.send_video_frame(data, len, keyframe, w, h, codec);
        if (keyframe)
            LOG_INFO("keyframe fn={} size={} sent={}", r.frame_seq, len, r.sent ? 1 : 0);

        if (core_.is_watching(core_.user_id_)) {
            enqueue_decode_work(core_.user_id_, ParsedVideoFrame::assemble(
                VideoFrameHeader{.frame_seq = r.frame_seq, .keyframe = keyframe,
                                 .width = w, .height = h, .codec = codec},
                std::span(data, len)));
        }
    };
    encode_on_encoded_ = on_encoded_cb;

    encode_write_slot_ = 0; encode_ready_slot_ = -1; encode_active_slot_ = -1;
    encode_tex_w_ = 0; encode_tex_h_ = 0; encode_registered_ = false;
    for (int i = 0; i < ENCODE_SLOTS; i++) {
        encode_textures_[i].Reset();
        encode_rtvs_[i].Reset();
        encode_nvenc_slots_[i] = -1;
    }

    encode_running_.store(true, std::memory_order_release);
    encode_thread_ = std::thread([this] { encode_loop(); });

    capture_->set_frame_callback([this](ID3D11Texture2D* texture, uint32_t w, uint32_t h) {
        ZoneScopedN("capture::on_frame");
        if (!sharing_screen_) return;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width < 64 || desc.Height < 64) return;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart < next_capture_qpc_) return;
        next_capture_qpc_ += capture_interval_qpc_;
        if (next_capture_qpc_ < now.QuadPart - capture_interval_qpc_)
            next_capture_qpc_ = now.QuadPart + capture_interval_qpc_;

        if (!core_.video_accounting().admit_frame()) return;

        uint32_t cap_w = (desc.Width  + 1) & ~1u;
        uint32_t cap_h = (desc.Height + 1) & ~1u;

        float sf = scale_factor(core_.model_.share_scale.get());
        uint32_t tex_w = (static_cast<uint32_t>(cap_w * sf) + 1) & ~1u;
        uint32_t tex_h = (static_cast<uint32_t>(cap_h * sf) + 1) & ~1u;
        if (tex_w < 64) tex_w = 64;
        if (tex_h < 64) tex_h = 64;
        bool needs_scale = (tex_w != cap_w || tex_h != cap_h);

        if (encode_tex_w_ != tex_w || encode_tex_h_ != tex_h) {
            std::unique_lock<std::mutex> lock(encode_mutex_);
            encode_cv_.wait(lock, [this] { return encode_active_slot_ < 0; });
            if (encoder_ && encode_registered_) { encoder_->unregister_inputs(); encode_registered_ = false; }
            for (int i = 0; i < ENCODE_SLOTS; i++) encode_nvenc_slots_[i] = -1;

            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = tex_w; sd.Height = tex_h; sd.MipLevels = 1; sd.ArraySize = 1;
            sd.Format = desc.Format; sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = needs_scale ? D3D11_BIND_RENDER_TARGET : 0;
            for (int i = 0; i < ENCODE_SLOTS; i++) {
                encode_textures_[i].Reset();
                encode_rtvs_[i].Reset();
                HRESULT hr = capture_->device()->CreateTexture2D(&sd, nullptr, &encode_textures_[i]);
                if (FAILED(hr)) { LOG_ERROR("CreateTexture2D failed slot {}: {:#010x}", i, static_cast<unsigned>(hr)); return; }
                if (needs_scale) {
                    hr = capture_->device()->CreateRenderTargetView(
                        encode_textures_[i].Get(), nullptr, &encode_rtvs_[i]);
                    if (FAILED(hr)) {
                        LOG_ERROR("CreateRenderTargetView failed slot {}: {:#010x}",
                                  i, static_cast<unsigned>(hr));
                        return;
                    }
                }
            }
            encode_tex_w_ = tex_w; encode_tex_h_ = tex_h;
            encode_write_slot_ = 0; encode_ready_slot_ = -1;

            if (needs_scale) {
                scale_src_tex_.Reset(); scale_src_srv_.Reset();
                D3D11_TEXTURE2D_DESC src_desc{};
                src_desc.Width = cap_w; src_desc.Height = cap_h; src_desc.MipLevels = 1; src_desc.ArraySize = 1;
                src_desc.Format = desc.Format; src_desc.SampleDesc.Count = 1;
                src_desc.Usage = D3D11_USAGE_DEFAULT; src_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                capture_->device()->CreateTexture2D(&src_desc, nullptr, &scale_src_tex_);
                capture_->device()->CreateShaderResourceView(scale_src_tex_.Get(), nullptr, &scale_src_srv_);
                scale_src_w_ = cap_w; scale_src_h_ = cap_h;
                init_scale_pipeline(capture_->device());
            }
        }

        int ws;
        { std::lock_guard<std::mutex> lock(encode_mutex_); ws = encode_write_slot_; }

        if (needs_scale && scale_pipeline_ready_) {
            ZoneScopedN("capture::ScaleBlit");
            auto* ctx = capture_->context();

            // WGC textures are often shader-readable already. Bind them
            // directly and avoid a full-resolution GPU copy before scaling.
            // Some drivers expose capture surfaces without SRV bind support;
            // retain the explicit copy as a compatibility fallback.
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> direct_capture_srv;
            ID3D11ShaderResourceView* source_srv = nullptr;
            if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
                SUCCEEDED(capture_->device()->CreateShaderResourceView(
                    texture, nullptr, &direct_capture_srv))) {
                source_srv = direct_capture_srv.Get();
            } else {
                D3D11_BOX src_box = { 0, 0, 0, desc.Width, desc.Height, 1 };
                ctx->CopySubresourceRegion(scale_src_tex_.Get(), 0, 0, 0, 0, texture, 0, &src_box);
                source_srv = scale_src_srv_.Get();
            }

            D3D11_VIEWPORT vp = { 0, 0, (float)tex_w, (float)tex_h, 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetRenderTargets(1, encode_rtvs_[ws].GetAddressOf(), nullptr);
            ctx->VSSetShader(scale_vs_.Get(), nullptr, 0);
            ctx->PSSetShader(scale_ps_.Get(), nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &source_srv);
            ctx->PSSetSamplers(0, 1, scale_sampler_.GetAddressOf());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(nullptr);
            ctx->Draw(3, 0);

            ID3D11ShaderResourceView* null_srv = nullptr;
            ID3D11RenderTargetView* null_rtv = nullptr;
            ctx->PSSetShaderResources(0, 1, &null_srv);
            ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
            ctx->Flush();
        } else {
            ZoneScopedN("capture::CopyResource");
            D3D11_BOX src_box = { 0, 0, 0, desc.Width, desc.Height, 1 };
            capture_->context()->CopySubresourceRegion(
                encode_textures_[ws].Get(), 0, 0, 0, 0,
                texture, 0, &src_box);
            capture_->context()->Flush();
        }

        {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            encode_ready_slot_ = ws;
            encode_ready_ts_ = (now.QuadPart - capture_start_qpc_) * 10'000'000LL / qpc_frequency_;
            for (int i = 0; i < ENCODE_SLOTS; i++) {
                if (i != encode_ready_slot_ && i != encode_active_slot_) { encode_write_slot_ = i; break; }
            }
        }
        encode_cv_.notify_one();
    });

    stream_audio_capture_ = std::make_unique<StreamAudioCapture>();
    if (stream_audio_capture_->init(audio_source)) {
        stream_audio_capture_->on_encoded_frame = [this](const uint8_t* data, size_t len) {
            if (!sharing_screen_ || !core_.authenticated_) return;
            std::vector<uint8_t> pkt(1 + len);
            pkt[0] = protocol::STREAM_AUDIO_PACKET_TYPE;
            std::memcpy(pkt.data() + 1, data, len);
            core_.net_.send_data(pkt.data(), pkt.size());
        };
        stream_audio_capture_->start();
    } else {
        LOG_WARN("Loopback audio capture unavailable");
        stream_audio_capture_.reset();
    }

    sharing_screen_ = true;
    core_.model_.is_sharing = true;

    BinaryWriter writer;
    writer.write_u8(0); writer.write_u16(0); writer.write_u16(0);
    core_.net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_START,
                            writer.data().data(), writer.data().size());
}

void App::stop_screen_share() {
    ZoneScopedN("App::stop_screen_share");
    if (!sharing_screen_) return;
    sharing_screen_ = false;

    if (core_.is_watching(core_.user_id_))
        core_.remove_watch(core_.user_id_);

    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    if (capture_) capture_->stop_frame_delivery();

    if (encode_thread_.joinable()) {
        encode_running_.store(false, std::memory_order_release);
        encode_cv_.notify_one();
        encode_thread_.join();
    }
    if (encoder_ && encode_registered_) { encoder_->unregister_inputs(); }
    for (auto& t : encode_textures_) t.Reset();
    for (auto& rtv : encode_rtvs_) rtv.Reset();
    scale_src_tex_.Reset(); scale_src_srv_.Reset(); scale_src_w_ = 0; scale_src_h_ = 0;
    scale_vs_.Reset(); scale_ps_.Reset(); scale_sampler_.Reset(); scale_pipeline_ready_ = false;
    encode_tex_w_ = 0; encode_tex_h_ = 0; encode_registered_ = false;
    for (auto& s : encode_nvenc_slots_) s = -1;
    encode_on_encoded_ = nullptr;

    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    if (capture_) ScreenCapture::shutdown_async(std::move(capture_));

    core_.model_.is_sharing = false;

    if (core_.authenticated_ && core_.current_channel_ != 0)
        core_.net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_STOP, nullptr, 0);
}

static int64_t steady_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void App::enqueue_decode_work(UserId sharer_id, ParsedVideoFrame&& frame) {
    ZoneScopedN("App::enqueue_decode_work");
    if (frame.encoded().empty()) return;
    KeyframeRecovery recovery = KeyframeRecovery::None;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = video_streams_.find(sharer_id);
        if (it == video_streams_.end()) return;
        recovery = it->second->inbox.push(std::move(frame), steady_now_us());
    }
    apply_keyframe_recovery(sharer_id, recovery, "Video frame discontinuity");
}

void App::apply_keyframe_recovery(UserId sharer_id, KeyframeRecovery recovery,
                                  std::string_view event) {
    switch (recovery) {
    case KeyframeRecovery::None:
    case KeyframeRecovery::Suppressed:
        return;
    case KeyframeRecovery::RequestFromSharer:
        LOG_WARN("{} for user {}; requesting a keyframe", event, sharer_id);
        core_.send_pli(sharer_id);
        return;
    case KeyframeRecovery::AwaitLocalEncoder:
        LOG_WARN("{} in self-preview; waiting for the encoder's next keyframe", event);
        return;
    case KeyframeRecovery::AwaitPeriodicKeyframe:
        LOG_WARN("{} for user {} keeps recurring; decoder too slow, waiting for the periodic keyframe",
                 event, sharer_id);
        return;
    }
}

void App::encode_loop() {
    ZoneScopedN("App::encode_loop");
    TracySetThreadName("VideoEncode");
    if (parties::raise_thread_to_highest_normal_priority() == parties::ThreadPriorityChange::Failed)
        LOG_WARN("Failed to set the video encode thread to highest priority");

    while (encode_running_.load(std::memory_order_relaxed)) {
        int slot = -1;
        int64_t ts = 0;
        {
            ZoneScopedN("encode::wait");
            std::unique_lock<std::mutex> lock(encode_mutex_);
            encode_cv_.wait(lock, [this] {
                return encode_ready_slot_ >= 0 || !encode_running_.load(std::memory_order_relaxed);
            });
            if (!encode_running_.load(std::memory_order_relaxed)) break;
            if (encode_ready_slot_ < 0) continue;
            slot = encode_ready_slot_; ts = encode_ready_ts_;
            encode_ready_slot_ = -1; encode_active_slot_ = slot;
        }

        uint32_t w = encode_tex_w_, h = encode_tex_h_;

        if (!encoder_ || w != encoder_->width() || h != encoder_->height()) {
            VideoCodecId codec = video_codec_of(core_.model_.share_codec.get());
            encoder_.reset(); encode_registered_ = false;
            auto enc = std::make_unique<VideoEncoder>();
            uint32_t bitrate_bps = core_.video_accounting().bitrate_bps();
            if (bitrate_bps == 0)
                bitrate_bps = static_cast<uint32_t>(core_.model_.share_bitrate * 1'000'000.0f);
            bitrate_bps = (std::max)(bitrate_bps, VIDEO_MIN_BITRATE);
            bitrate_bps = (std::min)(bitrate_bps, VIDEO_MAX_BITRATE);
            if (!enc->init(capture_->device(), w, h, encode_fps_, bitrate_bps, codec)) {
                LOG_ERROR("Encoder init failed at {}x{}", w, h);
                { std::lock_guard<std::mutex> lock(encode_mutex_); encode_active_slot_ = -1; }
                encode_cv_.notify_one();
                continue;
            }
            enc->on_encoded = encode_on_encoded_;
            encoder_ = std::move(enc);

            if (sharing_screen_) {
                BinaryWriter upd;
                upd.write_u8(static_cast<uint8_t>(encoder_->codec()));
                upd.write_u16(static_cast<uint16_t>(encoder_->width()));
                upd.write_u16(static_cast<uint16_t>(encoder_->height()));
                core_.net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_UPDATE,
                                        upd.data().data(), upd.data().size());
            }

            if (encoder_->supports_registered_input()) {
                bool ok = true;
                for (int i = 0; i < ENCODE_SLOTS; i++) {
                    if (encode_textures_[i]) {
                        encode_nvenc_slots_[i] = encoder_->register_input(encode_textures_[i].Get());
                        if (encode_nvenc_slots_[i] < 0) { ok = false; break; }
                    }
                }
                encode_registered_ = ok;
            }
        }

        if (auto b = core_.video_encode_feedback().take_bitrate_update()) {
            uint32_t bitrate_bps = (std::max)(*b, VIDEO_MIN_BITRATE);
            bitrate_bps = (std::min)(bitrate_bps, VIDEO_MAX_BITRATE);
            LOG_INFO("Encoder bitrate -> {} kbps", bitrate_bps / 1000);
            encoder_->set_average_bitrate_bps(bitrate_bps);
        }
        if (core_.video_encode_feedback().take_keyframe_request())
            encoder_->force_keyframe();

        {
            ZoneScopedN("encode::frame");
            bool ok;
            if (encode_registered_ && encode_nvenc_slots_[slot] >= 0)
                ok = encoder_->encode_registered(encode_nvenc_slots_[slot], ts);
            else
                ok = encoder_->encode_frame(encode_textures_[slot].Get(), ts);
            if (!ok)
                LOG_ERROR("Encode failed (slot={}, registered={})",
                             slot, encode_registered_ ? 1 : 0);
        }

        { std::lock_guard<std::mutex> lock(encode_mutex_); encode_active_slot_ = -1; }
        encode_cv_.notify_one();
    }
}

App::VideoStream::VideoStream(UserId sharer, StreamOrigin origin) : sharer_id(sharer), inbox(origin) {}

void App::start_video_stream(UserId sharer_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    if (video_streams_.contains(sharer_id)) return;
    auto s = std::make_unique<VideoStream>(
        sharer_id, sharer_id == core_.user_id_ ? StreamOrigin::SelfPreview : StreamOrigin::RemoteSharer);
    s->thread = std::jthread([this, stream = s.get()](std::stop_token stop) { decode_loop(stop, stream); });
    video_streams_.emplace(sharer_id, std::move(s));
}

void App::stop_video_stream(UserId sharer_id) {
    std::unique_ptr<VideoStream> stopped;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = video_streams_.find(sharer_id);
        if (it == video_streams_.end()) return;
        stopped = std::move(it->second);
        video_streams_.erase(it);
    }
}

void App::stop_all_video_streams() {
    std::vector<std::unique_ptr<VideoStream>> stopped;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& [uid, s] : video_streams_) stopped.push_back(std::move(s));
        video_streams_.clear();
    }
}

void App::on_video_decoded(VideoStream* s, const encdec::DecodedFrame& frame) {
    ZoneScopedN("on_decoded::copy_planes");
    s->decoded_since_init = true;
    if (s->suppress_output) return;
    uint32_t w = frame.width, h = frame.height;

    if (frame.native_d3d12_resource && frame.native_owner) {
        std::lock_guard<std::mutex> lock(s->frame_mutex);
        s->native = NativeVideoFrame{
            .resource = frame.native_d3d12_resource,
            .chroma_resource = frame.native_d3d12_chroma_resource,
            .owner = frame.native_owner,
            .ready_fence = frame.native_d3d12_fence,
            .ready_fence_value = frame.native_fence_value,
            .resource_state = frame.native_d3d12_state,
            .rgba = frame.native_rgba,
            .visible_width = w,
            .visible_height = h,
            .texture_width = frame.native_texture_width,
            .texture_height = frame.native_texture_height,
            .crop_x = frame.native_crop_x,
            .crop_y = frame.native_crop_y,
        };
        s->y.clear();
        s->u.clear();
        s->v.clear();
        s->width = w;
        s->height = h;
        s->y_stride = 0;
        s->uv_stride = 0;
        s->nv12 = true;
        s->new_frame.store(true, std::memory_order_release);
        frame_requests_.request();
        return;
    }

    uint32_t half_h = h / 2;
    size_t y_size = static_cast<size_t>(frame.y_stride) * h;
    s->staging_y.resize(y_size);
    std::memcpy(s->staging_y.data(), frame.y_plane, y_size);
    size_t uv_size = static_cast<size_t>(frame.uv_stride) * half_h;
    s->staging_u.resize(uv_size);
    std::memcpy(s->staging_u.data(), frame.u_plane, uv_size);
    if (!frame.nv12 && frame.v_plane) {
        s->staging_v.resize(uv_size);
        std::memcpy(s->staging_v.data(), frame.v_plane, uv_size);
    }
    {
        std::lock_guard<std::mutex> lock(s->frame_mutex);
        s->native.reset();
        s->y.swap(s->staging_y); s->u.swap(s->staging_u); s->v.swap(s->staging_v);
        s->width = w; s->height = h;
        s->y_stride = frame.y_stride; s->uv_stride = frame.uv_stride;
        s->nv12 = frame.nv12;
        s->new_frame.store(true, std::memory_order_release);
    }
    frame_requests_.request();
}

void App::decode_loop(std::stop_token stop, VideoStream* s) {
    TracySetThreadName("VideoDecoder");
    if (parties::raise_thread_to_highest_normal_priority() == parties::ThreadPriorityChange::Failed)
        LOG_WARN("Failed to set a video decode thread to highest priority");

    auto resync_warm_decoder = [this, s](std::string_view event) {
        apply_keyframe_recovery(s->sharer_id, s->inbox.resync_after_stale_backlog(), event);
    };
    auto reset_decoder = [this, s](std::string_view event, bool disable_hardware) {
        const KeyframeRecovery recovery = s->inbox.reset_after_decoder_failure();
        if (disable_hardware) s->hardware_decode_disabled = true;
        s->decoder.reset();
        s->decoded_since_init = false;
        apply_keyframe_recovery(s->sharer_id, recovery, event);
    };
    const auto is_keyframe = [](const DecodeWork& work) { return work.keyframe(); };

    while (auto batch = s->inbox.wait_batch(stop)) {
        ZoneScopedN("App::decode_loop");
        std::deque<DecodeWork>& frames = *batch;
        std::optional<bool> has_keyframe;

        if (frames.size() > kVideoDecodeBacklogWarningFrames) {
            const size_t original_size = frames.size();
            const auto trim = trim_to_latest_keyframe(frames, is_keyframe);
            has_keyframe = trim.found_keyframe;
            if (trim.dropped > 0) {
                LOG_WARN("Decode queue backed up ({} frames); dropped {} frames before "
                         "the newest keyframe and retained {}",
                         original_size, trim.dropped, frames.size());
            } else {
                LOG_WARN("Decode queue backed up ({} contiguous frames); draining without "
                         "resetting the warm decoder",
                         original_size);
            }

            if (should_resync_decode_backlog(
                    frames.size(), trim.found_keyframe, s->decoded_since_init)) {
                resync_warm_decoder("Sustained decode queue overflow");
                continue;
            }
        }

        if (!frames.empty() && s->decoded_since_init) {
            const int64_t now_us = steady_now_us();
            const int64_t oldest_us = frames.front().arrival_us;
            if (now_us - oldest_us >= kVideoDecodeBacklogMaxAgeUs) {
                if (!has_keyframe) has_keyframe = std::ranges::any_of(frames, is_keyframe);
                if (should_resync_stale_decode_backlog(oldest_us, now_us, *has_keyframe,
                                                       s->decoded_since_init)) {
                    if (s->inbox.stale_flushes_suppressed())
                        LOG_DEBUG("Decode backlog for user {} is stale ({} frames, oldest {} ms old)",
                                  s->sharer_id, frames.size(), (now_us - oldest_us) / 1000);
                    else
                        LOG_WARN("Decode backlog for user {} is stale ({} frames, oldest {} ms old)",
                                 s->sharer_id, frames.size(), (now_us - oldest_us) / 1000);
                    resync_warm_decoder("Stale decode backlog");
                    continue;
                }
            }
        }

        for (size_t i = 0; i < frames.size(); ++i) {
            const DecodeWork& work = frames[i];
            const VideoFrameHeader& header = work.frame.header();
            if (s->decoder && s->decoder->context_lost()) {
                reset_decoder("GPU decode context loss", true);
                break;
            }
            if (!s->decoder ||
                s->decoder->codec() != header.codec ||
                s->decoder->width() != header.width ||
                s->decoder->height() != header.height) {
                s->decoder = std::make_unique<VideoDecoder>();
                s->decoded_since_init = false;
                if (s->hardware_decode_disabled)
                    s->decoder->disable_hardware();
                if (!s->decoder->init(header.codec, header.width, header.height,
                                      decode_d3d12_device_, stop)) {
                    LOG_ERROR("Decoder init failed codec={} {}x{}",
                                 static_cast<uint8_t>(header.codec), header.width, header.height);
                    reset_decoder("Decoder initialization failure", false);
                    break;
                }
                LOG_INFO("Decoder reinitialized: {}",  s->decoder->backend_name());
                s->decoder->on_decoded = [this, s](const DecodedFrame& f) { on_video_decoded(s, f); };
            }
            s->suppress_output = i + 1 < frames.size();
            const auto encoded = work.frame.encoded();
            const bool decoded = s->decoder->decode(encoded.data(), encoded.size(), header.frame_seq);
            s->suppress_output = false;
            if (!decoded) {
                if (stop.stop_requested()) break;
                reset_decoder("Bitstream decode failure", s->decoder->context_lost());
                break;
            }
            if (header.keyframe)
                s->inbox.on_keyframe_decoded(steady_now_us(), work.recovery_keyframe);
        }
    }
}

}

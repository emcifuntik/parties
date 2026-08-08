// Windows platform application — wraps AppCore and handles Win32 / D3D11 specifics.

#include <client/app.h>
#include <client/app_core.h>
#include <d3dcompiler.h>
#include <client/screen_capture.h>
#include <client/video_encoder.h>
#include <client/video_decoder.h>
#include <client/video_decode_backlog.h>
#include <client/video_element.h>
#include <client/custom_elements.h>
#include "RmlUi_RenderInterface_Extended.h"
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/profiler.h>
#include <parties/log.h>

#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
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

// Modifier bitmask: 1=Ctrl  2=Shift  4=Alt
static bool is_modifier_vk(int vk) {
    return vk == VK_SHIFT   || vk == VK_CONTROL  || vk == VK_MENU    ||
           vk == VK_LSHIFT  || vk == VK_RSHIFT   ||
           vk == VK_LCONTROL|| vk == VK_RCONTROL ||
           vk == VK_LMENU   || vk == VK_RMENU;
}

static int current_mods() {
    int m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= 1;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= 2;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= 4;
    return m;
}

// key2=0 means no second key; modifiers prefix; key2 (if set) shown before main key
static std::string combo_name(int key, int key2, int mods) {
    std::string name;
    if (mods & 1) name += "Ctrl+";
    if (mods & 2) name += "Shift+";
    if (mods & 4) name += "Alt+";
    if (key2 != 0) { name += vk_to_name(key2); name += "+"; }
    name += vk_to_name(key);
    return name;
}


// ═══════════════════════════════════════════════════════════════════════
// App — Windows platform wrapper around AppCore
// ═══════════════════════════════════════════════════════════════════════

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

// Walk up to the enclosing <selectable_text> element, if any.
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
            sel.clear();  // click elsewhere drops the selection
        }
        return false;  // let RmlUi also handle (hover, links, context menu)
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
            if (!chat_drag_moved_) sel.clear();  // a plain click deselects
        }
        return false;
    }
    case WM_KEYDOWN: {
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) && sel.has_selection()) {
            // Don't steal Ctrl+C from a focused text field (e.g. the compose box).
            Rml::Element* focus = ctx->GetFocusElement();
            if (focus) {
                const Rml::String& tag = focus->GetTagName();
                if (tag == "input" || tag == "textarea") return false;
            }
            std::string text = sel.selected_text();
            if (!text.empty()) set_clipboard_text(hwnd_, text);
            return true;  // consumed
        }
        return false;
    }
    }
    return false;
}

App::App() = default;
App::~App() { shutdown(); }

bool App::init(HWND hwnd) {
    hwnd_ = hwnd;

    // Build PlatformBridge — all callbacks capture hwnd_ and App members
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
        callbacks.set_role = [this, user_id = request.user_id](int role) {
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
        constexpr int ID_DELETE = 1;
        ContextWindowManager::ActionRequest request;
        request.title = "Saved party";
        request.subtitle = "Connection actions";
        request.icon_text = "P";
        request.actions = {{ID_DELETE, "Remove saved party", "Delete this connection", true, false}};
        context_windows_.show_actions(request, [this, id](int command) {
            if (command != ID_DELETE) return;
            core_.settings_.delete_server(id);
            core_.refresh_server_list();
        });
    };

    bridge.show_message_menu = [this](int64_t msg_id) {
        constexpr int ID_COPY   = 1;
        constexpr int ID_PIN    = 2;
        constexpr int ID_DELETE = 3;

        // Snapshot the message text before creating the transient RmlUI HWND.
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

    bridge.open_share_picker = [this]() { show_share_picker(false); };
    bridge.open_audio_share_picker = [this]() { show_share_picker(true); };

    bridge.on_authenticated = nullptr; // Windows needs no special post-auth step

    bridge.stop_screen_share = [this]() { stop_screen_share(); };
    bridge.stop_audio_share = [this]() { stop_application_audio_share(); };

    bridge.request_keyframe = [this]() {
        if (encoder_) encoder_->force_keyframe();
        // Wake the encode thread to re-encode last frame (handles static screens
        // where capture stops delivering frames)
        if (sharing_screen_ && encode_running_.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(encode_mutex_);
            if (encode_ready_slot_ < 0 && encode_active_slot_ < 0) {
                // Re-submit the last written slot
                int last = encode_write_slot_;
                // Find the most recently used slot that isn't the current write slot
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

    // Grid cells are created/destroyed by the data-for binding over model_.watched,
    // and each stream's decoder is torn down by stop_video_stream, so there is no
    // single element to clear here — make it a safe no-op on Windows.
    bridge.clear_video_element = []() {};

    bridge.start_video_stream = [this](UserId id) { start_video_stream(id); };
    bridge.stop_video_stream  = [this](UserId id) { stop_video_stream(id); };

    // Initialize UI
    if (!ui_.init(hwnd)) return false;
    decode_d3d12_device_ = static_cast<ID3D12Device*>(ui_.renderer()->GetD3D12Device());
    if (!context_windows_.init(hwnd, &ui_mutex_)) {
        LOG_ERROR("Context window manager init failed");
        return false;
    }

    // Init AppCore (wires audio, net callbacks, model callbacks)
    if (!core_.init("parties_client.db", std::move(bridge), ui_.context()))
        return false;

    // Feed the chat text selection the ordered message list (id + raw text) so
    // copy and cross-message ordering survive scrolling / element recycling.
    ChatSelection::instance().get_messages = [this]() {
        std::vector<std::pair<int64_t, std::string>> out;
        for (const auto& m : core_.chat_model_.messages.get())
            out.emplace_back(static_cast<int64_t>(m.id), std::string(m.text));
        return out;
    };

    // Wire video frame reception to local on_video_frame_received
    core_.on_video_frame_received = [this](uint32_t sender_id, const uint8_t* data, size_t len) {
        on_video_frame_received(sender_id, data, len);
    };

    // Windows-only: select share target from DXGI list
    core_.model_.on_select_share_target = [this](int index) {
        if (core_.model_.router.is(DocumentRoute::SharePicker))
            core_.model_.router.back();
        cancel_share_thumbnails();
        if (core_.model_.share_picker_mode == 1)
            start_application_audio_share(index);
        else
            start_screen_share(index);
    };
    // Override on_cancel_share to also clear capture targets
    core_.model_.on_cancel_share = [this]() {
        if (core_.model_.router.is(DocumentRoute::SharePicker))
            core_.model_.router.back();
        cancel_share_thumbnails();
        capture_targets_.clear();
        // Audio-only selection uses a temporary enumerator. Never tear down an
        // active screen capture when its picker is cancelled.
        if (core_.model_.share_picker_mode == 0 && capture_) {
            capture_->shutdown();
            capture_.reset();
        }
    };

    core_.model_.on_share_bitrate_changed = [this](float mbps) {
        core_.settings_.set_pref("video.share_bitrate", std::to_string(mbps));
        if (encoder_) {
            uint32_t bps = static_cast<uint32_t>(mbps * 1'000'000.0f);
            bps = (std::max)(bps, VIDEO_MIN_BITRATE);
            bps = (std::min)(bps, VIDEO_MAX_BITRATE);
            encoder_->set_bitrate(bps);
        }
    };

    // Load identity
    if (core_.settings_.has_identity()) {
        auto id = core_.settings_.load_identity();
        if (id) {
            core_.secret_key_  = id->secret_key;
            core_.public_key_  = id->public_key;
            core_.has_identity_ = true;
            LOG_INFO("Identity loaded: {}",
                        parties::public_key_fingerprint(core_.public_key_));
        }
    }

    // Load saved prefs into model/audio
    core_.load_saved_prefs();

    // Load Win32-specific prefs (hotkeys)
    {
        auto pref = [&](const char* key) -> std::string {
            auto v = core_.settings_.get_pref(key);
            return v.value_or("");
        };
        auto load_hotkey = [&](int& key, int& key2, int& mods, Rml::String& name,
                                const char* kKey, const char* kKey2, const char* kMods) {
            std::string v = pref(kKey);
            if (v.empty()) return;
            key  = std::stoi(v);
            std::string v2 = pref(kKey2); key2 = v2.empty() ? 0 : std::stoi(v2);
            std::string vm = pref(kMods); mods = vm.empty() ? 0 : std::stoi(vm);
            name = Rml::String(combo_name(key, key2, mods).c_str());
        };
        load_hotkey(core_.model_.ptt_key.silent(),    core_.model_.ptt_key2,    core_.model_.ptt_mods,
                    core_.model_.ptt_key_name.silent(),
                    "audio.ptt_key",    "audio.ptt_key2",    "audio.ptt_mods");
        load_hotkey(core_.model_.mute_key.silent(),   core_.model_.mute_key2,   core_.model_.mute_mods,
                    core_.model_.mute_key_name.silent(),
                    "audio.mute_key",   "audio.mute_key2",   "audio.mute_mods");
        load_hotkey(core_.model_.deafen_key.silent(), core_.model_.deafen_key2, core_.model_.deafen_mods,
                    core_.model_.deafen_key_name.silent(),
                    "audio.deafen_key", "audio.deafen_key2", "audio.deafen_mods");
    }

    // Sound player (separate device, always running)
    sound_player_.init();

    // Register custom elements before loading document
    register_custom_elements(element_registry_);

    doc_ = ui_.load_document("ui/lobby.rml");
    if (doc_) {
        ui_.show_document(doc_);
    }

    core_.refresh_server_list();

    // Set initial identity state on model
    if (core_.has_identity_) {
        core_.server_model_.has_identity = true;
        core_.server_model_.fingerprint  = Rml::String(
            parties::public_key_fingerprint(core_.public_key_));
    }

    // Start the render thread last: the document is loaded and models are
    // initialized, so it is safe for it to begin touching the RmlUi context.
    render_running_.store(true, std::memory_order_release);
    render_thread_ = std::thread([this] { render_loop(); });

    return true;
}

void App::shutdown() {
    // The thumbnail worker owns a D3D11/WinRT capture session. Stop it while the
    // main graphics runtime is still fully alive so its resources are released
    // from the worker body, not during OS thread teardown after rendering stops.
    cancel_share_thumbnails();
    if (share_thumbnail_thread_.joinable()) {
        share_thumbnail_thread_.request_stop();
        share_thumbnail_cv_.notify_all();
        share_thumbnail_thread_.join();
    }

    // Nothing can enqueue preview textures now. Stop rendering before tearing
    // down the RmlUi context and the remaining application resources.
    render_running_.store(false, std::memory_order_release);
    if (render_thread_.joinable())
        render_thread_.join();

    context_windows_.prepare_shutdown();
    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    stop_application_audio_share();
    // Stop producing frames first, but keep the capture D3D11 device alive
    // until the consumer thread and every encoder registration are gone.
    // NVENC/AMF keep references to resources created by this device.
    if (capture_) capture_->stop();
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
    if (capture_) { capture_->shutdown(); capture_.reset(); }
    capture_targets_.clear();
    stop_all_video_streams();
    core_.shutdown();
    ui_.shutdown();
    context_windows_.shutdown();
}

void App::poll_hotkeys() {
    // Keybind capture — peak-combo approach:
    //   1. Track the maximum simultaneous keys pressed (including Ctrl/Shift/Alt).
    //   2. Finalize only when ALL keys are released after at least one input.
    //   This lets the user press G+M together and have both recorded.
    bool any_binding = core_.model_.ptt_binding || core_.model_.mute_binding || core_.model_.deafen_binding;
    if (any_binding) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            core_.model_.ptt_binding = core_.model_.mute_binding = core_.model_.deafen_binding = false;
            capture_peak_key_ = capture_peak_key2_ = capture_peak_mods_ = 0;
            capture_had_input_ = false;
        } else {
            // Collect currently pressed non-modifier keys (up to 2)
            int pressed[2] = {0, 0};
            int count = 0;
            for (int vk = 1; vk < 256 && count < 2; vk++) {
                if (is_modifier_vk(vk)) continue;
                if (GetAsyncKeyState(vk) & 0x8000) pressed[count++] = vk;
            }
            int mods = current_mods();

            if (count > 0) {
                // Update peak: prefer the combination with more regular keys
                int peak_count = (capture_peak_key2_ != 0) ? 2 : (capture_peak_key_ != 0 ? 1 : 0);
                if (count > peak_count) {
                    capture_peak_key2_ = (count >= 2) ? pressed[0] : 0;
                    capture_peak_key_  = (count >= 2) ? pressed[1] : pressed[0];
                    capture_peak_mods_ = mods;
                }
                capture_had_input_ = true;
            } else if (capture_had_input_ && capture_peak_key_ != 0) {
                // All keys released — finalize
                auto finalize = [&](int& key, int& key2, int& out_mods, Rml::String& name,
                                    bool& binding,
                                    const char* pk, const char* pk2, const char* pm,
                                    const char* dirty_key, const char* dirty_name, const char* dirty_bind) {
                    key     = capture_peak_key_;
                    key2    = capture_peak_key2_;
                    out_mods= capture_peak_mods_;
                    name    = Rml::String(combo_name(key, key2, out_mods).c_str());
                    binding = false;
                    core_.settings_.set_pref(pk,  std::to_string(key));
                    core_.settings_.set_pref(pk2, std::to_string(key2));
                    core_.settings_.set_pref(pm,  std::to_string(out_mods));
                    core_.model_.dirty(dirty_key);
                    core_.model_.dirty(dirty_name);
                    core_.model_.dirty(dirty_bind);
                };
                if (core_.model_.ptt_binding)
                    finalize(core_.model_.ptt_key.silent(), core_.model_.ptt_key2, core_.model_.ptt_mods,
                             core_.model_.ptt_key_name.silent(), core_.model_.ptt_binding.silent(),
                             "audio.ptt_key", "audio.ptt_key2", "audio.ptt_mods",
                             "ptt_key", "ptt_key_name", "ptt_binding");
                if (core_.model_.mute_binding)
                    finalize(core_.model_.mute_key.silent(), core_.model_.mute_key2, core_.model_.mute_mods,
                             core_.model_.mute_key_name.silent(), core_.model_.mute_binding.silent(),
                             "audio.mute_key", "audio.mute_key2", "audio.mute_mods",
                             "mute_key", "mute_key_name", "mute_binding");
                if (core_.model_.deafen_binding)
                    finalize(core_.model_.deafen_key.silent(), core_.model_.deafen_key2, core_.model_.deafen_mods,
                             core_.model_.deafen_key_name.silent(), core_.model_.deafen_binding.silent(),
                             "audio.deafen_key", "audio.deafen_key2", "audio.deafen_mods",
                             "deafen_key", "deafen_key_name", "deafen_binding");
                capture_peak_key_ = capture_peak_key2_ = capture_peak_mods_ = 0;
                capture_had_input_ = false;
            }
        }
    }

    // Returns true when the full hotkey combo (key + optional key2 + mods) is held
    auto combo_held = [](int key, int key2, int mods) -> bool {
        if (!(GetAsyncKeyState(key) & 0x8000)) return false;
        if (key2 != 0 && !(GetAsyncKeyState(key2) & 0x8000)) return false;
        return current_mods() == mods;
    };

    // PTT polling — only controls audio mute, does not touch model_.is_muted
    // PTT is blocked when manually muted or deafened
    if (core_.model_.ptt_enabled && core_.model_.ptt_key != 0 && core_.current_channel_ != 0) {
        bool blocked = core_.model_.is_muted || core_.model_.is_deafened;
        bool held = combo_held(core_.model_.ptt_key, core_.model_.ptt_key2, core_.model_.ptt_mods);
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

    // Mute toggle hotkey (edge-triggered)
    if (core_.model_.mute_key != 0 && core_.current_channel_ != 0) {
        bool held = combo_held(core_.model_.mute_key, core_.model_.mute_key2, core_.model_.mute_mods);
        if (held && !mute_key_held_) {
            if (core_.model_.on_toggle_mute) core_.model_.on_toggle_mute();
        }
        mute_key_held_ = held;
    }

    // Deafen toggle hotkey (edge-triggered)
    if (core_.model_.deafen_key != 0 && core_.current_channel_ != 0) {
        bool held = combo_held(core_.model_.deafen_key, core_.model_.deafen_key2, core_.model_.deafen_mods);
        if (held && !deafen_key_held_) {
            if (core_.model_.on_toggle_deafen) core_.model_.on_toggle_deafen();
        }
        deafen_key_held_ = held;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Render thread — owns the GPU swap chain, per-frame rendering and decoded-video
// delivery. Runs independently of the Win32 message loop so the picture keeps
// updating even while the OS modal move/resize loop parks the message thread.
// ─────────────────────────────────────────────────────────────────────────────

void App::render_loop() {
    TracySetThreadName("Render");
    while (render_running_.load(std::memory_order_acquire)) {
        // Apply deferred resize / DPI on the render thread (it owns the GPU swap
        // chain and the RmlUi context dimensions).
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
        if (ui_.is_minimized()) {
            Sleep(16);          // nothing to draw — don't spin
            continue;
        }
        render_frame();         // self-paced by vsync (BeginFrame wait + present)
    }
}

// Walk a subtree for the <video_frame> whose bound "streamid" attribute matches
// the given sharer id (the grid cells are created by a data-for binding).
static Rml::Element* find_grid_video(Rml::Element* el, uint32_t streamid) {
    if (el->GetTagName() == "video_frame" &&
        el->GetAttribute<int>("streamid", -1) == static_cast<int>(streamid))
        return el;
    const int n = el->GetNumChildren();
    for (int i = 0; i < n; ++i) {
        if (auto* found = find_grid_video(el->GetChild(i), streamid))
            return found;
    }
    return nullptr;
}

static Rml::Element* find_share_thumbnail(Rml::ElementDocument* document, int target_index) {
    if (!document) return nullptr;
    const Rml::String element_id = "share-thumbnail-" + Rml::ToString(target_index);
    return document->GetElementById(element_id);
}

void App::render_frame() {
    ZoneScopedN("App::render_frame");

    const auto frame_start = std::chrono::steady_clock::now();

    if (!ui_.render_begin()) {  // BeginFrame: GPU/vsync wait — no context, no lock
        Sleep(16);              // Invalid/lost renderer: avoid a busy retry loop.
        return;
    }
    const auto begin_complete = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);

        // Deliver each watched stream's latest decoded frame to its grid cell.
        // Swap planes out under the locks, then upload after releasing them so the
        // QUIC receive thread isn't blocked on streams_mutex_ during GPU work.
        if (doc_) {
            ZoneScopedN("App::deliver_video_frames");
            struct PendingUpload {
                uint32_t sharer;
                std::shared_ptr<void> native_owner;
                void* native_resource = nullptr;
                void* native_chroma_resource = nullptr;
                void* native_ready_fence = nullptr;
                uint64_t native_ready_value = 0;
                uint32_t native_resource_state = 0;
                bool native_rgba = false;
                uint32_t native_texture_width = 0;
                uint32_t native_texture_height = 0;
                uint32_t native_crop_x = 0;
                uint32_t native_crop_y = 0;
                std::vector<uint8_t> y, u, v;
                uint32_t w, h, ys, uvs;
                bool nv12;
            };
            std::vector<PendingUpload> uploads;
            {
                std::lock_guard<std::mutex> slock(streams_mutex_);
                for (auto& [uid, sp] : video_streams_) {
                    VideoStream* s = sp.get();
                    if (!s->new_frame.load(std::memory_order_acquire)) continue;
                    std::lock_guard<std::mutex> flock(s->frame_mutex);
                    if (!s->new_frame.load(std::memory_order_relaxed)) continue;
                    PendingUpload up;
                    up.sharer = static_cast<uint32_t>(s->sharer_id);
                    up.native_owner = std::move(s->native_owner);
                    up.native_resource = s->native_resource;
                    up.native_chroma_resource = s->native_chroma_resource;
                    up.native_ready_fence = s->native_ready_fence;
                    up.native_ready_value = s->native_ready_value;
                    up.native_resource_state = s->native_resource_state;
                    up.native_rgba = s->native_rgba;
                    up.native_texture_width = s->native_texture_width;
                    up.native_texture_height = s->native_texture_height;
                    up.native_crop_x = s->native_crop_x;
                    up.native_crop_y = s->native_crop_y;
                    s->native_resource = nullptr;
                    s->native_chroma_resource = nullptr;
                    s->native_ready_fence = nullptr;
                    s->native_ready_value = 0;
                    s->native_resource_state = 0;
                    s->native_rgba = false;
                    s->native_texture_width = 0;
                    s->native_texture_height = 0;
                    s->native_crop_x = 0;
                    s->native_crop_y = 0;
                    up.y.swap(s->y); up.u.swap(s->u); up.v.swap(s->v);
                    up.w = s->width; up.h = s->height;
                    up.ys = s->y_stride; up.uvs = s->uv_stride;
                    up.nv12 = s->nv12;
                    s->new_frame.store(false, std::memory_order_relaxed);
                    uploads.push_back(std::move(up));
                }
            }
            // Resolve each sharer's grid cell by walking the grid for the
            // <video_frame> tagged with the matching "streamid" attribute. This
            // is the same proven attribute-read path SelectableTextElement uses,
            // and avoids relying on GetElementById finding a data-bound id.
            Rml::Element* grid = uploads.empty() ? nullptr : doc_->GetElementById("stream-grid");
            for (auto& up : uploads) {
                if ((!up.native_resource && up.y.empty()) || up.w == 0 || up.h == 0) continue;
                core_.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);
                auto* elem = grid ? find_grid_video(grid, up.sharer) : nullptr;
                if (elem) {
                    auto* ve = static_cast<VideoElement*>(elem);
                    if (up.native_resource)
                        ve->UpdateNativeNV12Frame(
                            up.native_resource, up.native_chroma_resource,
                            std::move(up.native_owner), up.native_ready_fence,
                            up.native_ready_value, up.native_resource_state, up.native_rgba, up.w, up.h,
                            up.native_texture_width, up.native_texture_height,
                            up.native_crop_x, up.native_crop_y);
                    else if (up.nv12)
                        ve->UpdateNV12Frame(up.y, up.ys, up.u, up.uvs, up.w, up.h);
                    else
                        ve->UpdateYUVFrame(up.y, up.ys, up.u, up.v, up.uvs, up.w, up.h);
                }
            }
        }

        // Update voice level meter

        // Update FPS + ping in titlebar (once per second)
        fps_frame_count_++;
        auto now_fps = std::chrono::steady_clock::now();
        float elapsed_fps = std::chrono::duration<float>(now_fps - fps_last_update_).count();
        if (elapsed_fps >= 1.0f) {
            int fps = static_cast<int>(fps_frame_count_ / elapsed_fps);
            fps_frame_count_ = 0;
            fps_last_update_ = now_fps;
            if (doc_) {
                Rml::String fps_text(std::to_string(fps) + " fps");
                if (fps_text != titlebar_fps_last_) {
                    if (auto* elem = doc_->GetElementById("titlebar-fps")) {
                        elem->SetInnerRML(fps_text);
                        titlebar_fps_last_ = std::move(fps_text);
                    }
                }
                Rml::String ping_text = core_.model_.is_connected
                    ? Rml::String(std::to_string(core_.model_.ping_ms.get()) + " ms")
                    : Rml::String();
                if (ping_text != titlebar_ping_last_) {
                    if (auto* elem = doc_->GetElementById("titlebar-ping")) {
                        elem->SetInnerRML(ping_text);
                        titlebar_ping_last_ = std::move(ping_text);
                    }
                }
            }
        }

        ui_.update();

        // Target cards are data-bound and only exist after the model update.
        // Upload at most two previews per frame so texture creation never turns
        // into a single large GPU/driver stall on machines with many windows.
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
            if (!remaining.empty()) {
                std::lock_guard lock(share_thumbnail_mutex_);
                for (auto& thumbnail : remaining)
                    pending_share_thumbnails_.push_back(std::move(thumbnail));
            }
        } else if (!core_.model_.router.is(DocumentRoute::SharePicker)) {
            cancel_share_thumbnails();
        }
        ui_.render_body();
    }
    const auto body_complete = std::chrono::steady_clock::now();
    ui_.render_end();           // EndFrame: present (+ DwmFlush) — no context, no lock
    const auto present_complete = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        context_windows_.render();
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

// ─────────────────────────────────────────────────────────────────────────────
// Message-thread logic tick (network, hotkeys, fullscreen). Paused while the OS
// modal move/resize loop runs — harmless, since rendering is on its own thread.
// ─────────────────────────────────────────────────────────────────────────────

void App::tick_message_thread() {
    ZoneScopedN("App::tick_message_thread");

    // Captured window closed (local screen share). Read the flag before locking;
    // stop_screen_share joins the encode thread, which never takes ui_mutex_.
    bool capture_lost = capture_lost_.exchange(false, std::memory_order_relaxed);

    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);

    if (capture_lost) {
        LOG_WARN("Capture target lost, stopping screen share");
        stop_screen_share();
    }

    // Tick shared logic (network messages, speaking state, model updates, etc.)
    core_.tick();

    poll_hotkeys();

    // ESC exits fullscreen stream view
    if (core_.model_.stream_fullscreen && (GetAsyncKeyState(VK_ESCAPE) & 1))
        core_.model_.stream_fullscreen = false;

    // Sync OS window fullscreen state with the model. set_fullscreen performs
    // window ops (SetWindowPos) and must run on the message thread.
    if (ui_.is_fullscreen() != core_.model_.stream_fullscreen)
        ui_.set_fullscreen(core_.model_.stream_fullscreen);
}

void App::defer_resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    pending_w_.store(width, std::memory_order_relaxed);
    pending_h_.store(height, std::memory_order_relaxed);
    resize_pending_.store(true, std::memory_order_release);
}

void App::defer_dpi(float scale) {
    pending_dpi_.store(scale, std::memory_order_relaxed);
    dpi_pending_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen sharing (Windows / DXGI specific)
// ─────────────────────────────────────────────────────────────────────────────

void App::show_share_picker(bool audio_only) {
    ZoneScopedN("App::show_share_picker");
    if ((!audio_only && sharing_screen_) ||
        (audio_only && application_audio_sharing_.load(std::memory_order_acquire)) ||
        !core_.authenticated_ || core_.current_channel_ == 0) return;
    std::unique_ptr<ScreenCapture> audio_picker_capture;
    ScreenCapture* picker = nullptr;
    if (audio_only) {
        audio_picker_capture = std::make_unique<ScreenCapture>();
        picker = audio_picker_capture.get();
    } else {
        capture_ = std::make_unique<ScreenCapture>();
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
    core_.model_.share_picker_mode = audio_only ? 1 : 0;

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
        share_thumbnail_job_active_ = true;
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
    share_thumbnail_job_active_ = false;
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
        }

        // WGC may need hundreds of milliseconds before a minimized or throttled
        // application produces its first frame. Serial capture made one such
        // window block every card behind it. A small bounded worker set keeps the
        // picker responsive without creating a GPU device per enumerated window.
        std::atomic<int> next_target{0};
        const int worker_count = (std::min)(4, static_cast<int>(targets.size()));
        std::vector<std::jthread> capture_workers;
        capture_workers.reserve(worker_count);
        for (int worker_index = 0; worker_index < worker_count; ++worker_index) {
            capture_workers.emplace_back([&, generation] {
                ScreenCapture::ThumbnailSession thumbnail_session;
                while (!stop_token.stop_requested()) {
                    const int index = next_target.fetch_add(1, std::memory_order_relaxed);
                    if (index >= static_cast<int>(targets.size()))
                        break;
                    {
                        std::lock_guard lock(share_thumbnail_mutex_);
                        if (generation != share_thumbnail_generation_)
                            break;
                    }

                    // The cards are 250x118dp. 320x180 retains enough detail for
                    // high DPI without uploading full-size window surfaces.
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
                }
            });
        }
        for (auto& worker : capture_workers)
            worker.join();

        {
            std::lock_guard lock(share_thumbnail_mutex_);
            if (generation == share_thumbnail_generation_)
                share_thumbnail_job_active_ = false;
        }
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
    if (!capture->init(static_cast<uint32_t>(target_pid), StreamAudioCapture::OutputMode::MonoPcm)) {
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

    // Minimal fullscreen triangle VS (no vertex buffer needed)
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
    if (sharing_screen_ || !core_.authenticated_ || core_.current_channel_ == 0) return;

    if (target_index < 0 || target_index >= static_cast<int>(capture_targets_.size())) {
        capture_targets_.clear();
        if (capture_) { capture_->shutdown(); capture_.reset(); }
        return;
    }
    if (!capture_) return;

    const auto& target = capture_targets_[target_index];

    uint32_t target_process_id = 0;
    if (target.type == CaptureTarget::Type::Window && target.handle) {
        DWORD pid = 0;
        GetWindowThreadProcessId(static_cast<HWND>(target.handle), &pid);
        target_process_id = static_cast<uint32_t>(pid);
    }

    constexpr uint32_t fps_presets[] = {15, 30, 60, 120};
    int fps_idx = (std::max)(0, (std::min)(core_.model_.share_fps.get(), 3));
    encode_fps_ = fps_presets[fps_idx];

    if (!capture_->start(target, encode_fps_)) {
        LOG_ERROR("Failed to start capture");
        capture_->shutdown(); capture_.reset(); capture_targets_.clear();
        return;
    }

    capture_->on_closed = [this]() { capture_lost_.store(true, std::memory_order_relaxed); };
    capture_targets_.clear();

    core_.settings_.set_pref("video.share_bitrate", std::to_string(core_.model_.share_bitrate.get()));
    core_.settings_.set_pref("video.share_fps",     std::to_string(core_.model_.share_fps.get()));
    core_.settings_.set_pref("video.share_codec",   std::to_string(core_.model_.share_codec.get()));
    core_.settings_.set_pref("video.share_scale",   std::to_string(core_.model_.share_scale.get()));

    core_.video_frame_number_ = 0;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    qpc_frequency_      = freq.QuadPart;
    capture_start_qpc_  = now.QuadPart;
    last_capture_qpc_   = 0;
    capture_interval_qpc_ = freq.QuadPart / encode_fps_;

    core_.stream_frame_count_.store(0, std::memory_order_relaxed);

    auto on_encoded_cb = [this](const uint8_t* data, size_t len, bool keyframe) {
        if (!sharing_screen_ || !core_.authenticated_ || !encoder_) return;
        core_.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);

        uint32_t fn = core_.video_frame_number_++;
        uint32_t ts = fn;
        uint8_t  flags = keyframe ? VIDEO_FLAG_KEYFRAME : 0;

        if (keyframe)
            LOG_INFO("keyframe fn={} size={}", fn, len);
        uint16_t w = static_cast<uint16_t>(encoder_->width());
        uint16_t h = static_cast<uint16_t>(encoder_->height());
        uint8_t  codec = static_cast<uint8_t>(encoder_->codec());

        constexpr size_t header_len = 1 + 4 + 4 + 1 + 2 + 2 + 1;
        std::array<uint8_t, header_len> pkt{};
        size_t off = 0;
        pkt[off++] = protocol::VIDEO_FRAME_PACKET_TYPE;
        std::memcpy(pkt.data() + off, &fn, 4);    off += 4;
        std::memcpy(pkt.data() + off, &ts, 4);    off += 4;
        pkt[off++] = flags;
        std::memcpy(pkt.data() + off, &w, 2);     off += 2;
        std::memcpy(pkt.data() + off, &h, 2);     off += 2;
        pkt[off++] = codec;
        core_.net_.send_video_parts(pkt.data(), pkt.size(), data, len);

        // Local self-preview feed: if we're watching our own share, decode our
        // encoder output locally into that stream's grid cell.
        if (encoder_ && core_.is_watching(core_.user_id_)) {
            std::vector<uint8_t> copy(data, data + len);
            enqueue_decode_work(core_.user_id_, std::move(copy), static_cast<int64_t>(fn),
                                encoder_->codec(),
                                static_cast<uint16_t>(encoder_->width()),
                                static_cast<uint16_t>(encoder_->height()),
                                keyframe);
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

    capture_->on_frame = [this](ID3D11Texture2D* texture, uint32_t w, uint32_t h) {
        ZoneScopedN("capture::on_frame");
        if (!sharing_screen_) return;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width < 64 || desc.Height < 64) return;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t elapsed = now.QuadPart - last_capture_qpc_;
        if (elapsed < capture_interval_qpc_) return;
        last_capture_qpc_ = now.QuadPart;

        uint32_t cap_w = (desc.Width  + 1) & ~1u;
        uint32_t cap_h = (desc.Height + 1) & ~1u;

        // Apply scale factor
        constexpr float scale_factors[] = {1.0f, 0.75f, 0.5f, 0.25f};
        int scale_idx = (std::max)(0, (std::min)(core_.model_.share_scale.get(), 3));
        float sf = scale_factors[scale_idx];
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

            // Recreate full-res source texture + SRV for scale blit
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

            // Blit with bilinear downscale
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

            // Unbind
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
    };

    stream_audio_capture_ = std::make_unique<StreamAudioCapture>();
    if (stream_audio_capture_->init(target_process_id)) {
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

    // If we were self-previewing our own share, drop just that stream.
    if (core_.is_watching(core_.user_id_))
        core_.remove_watch(core_.user_id_);

    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    // Stop callbacks before joining, while preserving the device and textures
    // used by the encode thread until that thread has fully exited.
    if (capture_) capture_->stop();

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
    if (capture_) { capture_->shutdown(); capture_.reset(); }
    core_.video_frame_number_ = 0;

    core_.model_.is_sharing = false;

    if (core_.authenticated_ && core_.current_channel_ != 0)
        core_.net_.send_message(protocol::ControlMessageType::SCREEN_SHARE_STOP, nullptr, 0);
}

void App::on_video_frame_received(uint32_t sender_id, const uint8_t* data, size_t len) {
    ZoneScopedN("App::on_video_frame_received");
    // data = [fn(4)][ts(4)][flags(1)][w(2)][h(2)][codec(1)][encoded(N)]
    if (len < 14) return;

    uint8_t flags = data[8];
    bool is_keyframe = (flags & VIDEO_FLAG_KEYFRAME) != 0;
    uint16_t width, height;
    std::memcpy(&width,  data + 9,  2);
    std::memcpy(&height, data + 11, 2);
    auto codec = static_cast<VideoCodecId>(data[13]);

    const uint8_t* encoded     = data + 14;
    size_t         encoded_len = len  - 14;
    if (encoded_len == 0) return;

    uint32_t frame_number;
    std::memcpy(&frame_number, data, 4);

    std::vector<uint8_t> copy(encoded, encoded + encoded_len);
    enqueue_decode_work(sender_id, std::move(copy), static_cast<int64_t>(frame_number),
                        codec, width, height, is_keyframe);
}

// Push one encoded frame into a watched stream's decode queue. Holds
// streams_mutex_ across the whole find+push so the stream can't be destroyed
// concurrently by stop_video_stream. A stream waits for its first keyframe
// before queueing anything (decoders need a sequence header to start).
void App::enqueue_decode_work(UserId sharer_id, std::vector<uint8_t>&& encoded,
                              int64_t timestamp, VideoCodecId codec,
                              uint16_t width, uint16_t height, bool is_keyframe) {
    bool request_keyframe = false;
    {
        std::lock_guard<std::mutex> slock(streams_mutex_);
        auto it = video_streams_.find(sharer_id);
        if (it == video_streams_.end()) return;
        VideoStream* s = it->second.get();
        if (!s->running.load(std::memory_order_relaxed)) return;

        std::lock_guard<std::mutex> qlock(s->queue_mutex);
        const auto decision = s->decode_gate.on_frame(
            static_cast<uint32_t>(timestamp), is_keyframe);
        if (decision == VideoDecodeDecision::Discontinuity) {
            while (!s->queue.empty()) s->queue.pop();
            request_keyframe = true;
        } else if (decision == VideoDecodeDecision::Accept) {
            DecodeWork work;
            work.data = std::move(encoded);
            work.timestamp = timestamp;
            work.codec = codec;
            work.width = width;
            work.height = height;
            work.keyframe = is_keyframe;
            s->queue.push(std::move(work));
            s->queue_cv.notify_one();
        }
    }
    if (request_keyframe) {
        LOG_WARN("Video frame discontinuity from user {}; requesting a keyframe", sharer_id);
        core_.send_pli(sharer_id);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Encode thread
// ─────────────────────────────────────────────────────────────────────────────

void App::encode_loop() {
    ZoneScopedN("App::encode_loop");
    TracySetThreadName("VideoEncode");

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
            VideoCodecId codec = core_.model_.share_codec == 2 ? VideoCodecId::H264
                               : core_.model_.share_codec == 1 ? VideoCodecId::H265
                                                                : VideoCodecId::AV1;
            encoder_.reset(); encode_registered_ = false;
            auto enc = std::make_unique<VideoEncoder>();
            uint32_t bitrate_bps = static_cast<uint32_t>(core_.model_.share_bitrate * 1'000'000.0f);
            bitrate_bps = (std::max)(bitrate_bps, VIDEO_MIN_BITRATE);
            bitrate_bps = (std::min)(bitrate_bps, VIDEO_MAX_BITRATE);
            if (!enc->init(capture_->device(), w, h, 0, 0, encode_fps_, bitrate_bps, codec)) {
                LOG_ERROR("Encoder init failed at {}x{}", w, h);
                { std::lock_guard<std::mutex> lock(encode_mutex_); encode_active_slot_ = -1; }
                encode_cv_.notify_one();
                continue;
            }
            enc->on_encoded = encode_on_encoded_;
            encoder_ = std::move(enc);
            core_.video_frame_number_ = 0;

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

// ─────────────────────────────────────────────────────────────────────────────
// Decode thread
// ─────────────────────────────────────────────────────────────────────────────

void App::start_video_stream(UserId sharer_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    if (video_streams_.count(sharer_id)) return;   // already watching
    auto s = std::make_unique<VideoStream>();
    s->sharer_id = sharer_id;
    s->element_id = "screen-share-" + std::to_string(sharer_id);
    s->running.store(true, std::memory_order_relaxed);
    VideoStream* ptr = s.get();
    s->thread = std::thread([this, ptr] { decode_loop(ptr); });
    video_streams_.emplace(sharer_id, std::move(s));
}

void App::stop_video_stream(UserId sharer_id) {
    // Extract+erase under the lock (so the receive thread can't be mid-enqueue on
    // it), then stop+join its thread OUTSIDE the lock so we never block the
    // receive thread on a thread join.
    std::unique_ptr<VideoStream> s;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = video_streams_.find(sharer_id);
        if (it == video_streams_.end()) return;
        s = std::move(it->second);
        video_streams_.erase(it);
    }
    stop_stream_thread(s.get());
    if (s->decoder) { s->decoder->shutdown(); s->decoder.reset(); }
    // s destroyed here — its grid cell is removed by the data-for binding when
    // app_core drops this sharer from model_.watched.
}

void App::stop_all_video_streams() {
    std::vector<std::unique_ptr<VideoStream>> dead;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        for (auto& [uid, sp] : video_streams_) dead.push_back(std::move(sp));
        video_streams_.clear();
    }
    for (auto& s : dead) {
        stop_stream_thread(s.get());
        if (s->decoder) { s->decoder->shutdown(); s->decoder.reset(); }
    }
}

// Signal a decode thread to exit and join it. The running=false store MUST happen
// under queue_mutex: decode_loop waits on queue_cv with a predicate that reads
// running, so mutating running outside that mutex opens a lost-wakeup window
// (notify fires in the gap between the waiter's predicate check and it parking on
// the condvar) that would hang the join forever.
void App::stop_stream_thread(VideoStream* s) {
    {
        std::lock_guard<std::mutex> lock(s->queue_mutex);
        s->running.store(false, std::memory_order_relaxed);
    }
    // NVDEC may be waiting for a surface lease that can only be retired by the
    // render thread. This function is called while the message thread owns the
    // UI mutex, so a plain join would prevent that renderer progress forever.
    s->decode_stop.request_stop();
    s->queue_cv.notify_all();
    if (s->thread.joinable()) s->thread.join();
}

void App::on_video_decoded(VideoStream* s, const encdec::DecodedFrame& frame) {
    ZoneScopedN("on_decoded::copy_planes");
    uint32_t w = frame.width, h = frame.height;

    if (frame.native_d3d12_resource && frame.native_owner) {
        std::lock_guard<std::mutex> lock(s->frame_mutex);
        s->native_owner = frame.native_owner;
        s->native_resource = frame.native_d3d12_resource;
        s->native_chroma_resource = frame.native_d3d12_chroma_resource;
        s->native_ready_fence = frame.native_d3d12_fence;
        s->native_ready_value = frame.native_fence_value;
        s->native_resource_state = frame.native_d3d12_state;
        s->native_rgba = frame.native_rgba;
        s->native_texture_width = frame.native_texture_width;
        s->native_texture_height = frame.native_texture_height;
        s->native_crop_x = frame.native_crop_x;
        s->native_crop_y = frame.native_crop_y;
        s->y.clear();
        s->u.clear();
        s->v.clear();
        s->width = w;
        s->height = h;
        s->y_stride = 0;
        s->uv_stride = 0;
        s->nv12 = true;
        s->new_frame.store(true, std::memory_order_release);
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
        s->native_owner.reset();
        s->native_resource = nullptr;
        s->native_chroma_resource = nullptr;
        s->native_ready_fence = nullptr;
        s->native_ready_value = 0;
        s->native_resource_state = 0;
        s->native_rgba = false;
        s->native_texture_width = 0;
        s->native_texture_height = 0;
        s->native_crop_x = 0;
        s->native_crop_y = 0;
        s->y.swap(s->staging_y); s->u.swap(s->staging_u); s->v.swap(s->staging_v);
        s->width = w; s->height = h;
        s->y_stride = frame.y_stride; s->uv_stride = frame.uv_stride;
        s->nv12 = frame.nv12;
        s->new_frame.store(true, std::memory_order_release);
    }
}

void App::decode_loop(VideoStream* s) {
    TracySetThreadName("VideoDecoder");

    auto resync_warm_decoder_at_keyframe = [this, s](const char* reason) {
        {
            std::lock_guard<std::mutex> lock(s->queue_mutex);
            s->decode_gate.require_keyframe();
            while (!s->queue.empty()) s->queue.pop();
        }
        LOG_WARN("Resynchronizing video decoder for user {} after {}; keeping warm decoder",
                 s->sharer_id, reason);
        core_.send_pli(s->sharer_id);
    };

    auto recover_at_keyframe = [this, s](const char* reason, bool disable_hardware) {
        {
            std::lock_guard<std::mutex> lock(s->queue_mutex);
            s->decode_gate.require_keyframe();
            while (!s->queue.empty()) s->queue.pop();
        }
        if (disable_hardware) s->hardware_decode_disabled = true;
        if (s->decoder) {
            s->decoder->shutdown();
            s->decoder.reset();
        }
        LOG_WARN("Resetting video decoder for user {} after {}; waiting for keyframe",
                 s->sharer_id, reason);
        core_.send_pli(s->sharer_id);
    };

    while (s->running.load(std::memory_order_relaxed)) {
        ZoneScopedN("App::decode_loop");
        std::queue<DecodeWork> batch;
        {
            std::unique_lock<std::mutex> lock(s->queue_mutex);
            s->queue_cv.wait(lock, [s] {
                return !s->queue.empty() || !s->running.load(std::memory_order_relaxed);
            });
            if (!s->running.load(std::memory_order_relaxed)) break;
            batch.swap(s->queue);
        }

        if (batch.size() > kVideoDecodeBacklogWarningFrames) {
            const size_t original_size = batch.size();
            const auto trim = trim_to_latest_keyframe(
                batch, [](const DecodeWork& work) { return work.keyframe; });
            if (trim.dropped > 0) {
                LOG_WARN("Decode queue backed up ({} frames); dropped {} frames before "
                         "the newest keyframe and retained {}",
                         original_size, trim.dropped, batch.size());
            } else {
                LOG_WARN("Decode queue backed up ({} contiguous frames); draining without "
                         "resetting the warm decoder",
                         original_size);
            }

            // This is a sustained overload rather than the normal one-time 4K
            // NVDEC warm-up. With no keyframe in the retained chain there is no
            // safe prefix to drop, so request a new random-access point. Keep the
            // decoder and its expensive CUDA/D3D12 surface pool alive.
            if (should_resync_decode_backlog(
                    batch.size(), trim.found_keyframe, s->decoder != nullptr)) {
                while (!batch.empty()) batch.pop();
                resync_warm_decoder_at_keyframe("sustained decode queue overflow");
                continue;
            }
        }

        while (!batch.empty()) {
            auto& work = batch.front();
            if (s->decoder && s->decoder->context_lost()) {
                while (!batch.empty()) batch.pop();
                recover_at_keyframe("GPU decode context loss", true);
                break;
            }
            if (!s->decoder ||
                s->decoder->codec() != work.codec ||
                s->decoder->width() != work.width ||
                s->decoder->height() != work.height) {
                if (s->decoder) s->decoder->shutdown();
                s->decoder = std::make_unique<VideoDecoder>();
                if (s->hardware_decode_disabled)
                    s->decoder->disable_hardware();
                if (!s->decoder->init(work.codec, work.width, work.height,
                                      decode_d3d12_device_, s->decode_stop.get_token())) {
                    LOG_ERROR("Decoder init failed codec={} {}x{}",
                                 static_cast<uint8_t>(work.codec), work.width, work.height);
                    while (!batch.empty()) batch.pop();
                    recover_at_keyframe("decoder initialization failure", false);
                    break;
                }
                LOG_INFO("Decoder reinitialized: {}",  s->decoder->backend_name());
                s->decoder->on_decoded = [this, s](const DecodedFrame& f) { on_video_decoded(s, f); };
            }
            if (!s->decoder->decode(work.data.data(), work.data.size(), work.timestamp)) {
                if (!s->running.load(std::memory_order_relaxed)) {
                    while (!batch.empty()) batch.pop();
                    break;
                }
                const bool lost_context = s->decoder->context_lost();
                while (!batch.empty()) batch.pop();
                recover_at_keyframe("bitstream decode failure", lost_context);
                break;
            }
            batch.pop();
        }
    }
}

} // namespace parties::client

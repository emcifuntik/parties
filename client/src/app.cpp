// Windows platform application — wraps AppCore and handles Win32 / D3D11 specifics.

#include <client/app.h>
#include <client/app_core.h>
#include <client/context_menu.h>
#include <client/screen_capture.h>
#include <client/video_encoder.h>
#include <client/video_decoder.h>
#include <client/video_element.h>
#include <client/level_meter_element.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/crypto.h>
#include <parties/permissions.h>
#include <parties/profiler.h>

#include <RmlUi/Core/Factory.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
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

// Helper: scan for key press during keybind capture
static bool capture_keybind(int& out_key, Rml::String& out_name, bool& binding,
                             const std::string& pref_key,
                             parties::client::Settings& settings) {
    for (int vk = 1; vk < 256; vk++) {
        if (vk == VK_ESCAPE) {
            if (GetAsyncKeyState(vk) & 0x8000) { binding = false; return true; }
            continue;
        }
        if (GetAsyncKeyState(vk) & 0x8000) {
            out_key  = vk;
            out_name = Rml::String(vk_to_name(vk).c_str());
            binding  = false;
            settings.set_pref(pref_key, std::to_string(vk));
            return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════
// App — Windows platform wrapper around AppCore
// ═══════════════════════════════════════════════════════════════════════

App::App() = default;
App::~App() { shutdown(); }

bool App::init(HWND hwnd, int renderer_id) {
    hwnd_ = hwnd;

    // Build PlatformBridge — all callbacks capture hwnd_ and App members
    PlatformBridge bridge;

    bridge.copy_to_clipboard = [this](const std::string& text) {
        if (text.empty()) return;
        if (!OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            char* dst = static_cast<char*>(GlobalLock(hMem));
            std::memcpy(dst, text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    };

    bridge.play_sound = [this](SoundPlayer::Effect e) {
        sound_player_.play(e);
    };

    bridge.show_channel_menu = [this](int channel_id, std::string name) {
        constexpr int ID_DELETE = 1;
        std::vector<ContextMenu::Item> items;
        items.push_back({L"Delete Channel", ID_DELETE, true});
        int cmd = ContextMenu::show(hwnd_, items);
        if (cmd == ID_DELETE) {
            BinaryWriter writer;
            writer.write_u32(static_cast<uint32_t>(channel_id));
            core_.net_.send_message(protocol::ControlMessageType::ADMIN_DELETE_CHANNEL,
                                    writer.data().data(), writer.data().size());
        }
    };

    bridge.show_server_menu = [this](int id) {
        constexpr int ID_DELETE = 1;
        std::vector<ContextMenu::Item> items;
        items.push_back({L"Delete", ID_DELETE, true});
        int cmd = ContextMenu::show(hwnd_, items);
        if (cmd == ID_DELETE) {
            core_.settings_.delete_server(id);
            core_.refresh_server_list();
        }
    };

    bridge.open_share_picker = [this]() { show_share_picker(); };

    bridge.on_authenticated = nullptr; // Windows needs no special post-auth step

    bridge.stop_screen_share = [this]() { stop_screen_share(); };

    bridge.request_keyframe = [this]() {
        if (encoder_) encoder_->force_keyframe();
    };

    bridge.clear_video_element = [this]() {
        if (doc_) {
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) static_cast<VideoElement*>(elem)->Clear();
        }
    };

    bridge.start_decode_thread = [this]() { start_decode_thread(); };
    bridge.stop_decode_thread  = [this]() { stop_decode_thread(); };

    // Initialize UI
    if (!ui_.init(hwnd, renderer_id)) return false;

    // Init AppCore (wires audio, net callbacks, model callbacks)
    if (!core_.init("parties_client.db", std::move(bridge), ui_.context()))
        return false;

    // Wire parsed video frames to decode queue
    core_.on_decoded_frame_ready = [this](const ParsedVideoFrame& frame) {
        if (decode_running_ && frame.payload_len > 0) {
            DecodeWork work;
            work.data.assign(frame.payload, frame.payload + frame.payload_len);
            work.timestamp = static_cast<int64_t>(frame.frame_number);
            work.codec  = frame.codec;
            work.width  = frame.width;
            work.height = frame.height;
            {
                std::lock_guard<std::mutex> lock(decode_queue_mutex_);
                decode_queue_.push(std::move(work));
            }
            decode_queue_cv_.notify_one();
        }
    };

    // Windows-only: select share target from DXGI list
    core_.model_.on_select_share_target = [this](int index) {
        core_.model_.show_share_picker = false;
        core_.model_.dirty("show_share_picker");
        start_screen_share(index);
    };

    // Override on_cancel_share to also clear capture targets
    core_.model_.on_cancel_share = [this]() {
        core_.model_.show_share_picker = false;
        core_.model_.dirty("show_share_picker");
        capture_targets_.clear();
    };

    // Load identity
    if (core_.settings_.has_identity()) {
        auto id = core_.settings_.load_identity();
        if (id) {
            core_.secret_key_  = id->secret_key;
            core_.public_key_  = id->public_key;
            core_.has_identity_ = true;
            std::printf("[App] Identity loaded: %s\n",
                        parties::public_key_fingerprint(core_.public_key_).c_str());
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
        std::string val;
        val = pref("audio.ptt_key");
        if (!val.empty()) {
            core_.model_.ptt_key      = std::stoi(val);
            core_.model_.ptt_key_name = Rml::String(vk_to_name(core_.model_.ptt_key).c_str());
        }
        val = pref("audio.mute_key");
        if (!val.empty()) {
            core_.model_.mute_key      = std::stoi(val);
            core_.model_.mute_key_name = Rml::String(vk_to_name(core_.model_.mute_key).c_str());
        }
        val = pref("audio.deafen_key");
        if (!val.empty()) {
            core_.model_.deafen_key      = std::stoi(val);
            core_.model_.deafen_key_name = Rml::String(vk_to_name(core_.model_.deafen_key).c_str());
        }
    }

    // Sound player (separate device, always running)
    sound_player_.init();

    // Register custom elements before loading document
    video_instancer_       = std::make_unique<VideoElementInstancer>();
    level_meter_instancer_ = std::make_unique<LevelMeterInstancer>();
    Rml::Factory::RegisterElementInstancer("video_frame", video_instancer_.get());
    Rml::Factory::RegisterElementInstancer("level_meter", level_meter_instancer_.get());

    doc_ = ui_.load_document("ui/lobby.rml");
    if (doc_) {
        ui_.show_document(doc_);
        level_meter_ = static_cast<LevelMeterElement*>(doc_->GetElementById("voice-level-meter"));
    }

    core_.refresh_server_list();

    // Set initial identity state on model
    if (core_.has_identity_) {
        core_.server_model_.has_identity = true;
        core_.server_model_.fingerprint  = Rml::String(
            parties::public_key_fingerprint(core_.public_key_));
        core_.server_model_.dirty("has_identity");
        core_.server_model_.dirty("fingerprint");
    }

    return true;
}

void App::shutdown() {
    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }
    if (encode_thread_.joinable()) {
        encode_running_.store(false, std::memory_order_release);
        encode_cv_.notify_one();
        encode_thread_.join();
    }
    if (encoder_ && encode_registered_) encoder_->unregister_inputs();
    for (auto& t : encode_textures_) t.Reset();
    encode_registered_ = false;
    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }
    stop_decode_thread();
    if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
    core_.shutdown();
    ui_.shutdown();
}

void App::poll_hotkeys() {
    // Keybind capture (scan for any key press)
    if (core_.model_.ptt_binding) {
        if (capture_keybind(core_.model_.ptt_key, core_.model_.ptt_key_name,
                            core_.model_.ptt_binding, "audio.ptt_key", core_.settings_)) {
            core_.model_.dirty("ptt_key");
            core_.model_.dirty("ptt_key_name");
            core_.model_.dirty("ptt_binding");
        }
    }
    if (core_.model_.mute_binding) {
        if (capture_keybind(core_.model_.mute_key, core_.model_.mute_key_name,
                            core_.model_.mute_binding, "audio.mute_key", core_.settings_)) {
            core_.model_.dirty("mute_key");
            core_.model_.dirty("mute_key_name");
            core_.model_.dirty("mute_binding");
        }
    }
    if (core_.model_.deafen_binding) {
        if (capture_keybind(core_.model_.deafen_key, core_.model_.deafen_key_name,
                            core_.model_.deafen_binding, "audio.deafen_key", core_.settings_)) {
            core_.model_.dirty("deafen_key");
            core_.model_.dirty("deafen_key_name");
            core_.model_.dirty("deafen_binding");
        }
    }

    // PTT polling
    if (core_.model_.ptt_enabled && core_.model_.ptt_key != 0 && core_.current_channel_ != 0) {
        bool held = (GetAsyncKeyState(core_.model_.ptt_key) & 0x8000) != 0;
        auto now = std::chrono::steady_clock::now();
        if (held) {
            ptt_held_ = true;
            if (core_.audio_.is_muted()) {
                core_.audio_.set_muted(false);
                core_.model_.is_muted = false;
                core_.model_.dirty("is_muted");
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
                core_.model_.is_muted = true;
                core_.model_.dirty("is_muted");
            }
        }
    }

    // Mute toggle hotkey (edge-triggered)
    if (core_.model_.mute_key != 0 && !core_.model_.ptt_enabled && core_.current_channel_ != 0) {
        bool held = (GetAsyncKeyState(core_.model_.mute_key) & 0x8000) != 0;
        if (held && !mute_key_held_) {
            if (core_.model_.on_toggle_mute) core_.model_.on_toggle_mute();
        }
        mute_key_held_ = held;
    }

    // Deafen toggle hotkey (edge-triggered)
    if (core_.model_.deafen_key != 0 && core_.current_channel_ != 0) {
        bool held = (GetAsyncKeyState(core_.model_.deafen_key) & 0x8000) != 0;
        if (held && !deafen_key_held_) {
            if (core_.model_.on_toggle_deafen) core_.model_.on_toggle_deafen();
        }
        deafen_key_held_ = held;
    }
}

void App::update() {
    ZoneScopedN("App::update");

    // Check if captured window was closed
    if (capture_lost_.exchange(false, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[App] Capture target lost, stopping screen share\n");
        stop_screen_share();
    }

    // Tick shared logic (network messages, speaking state, FPS counter, etc.)
    core_.tick();

    poll_hotkeys();

    // ESC exits fullscreen stream view
    if (core_.model_.stream_fullscreen && (GetAsyncKeyState(VK_ESCAPE) & 1)) {
        core_.model_.stream_fullscreen = false;
        core_.model_.dirty("stream_fullscreen");
    }

    // Sync OS window fullscreen state with model
    if (ui_.is_fullscreen() != core_.model_.stream_fullscreen)
        ui_.set_fullscreen(core_.model_.stream_fullscreen);

    // Deliver latest decoded video frame to VideoElement for GPU rendering
    if (new_frame_available_ && doc_) {
        ZoneScopedN("App::deliver_video_frame");
        std::vector<uint8_t> y, u, v;
        uint32_t w = 0, h = 0, ys = 0, uvs = 0;
        bool nv12 = false;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (new_frame_available_) {
                y.swap(shared_y_); u.swap(shared_u_); v.swap(shared_v_);
                w = shared_width_; h = shared_height_;
                ys = shared_y_stride_; uvs = shared_uv_stride_;
                nv12 = shared_nv12_;
                new_frame_available_ = false;
            }
        }
        if (!y.empty() && w > 0 && h > 0) {
            core_.stream_frame_count_.fetch_add(1, std::memory_order_relaxed);
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) {
                auto* ve = static_cast<VideoElement*>(elem);
                if (nv12)
                    ve->UpdateNV12Frame(y, ys, u, uvs, w, h);
                else
                    ve->UpdateYUVFrame(y.data(), ys, u.data(), v.data(), uvs, w, h);
            }
        }
        // Return spent buffers so staging_ can reuse them
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (!new_frame_available_) {
                shared_y_.swap(y); shared_u_.swap(u); shared_v_.swap(v);
            }
        }
    }

    // Update voice level meter
    update_voice_level();

    // Update FPS counter in titlebar (once per second)
    fps_frame_count_++;
    auto now_fps = std::chrono::steady_clock::now();
    float elapsed_fps = std::chrono::duration<float>(now_fps - fps_last_update_).count();
    if (elapsed_fps >= 1.0f) {
        int fps = static_cast<int>(fps_frame_count_ / elapsed_fps);
        fps_frame_count_ = 0;
        fps_last_update_ = now_fps;
        if (doc_) {
            if (auto* elem = doc_->GetElementById("titlebar-fps"))
                elem->SetInnerRML(Rml::String(std::to_string(fps) + " fps"));
        }
    }

    // Update + render UI
    ui_.update();
    ui_.render_begin();
    ui_.render_body();
    ui_.render_end();
}

void App::update_voice_level() {
    ZoneScopedN("App::update_voice_level");
    if (!level_meter_ || !core_.model_.is_connected) return;
    float level = core_.audio_.voice_level();
    level_meter_->SetLevel(level);
    level_meter_->SetThreshold(core_.model_.vad_threshold);
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen sharing (Windows / DXGI specific)
// ─────────────────────────────────────────────────────────────────────────────

void App::show_share_picker() {
    ZoneScopedN("App::show_share_picker");
    if (core_.sharing_ || !core_.authenticated_ || core_.current_channel_ == 0) return;

    capture_ = std::make_unique<ScreenCapture>();
    if (!capture_->init()) {
        std::fprintf(stderr, "[App] Screen capture init failed\n");
        capture_.reset();
        return;
    }

    capture_targets_.clear();
    core_.model_.share_targets.clear();

    for (auto& m : capture_->enumerate_monitors()) {
        int idx = static_cast<int>(capture_targets_.size());
        ShareTarget st; st.name = Rml::String(m.name); st.index = idx; st.is_monitor = true;
        core_.model_.share_targets.push_back(std::move(st));
        capture_targets_.push_back(std::move(m));
    }
    for (auto& w : capture_->enumerate_windows()) {
        int idx = static_cast<int>(capture_targets_.size());
        ShareTarget st; st.name = Rml::String(w.name); st.index = idx; st.is_monitor = false;
        core_.model_.share_targets.push_back(std::move(st));
        capture_targets_.push_back(std::move(w));
    }

    core_.model_.show_share_picker = true;
    core_.model_.dirty("share_targets");
    core_.model_.dirty("show_share_picker");
}

void App::start_screen_share(int target_index) {
    ZoneScopedN("App::start_screen_share");
    if (core_.sharing_ || !core_.authenticated_ || core_.current_channel_ == 0) return;

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
    int fps_idx = (std::max)(0, (std::min)(core_.model_.share_fps, 3));
    encode_fps_ = fps_presets[fps_idx];

    if (!capture_->start(target, encode_fps_)) {
        std::fprintf(stderr, "[App] Failed to start capture\n");
        capture_->shutdown(); capture_.reset(); capture_targets_.clear();
        return;
    }

    capture_->on_closed = [this]() { capture_lost_.store(true, std::memory_order_relaxed); };
    capture_targets_.clear();

    core_.settings_.set_pref("video.share_bitrate", std::to_string(core_.model_.share_bitrate));
    core_.settings_.set_pref("video.share_fps",     std::to_string(core_.model_.share_fps));

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    qpc_frequency_      = freq.QuadPart;
    capture_start_qpc_  = now.QuadPart;
    last_capture_qpc_   = 0;
    capture_interval_qpc_ = freq.QuadPart / encode_fps_;

    auto on_encoded_cb = [this](const uint8_t* data, size_t len, bool keyframe) {
        if (!core_.sharing_ || !core_.authenticated_ || !encoder_) return;
        uint16_t w = static_cast<uint16_t>(encoder_->width());
        uint16_t h = static_cast<uint16_t>(encoder_->height());
        core_.send_video_frame(data, len, keyframe, w, h, encoder_->codec());

        // Local self-preview feed
        if (core_.viewing_sharer_ == core_.user_id_ && encoder_ && decode_running_) {
            if (core_.awaiting_keyframe_) { if (!keyframe) return; core_.awaiting_keyframe_ = false; }
            DecodeWork work;
            work.data.assign(data, data + len);
            work.timestamp = 0;
            work.codec  = encoder_->codec();
            work.width  = w;
            work.height = h;
            std::lock_guard<std::mutex> lock(decode_queue_mutex_);
            decode_queue_.push(std::move(work));
            decode_queue_cv_.notify_one();
        }
    };
    encode_on_encoded_ = on_encoded_cb;

    encode_write_slot_ = 0; encode_ready_slot_ = -1; encode_active_slot_ = -1;
    encode_tex_w_ = 0; encode_tex_h_ = 0; encode_registered_ = false;
    for (int i = 0; i < ENCODE_SLOTS; i++) { encode_textures_[i].Reset(); encode_nvenc_slots_[i] = -1; }

    encode_running_.store(true, std::memory_order_release);
    encode_thread_ = std::thread([this] { encode_loop(); });

    capture_->on_frame = [this](ID3D11Texture2D* texture, uint32_t w, uint32_t h) {
        ZoneScopedN("capture::on_frame");
        if (!core_.sharing_) return;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width < 64 || desc.Height < 64) return;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t elapsed = now.QuadPart - last_capture_qpc_;
        if (elapsed < capture_interval_qpc_) return;
        last_capture_qpc_ = now.QuadPart;

        uint32_t tex_w = (desc.Width  + 1) & ~1u;
        uint32_t tex_h = (desc.Height + 1) & ~1u;

        if (encode_tex_w_ != tex_w || encode_tex_h_ != tex_h) {
            std::unique_lock<std::mutex> lock(encode_mutex_);
            encode_cv_.wait(lock, [this] { return encode_active_slot_ < 0; });
            if (encoder_ && encode_registered_) { encoder_->unregister_inputs(); encode_registered_ = false; }
            for (int i = 0; i < ENCODE_SLOTS; i++) encode_nvenc_slots_[i] = -1;

            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = tex_w; sd.Height = tex_h; sd.MipLevels = 1; sd.ArraySize = 1;
            sd.Format = desc.Format; sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = 0;
            for (int i = 0; i < ENCODE_SLOTS; i++) {
                encode_textures_[i].Reset();
                HRESULT hr = capture_->device()->CreateTexture2D(&sd, nullptr, &encode_textures_[i]);
                if (FAILED(hr)) { std::fprintf(stderr, "[App] CreateTexture2D failed slot %d: 0x%08lx\n", i, hr); return; }
            }
            encode_tex_w_ = tex_w; encode_tex_h_ = tex_h;
            encode_write_slot_ = 0; encode_ready_slot_ = -1;
        }

        int ws;
        { std::lock_guard<std::mutex> lock(encode_mutex_); ws = encode_write_slot_; }

        {
            ZoneScopedN("capture::CopyResource");
            capture_->context()->CopyResource(encode_textures_[ws].Get(), texture);
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
            if (!core_.sharing_ || !core_.authenticated_) return;
            std::vector<uint8_t> pkt(1 + len);
            pkt[0] = protocol::STREAM_AUDIO_PACKET_TYPE;
            std::memcpy(pkt.data() + 1, data, len);
            core_.net_.send_data(pkt.data(), pkt.size());
        };
        stream_audio_capture_->start();
    } else {
        std::fprintf(stderr, "[App] Loopback audio capture unavailable\n");
        stream_audio_capture_.reset();
    }

    // Codec/dimensions not known until encoder init — send placeholder, encode_loop sends UPDATE
    core_.notify_share_started(VideoCodecId::AV1, 0, 0);
}

void App::stop_screen_share() {
    ZoneScopedN("App::stop_screen_share");
    if (!core_.sharing_) return;

    if (core_.viewing_sharer_ == core_.user_id_) {
        stop_decode_thread();
        if (decoder_) { decoder_->shutdown(); decoder_.reset(); }
        if (doc_) {
            auto* elem = doc_->GetElementById("screen-share");
            if (elem) static_cast<VideoElement*>(elem)->Clear();
        }
        core_.viewing_sharer_ = 0;
    }

    if (stream_audio_capture_) { stream_audio_capture_->stop(); stream_audio_capture_.reset(); }
    if (capture_) { capture_->stop(); capture_->shutdown(); capture_.reset(); }

    if (encode_thread_.joinable()) {
        encode_running_.store(false, std::memory_order_release);
        encode_cv_.notify_one();
        encode_thread_.join();
    }
    if (encoder_ && encode_registered_) { encoder_->unregister_inputs(); }
    for (auto& t : encode_textures_) t.Reset();
    encode_tex_w_ = 0; encode_tex_h_ = 0; encode_registered_ = false;
    for (auto& s : encode_nvenc_slots_) s = -1;
    encode_on_encoded_ = nullptr;

    if (encoder_) { encoder_->shutdown(); encoder_.reset(); }

    core_.notify_share_stopped();
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
            VideoCodecId codec = encoder_ ? encoder_->codec() : VideoCodecId::AV1;
            encoder_.reset(); encode_registered_ = false;
            auto enc = std::make_unique<VideoEncoder>();
            uint32_t bitrate_bps = static_cast<uint32_t>(core_.model_.share_bitrate * 1'000'000.0f);
            bitrate_bps = (std::max)(bitrate_bps, VIDEO_MIN_BITRATE);
            bitrate_bps = (std::min)(bitrate_bps, VIDEO_MAX_BITRATE);
            if (!enc->init(capture_->device(), w, h, 0, 0, encode_fps_, bitrate_bps, codec)) {
                std::fprintf(stderr, "[App] Encoder init failed at %ux%u\n", w, h);
                { std::lock_guard<std::mutex> lock(encode_mutex_); encode_active_slot_ = -1; }
                encode_cv_.notify_one();
                continue;
            }
            enc->on_encoded = encode_on_encoded_;
            encoder_ = std::move(enc);
            core_.video_frame_number_ = 0;

            core_.notify_share_updated(encoder_->codec(),
                                       static_cast<uint16_t>(encoder_->width()),
                                       static_cast<uint16_t>(encoder_->height()));

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
                std::fprintf(stderr, "[App] Encode failed (slot=%d, registered=%d)\n",
                             slot, encode_registered_ ? 1 : 0);
        }

        { std::lock_guard<std::mutex> lock(encode_mutex_); encode_active_slot_ = -1; }
        encode_cv_.notify_one();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Decode thread
// ─────────────────────────────────────────────────────────────────────────────

void App::start_decode_thread() {
    if (decode_running_) return;
    if (decoder_)
        decoder_->on_decoded = [this](const DecodedFrame& f) { on_video_decoded(f); };
    decode_running_ = true;
    decode_thread_ = std::thread([this] { decode_loop(); });
}

void App::stop_decode_thread() {
    if (!decode_running_) return;
    decode_running_ = false;
    decode_queue_cv_.notify_all();
    if (decode_thread_.joinable()) decode_thread_.join();
    std::lock_guard<std::mutex> lock(decode_queue_mutex_);
    while (!decode_queue_.empty()) decode_queue_.pop();
}

void App::on_video_decoded(const encdec::DecodedFrame& frame) {
    ZoneScopedN("on_decoded::copy_planes");
    uint32_t w = frame.width, h = frame.height;
    uint32_t half_h = h / 2;
    size_t y_size = static_cast<size_t>(frame.y_stride) * h;
    staging_y_.resize(y_size);
    std::memcpy(staging_y_.data(), frame.y_plane, y_size);
    size_t uv_size = static_cast<size_t>(frame.uv_stride) * half_h;
    staging_u_.resize(uv_size);
    std::memcpy(staging_u_.data(), frame.u_plane, uv_size);
    if (!frame.nv12 && frame.v_plane) {
        staging_v_.resize(uv_size);
        std::memcpy(staging_v_.data(), frame.v_plane, uv_size);
    }
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        shared_y_.swap(staging_y_); shared_u_.swap(staging_u_); shared_v_.swap(staging_v_);
        shared_width_ = w; shared_height_ = h;
        shared_y_stride_ = frame.y_stride; shared_uv_stride_ = frame.uv_stride;
        shared_nv12_ = frame.nv12;
        new_frame_available_ = true;
    }
}

void App::decode_loop() {
    TracySetThreadName("VideoDecoder");
    static constexpr size_t MAX_DECODE_QUEUE = 10;

    while (decode_running_) {
        ZoneScopedN("App::decode_loop");
        std::queue<DecodeWork> batch;
        {
            std::unique_lock<std::mutex> lock(decode_queue_mutex_);
            decode_queue_cv_.wait(lock, [this] {
                return !decode_queue_.empty() || !decode_running_;
            });
            if (!decode_running_) break;
            batch.swap(decode_queue_);
        }

        if (batch.size() > MAX_DECODE_QUEUE) {
            std::fprintf(stderr, "[Video] Decode queue backed up (%zu frames), flushing\n", batch.size());
            if (decoder_) decoder_->flush();
            while (!batch.empty()) batch.pop();
            if (core_.viewing_sharer_ != 0) core_.send_pli(core_.viewing_sharer_);
            continue;
        }

        while (!batch.empty()) {
            auto& work = batch.front();
            if (!decoder_ || decoder_->codec() != work.codec ||
                decoder_->width() != work.width || decoder_->height() != work.height) {
                if (decoder_) decoder_->shutdown();
                decoder_ = std::make_unique<VideoDecoder>();
                if (!decoder_->init(work.codec, work.width, work.height)) {
                    std::fprintf(stderr, "[Video] Decoder init failed codec=%u %ux%u\n",
                                 static_cast<uint8_t>(work.codec), work.width, work.height);
                    decoder_.reset(); batch.pop(); continue;
                }
                decoder_->on_decoded = [this](const DecodedFrame& f) { on_video_decoded(f); };
            }
            decoder_->decode(work.data.data(), work.data.size(), work.timestamp);
            batch.pop();
        }
    }
}

} // namespace parties::client

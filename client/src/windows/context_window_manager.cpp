#include <client/context_window_manager.h>
#include <client/context_window_model.h>
#include <client/win32_window_lifecycle.h>
#include <parties/log.h>

#include "RmlUi_Platform_Win32.h"
#include "RmlUi_RenderInterface_Extended.h"
#include "dx12/Parties_Renderer_DX12.h"

#include <RmlUi/Core.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace parties::client {
namespace {

std::unique_ptr<ExtendedRenderInterface> make_renderer(HWND hwnd) {
    Backend::RmlRendererSettings settings{};
    settings.vsync = false;
    settings.msaa_sample_count = 4;

    auto renderer = std::make_unique<PartiesRenderInterface_DX12>(hwnd, settings);
    if (!renderer || !*renderer)
        return nullptr;
    return std::unique_ptr<ExtendedRenderInterface>(std::move(renderer));
}

Rml::String initials_from_name(const std::string& name) {
    if (name.empty()) return "?";
    Rml::String result;
    bool take = true;
    for (unsigned char ch : name) {
        if (take && ch < 0x80 && ch != ' ') {
            result.push_back(static_cast<char>(std::toupper(ch)));
            if (result.size() == 2) break;
            take = false;
        } else if (ch == ' ') {
            take = true;
        }
    }
    if (result.empty()) result = "?";
    return result;
}

Rml::String role_name(int role) {
    switch (role) {
    case 0: return "Owner";
    case 1: return "Administrator";
    case 2: return "Moderator";
    default: return "Member";
    }
}

} // namespace

struct ContextWindowManager::Impl {
    struct ActiveWindow {
        HWND hwnd = nullptr;
        std::string context_name;
        std::unique_ptr<ExtendedRenderInterface> renderer;
        std::unique_ptr<TextInputMethodEditor_Win32> text_input;
        std::unique_ptr<ContextWindowModel> model;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::atomic<bool> open{false};
        int last_content_height_px = 0;
    };

    HWND owner = nullptr;
    std::recursive_mutex* ui_mutex = nullptr;
    std::unique_ptr<ActiveWindow> active;
    uint64_t next_context_id = 1;
    bool initialised = false;

    static constexpr wchar_t kClassName[] = L"PartiesRmlContextWindow";

    static void apply_rounded_region(HWND hwnd, int width, int height) {
        if (!hwnd || width <= 0 || height <= 0)
            return;
        const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
        const int diameter = std::max(2, static_cast<int>(std::lround(28.0f * scale)));
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
        if (region && !SetWindowRgn(hwnd, region, TRUE))
            DeleteObject(region);
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        // HWND destruction is thread-affine. This message can be posted after
        // the render thread has released the popup's RmlUI/GPU resources. Do
        // not dereference the manager here: shutdown may already be finishing.
        if (OwnerThreadWindowDestroy::handle_message(hwnd, message))
            return 0;
        // This must be handled before taking ui_mutex. The message is posted by
        // the render thread specifically so no thread waits for the HWND owner
        // while holding the RmlUi lock.
        if (OwnerThreadWindowResize::handle_message(hwnd, message, w_param, l_param))
            return 0;

        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, w_param, l_param);

        std::unique_lock<std::recursive_mutex> ui_lock;
        if (self->ui_mutex)
            ui_lock = std::unique_lock<std::recursive_mutex>(*self->ui_mutex);

        ActiveWindow* window = self->active.get();
        if (!window || window->hwnd != hwnd)
            return DefWindowProcW(hwnd, message, w_param, l_param);

        switch (message) {
        case WM_CLOSE:
            window->open.store(false, std::memory_order_release);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w_param) == WA_INACTIVE) {
                window->open.store(false, std::memory_order_release);
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_KEYDOWN:
            if (w_param == VK_ESCAPE) {
                window->open.store(false, std::memory_order_release);
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_SIZE:
            if (w_param != SIZE_MINIMIZED)
                apply_rounded_region(hwnd, LOWORD(l_param), HIWORD(l_param));
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_DPICHANGED: {
            auto* rect = reinterpret_cast<RECT*>(l_param);
            SetWindowPos(hwnd, HWND_TOPMOST, rect->left, rect->top,
                rect->right - rect->left, rect->bottom - rect->top,
                SWP_NOACTIVATE);
            if (self->ui_mutex && window->renderer && window->context) {
                const int width = rect->right - rect->left;
                const int height = rect->bottom - rect->top;
                window->renderer->SetViewport(width, height, true);
                window->context->SetDimensions({width, height});
                window->context->SetDensityIndependentPixelRatio(
                    static_cast<float>(HIWORD(w_param)) / 96.0f);
                window->last_content_height_px = 0;
            }
            return 0;
        }
        default:
            break;
        }

        if (window->context && window->text_input && self->ui_mutex) {
            const bool propagating = RmlWin32::WindowProcedure(
                window->context, *window->text_input, hwnd, message, w_param, l_param);
            if (!propagating) return 0;
        }
        return DefWindowProcW(hwnd, message, w_param, l_param);
    }

    bool register_class() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Impl::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClassName;
        if (RegisterClassExW(&wc)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void request_close() {
        if (!active) return;
        active->open.store(false, std::memory_order_release);
        if (!active->hwnd)
            return;
        const DWORD window_thread = GetWindowThreadProcessId(active->hwnd, nullptr);
        if (window_thread == GetCurrentThreadId())
            ShowWindow(active->hwnd, SW_HIDE);
        else
            PostMessageW(active->hwnd, WM_CLOSE, 0, 0);
    }

    void reset_context() {
        if (!active) return;
        if (active->context) {
            active->context->UnloadAllDocuments();
            Rml::RemoveContext(active->context_name);
            active->context = nullptr;
            active->document = nullptr;
        }
        active->model.reset();
        active->text_input.reset();
    }

    // Must run only after the application's global Rml::Shutdown. Until then,
    // RmlUi's RenderManager owns resources referring to this render interface.
    void destroy_surface_after_rml_shutdown() {
        if (!active) return;
        HWND hwnd = active->hwnd;
        active->model.reset();
        active->text_input.reset();
        active->context = nullptr;
        active->document = nullptr;
        active->renderer.reset();
        active.reset();
        OwnerThreadWindowDestroy::destroy(hwnd);
    }

    bool resize_to_content() {
        if (!active || !active->hwnd || !active->renderer || !active->context ||
            !active->document)
            return false;

        Rml::Element* shell = active->document->GetElementById("context-window-shell");
        if (!shell) return false;

        const float measured_height = shell->GetBox().GetSize(Rml::BoxArea::Border).y;
        if (!std::isfinite(measured_height) || measured_height < 1.0f)
            return false;

        RECT window_rect{};
        if (!GetWindowRect(active->hwnd, &window_rect))
            return false;

        HMONITOR monitor = MonitorFromWindow(active->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoW(monitor, &monitor_info))
            return false;

        const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
        const int desired_height = std::clamp(
            static_cast<int>(std::ceil(measured_height)), 1, work_height);
        if (desired_height == active->last_content_height_px)
            return false;
        const int width = window_rect.right - window_rect.left;

        if (!OwnerThreadWindowResize::request(active->hwnd, width, desired_height))
            return false;

        active->last_content_height_px = desired_height;
        // The render thread owns both the popup renderer and its RmlUi context.
        // Resize those immediately; the HWND placement itself is applied later
        // by its owner thread through the posted message above.
        active->renderer->SetViewport(width, desired_height, true);
        active->context->SetDimensions({width, desired_height});
        return true;
    }

    bool create_window(int width_dp, int height_dp, std::unique_ptr<ContextWindowModel> model) {
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        GetMonitorInfoW(monitor, &monitor_info);

        UINT dpi = GetDpiForWindow(owner);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const int width = static_cast<int>(std::lround(width_dp * scale));
        const int height = static_cast<int>(std::lround(height_dp * scale));
        int x = cursor.x;
        int y = cursor.y;
        x = std::clamp(x, static_cast<int>(monitor_info.rcWork.left),
            static_cast<int>(monitor_info.rcWork.right) - width);
        y = std::clamp(y, static_cast<int>(monitor_info.rcWork.top),
            static_cast<int>(monitor_info.rcWork.bottom) - height);

        if (!active) {
            active = std::make_unique<ActiveWindow>();
            active->hwnd = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                kClassName, L"", WS_POPUP,
                x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), this);
            if (!active->hwnd) {
                active.reset();
                return false;
            }

            DWORD corner = 3; // DWMWCP_ROUND
            DwmSetWindowAttribute(active->hwnd, 33, &corner, sizeof(corner));
            // DWMWA_BORDER_COLOR: 0xFFFFFFFE is DWMWA_COLOR_NONE. 0xFFFFFFFF
            // asks DWM for its default native border, which doubled the custom
            // RmlUI outline around the rounded popup.
            constexpr COLORREF no_border = 0xFFFFFFFEu;
            DwmSetWindowAttribute(active->hwnd, 34, &no_border, sizeof(no_border));
            apply_rounded_region(active->hwnd, width, height);

            active->renderer = make_renderer(active->hwnd);
            if (!active->renderer) {
                HWND failed_hwnd = active->hwnd;
                active.reset();
                OwnerThreadWindowDestroy::destroy(failed_hwnd);
                return false;
            }
        } else {
            active->open.store(false, std::memory_order_release);
            ShowWindow(active->hwnd, SW_HIDE);
            reset_context();
            SetWindowPos(active->hwnd, HWND_TOPMOST, x, y, width, height,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }

        active->renderer->SetViewport(width, height, true);
        active->last_content_height_px = 0;
        active->text_input = std::make_unique<TextInputMethodEditor_Win32>();
        active->context_name = "context-window-" + std::to_string(next_context_id++);
        active->context = Rml::CreateContext(
            active->context_name, {width, height}, active->renderer.get(), active->text_input.get());
        if (!active->context) {
            reset_context();
            return false;
        }
        active->context->SetDensityIndependentPixelRatio(scale);

        active->model = std::move(model);
        active->model->on_close = [this] {
            request_close();
        };
        if (!active->model->init(active->context)) {
            reset_context();
            return false;
        }
        active->document = active->context->LoadDocument("ui/context_window.rml");
        if (!active->document) {
            reset_context();
            return false;
        }
        active->document->SetClass("platform-windows", true);
        active->document->SetClass("platform-desktop", true);
        active->document->Show();
        active->context->Update();
        if (resize_to_content())
            active->context->Update();

        active->open.store(true, std::memory_order_release);
        ShowWindow(active->hwnd, SW_SHOW);
        SetForegroundWindow(active->hwnd);
        SetFocus(active->hwnd);
        return true;
    }
};

ContextWindowManager::ContextWindowManager() : impl_(std::make_unique<Impl>()) {}
ContextWindowManager::~ContextWindowManager() { shutdown(); }

bool ContextWindowManager::init(HWND owner, std::recursive_mutex* ui_mutex) {
    impl_->owner = owner;
    impl_->ui_mutex = ui_mutex;
    impl_->initialised = impl_->register_class();
    return impl_->initialised;
}

void ContextWindowManager::prepare_shutdown() {
    if (!impl_) return;
    if (impl_->ui_mutex) {
        std::lock_guard<std::recursive_mutex> lock(*impl_->ui_mutex);
        impl_->request_close();
        impl_->reset_context();
    } else {
        impl_->request_close();
        impl_->reset_context();
    }
    impl_->initialised = false;
}

void ContextWindowManager::shutdown() {
    if (!impl_) return;
    impl_->destroy_surface_after_rml_shutdown();
    impl_->initialised = false;
}

void ContextWindowManager::show_user(const UserRequest& request, UserCallbacks callbacks) {
    if (!impl_->initialised) return;
    auto model = std::make_unique<ContextWindowModel>();
    model->user_mode = true;
    model->title = request.name;
    model->subtitle = "In voice · " + request.channel_name;
    model->icon_text = initials_from_name(request.name);
    model->role_label = role_name(request.role);
    model->volume = request.volume;
    model->volume_text = std::to_string(static_cast<int>(std::lround(request.volume * 100.0f))) + "%";
    model->music_volume = request.music_volume;
    model->music_volume_text =
        std::to_string(static_cast<int>(std::lround(request.music_volume * 100.0f))) + "%";
    model->compression = request.compression;
    model->compression_target = request.compression_target;

    auto& actions = model->actions.silent();
    if (request.can_manage_roles) {
        if (request.role != 1) actions.push_back({"Make administrator", "Full moderation access", 101, false, false});
        if (request.role != 2) actions.push_back({"Make moderator", "Manage conversations", 102, false, false});
        if (request.role != 3) actions.push_back({"Make member", "Standard permissions", 103, false, false});
    }
    if (request.can_kick) {
        if (!actions.empty()) actions.push_back({"", "", 0, false, true});
        actions.push_back({"Remove from party", "Disconnect this member", 200, true, false});
    }
    model->has_actions = !actions.empty();

    model->on_volume = std::move(callbacks.set_volume);
    model->on_music_volume = std::move(callbacks.set_music_volume);
    model->on_compression = std::move(callbacks.set_compression);
    model->on_action = [this, callbacks = std::move(callbacks)](int id) mutable {
        if (id == 101 && callbacks.set_role) callbacks.set_role(1);
        else if (id == 102 && callbacks.set_role) callbacks.set_role(2);
        else if (id == 103 && callbacks.set_role) callbacks.set_role(3);
        else if (id == 200 && callbacks.kick) callbacks.kick();
        impl_->request_close();
    };
    int action_height = 0;
    for (const auto& action : model->actions.get())
        action_height += action.separator ? 11 : 48;
    impl_->create_window(420, 270 + action_height, std::move(model));
}

void ContextWindowManager::show_actions(const ActionRequest& request,
                                        std::function<void(int)> on_action) {
    if (!impl_->initialised || request.actions.empty()) return;
    auto model = std::make_unique<ContextWindowModel>();
    model->title = request.title;
    model->subtitle = request.subtitle;
    model->icon_text = request.icon_text;
    model->room_icon = request.room_icon;
    auto& actions = model->actions.silent();
    for (const auto& action : request.actions)
        actions.push_back({action.label, action.detail, action.id, action.danger, action.separator});
    model->has_actions = true;
    model->on_action = [this, on_action = std::move(on_action)](int id) {
        if (on_action) on_action(id);
        impl_->request_close();
    };
    const int action_height = static_cast<int>(request.actions.size()) * 54;
    impl_->create_window(request.width_dp, 92 + action_height, std::move(model));
}

void ContextWindowManager::render() {
    if (!impl_->active) return;
    if (!impl_->active->open.load(std::memory_order_acquire))
        return;
    auto& window = *impl_->active;
    window.context->Update();
    if (impl_->resize_to_content())
        window.context->Update();
    window.renderer->BeginFrame();
    if (!window.renderer->IsFrameActive()) return;
    window.renderer->Clear();
    window.context->Render();
    window.renderer->EndFrame();
}

void ContextWindowManager::close() {
    impl_->request_close();
}

bool ContextWindowManager::is_open() const {
    return impl_ && impl_->active && impl_->active->open.load(std::memory_order_acquire);
}

} // namespace parties::client

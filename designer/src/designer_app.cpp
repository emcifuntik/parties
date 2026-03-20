#include "designer_app.h"

#include "RmlUi_Renderer_DX12.h"
#include "RmlUi_Platform_Win32.h"
#include <client/slug_font_engine.h>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Debugger.h>

#include <dwmapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

namespace designer {

// ══════════════════════════════════════════════════════════════════════════
// FilesystemFileInterface
// ══════════════════════════════════════════════════════════════════════════

void FilesystemFileInterface::AddSearchPath(const std::string& dir) {
	for (auto& p : search_paths_)
		if (p == dir) return;
	search_paths_.push_back(dir);
}

void FilesystemFileInterface::RemoveSearchPath(const std::string& dir) {
	search_paths_.erase(
		std::remove(search_paths_.begin(), search_paths_.end(), dir),
		search_paths_.end());
}

void FilesystemFileInterface::ClearSearchPaths() { search_paths_.clear(); }

std::string FilesystemFileInterface::Resolve(const std::string& path) {
	// Absolute path
	if (fs::path(path).is_absolute() && fs::exists(path))
		return path;

	// Relative to base directory (parent of .rml file)
	if (!base_dir_.empty()) {
		auto candidate = fs::path(base_dir_) / path;
		if (fs::exists(candidate))
			return candidate.string();
	}

	// Search paths
	for (auto& sp : search_paths_) {
		auto candidate = fs::path(sp) / path;
		if (fs::exists(candidate))
			return candidate.string();
	}

	// Last resort: try as-is (may be relative to CWD)
	if (fs::exists(path))
		return path;

	return {};
}

Rml::FileHandle FilesystemFileInterface::Open(const Rml::String& path) {
	std::string resolved = Resolve(path);
	if (resolved.empty()) {
		std::printf("[Designer] File not found: %s\n", path.c_str());
		return 0;
	}
	FILE* f = std::fopen(resolved.c_str(), "rb");
	return reinterpret_cast<Rml::FileHandle>(f);
}

void FilesystemFileInterface::Close(Rml::FileHandle file) {
	if (file) std::fclose(reinterpret_cast<FILE*>(file));
}

size_t FilesystemFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
	return std::fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
}

bool FilesystemFileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
	return std::fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
}

size_t FilesystemFileInterface::Tell(Rml::FileHandle file) {
	return static_cast<size_t>(std::ftell(reinterpret_cast<FILE*>(file)));
}

size_t FilesystemFileInterface::Length(Rml::FileHandle file) {
	auto* f = reinterpret_cast<FILE*>(file);
	long cur = std::ftell(f);
	std::fseek(f, 0, SEEK_END);
	long len = std::ftell(f);
	std::fseek(f, cur, SEEK_SET);
	return static_cast<size_t>(len);
}

// ══════════════════════════════════════════════════════════════════════════
// DesignerApp
// ══════════════════════════════════════════════════════════════════════════

DesignerApp::DesignerApp() = default;
DesignerApp::~DesignerApp() { Shutdown(); }

const std::vector<std::string>& DesignerApp::GetAssetFolders() const {
	return file_interface_.GetSearchPaths();
}

// ── Window procedures ────────────────────────────────────────────────────

LRESULT CALLBACK DesignerApp::PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	auto* app = reinterpret_cast<DesignerApp*>(GetPropW(hwnd, L"DesignerApp"));

	switch (msg) {
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;

	case WM_SIZE:
		if (app) {
			int w = LOWORD(lp), h = HIWORD(lp);
			app->preview_minimized_ = (wp == SIZE_MINIMIZED);
			if (!app->preview_minimized_ && app->preview_renderer_ && w > 0 && h > 0) {
				app->preview_renderer_->SetViewport(w, h);
				if (app->preview_context_)
					app->preview_context_->SetDimensions({w, h});
			}
		}
		return 0;

	case WM_DPICHANGED:
		if (app) {
			UINT dpi = HIWORD(wp);
			app->dpi_scale_ = static_cast<float>(dpi) / 96.0f;
			if (app->preview_context_)
				app->preview_context_->SetDensityIndependentPixelRatio(app->dpi_scale_);
			if (app->manager_context_)
				app->manager_context_->SetDensityIndependentPixelRatio(app->dpi_scale_);
			RECT* suggested = reinterpret_cast<RECT*>(lp);
			SetWindowPos(hwnd, nullptr,
				suggested->left, suggested->top,
				suggested->right - suggested->left,
				suggested->bottom - suggested->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
		return 0;

	case WM_KEYDOWN:
		if (wp == VK_F5 && app) {
			app->ReloadPreview();
			return 0;
		}
		if (wp == VK_F6 && app && !app->GetDocumentPath().empty()) {
			auto vars_path = fs::path(app->GetDocumentPath()).replace_extension(".vars").string();
			app->SaveBindVars(vars_path);
			return 0;
		}
		if (wp == VK_F8 && app && app->preview_context_) {
			Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
			return 0;
		}
		break;
	}

	// Forward to RmlUi
	if (app && app->preview_context_ && app->text_input_editor_) {
		RmlWin32::WindowProcedure(app->preview_context_, *app->text_input_editor_, hwnd, msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DesignerApp::ManagerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	auto* app = reinterpret_cast<DesignerApp*>(GetPropW(hwnd, L"DesignerApp"));

	switch (msg) {
	case WM_CLOSE:
		// Don't quit — just hide the manager. Preview close quits.
		ShowWindow(hwnd, SW_HIDE);
		return 0;

	case WM_SIZE:
		if (app) {
			int w = LOWORD(lp), h = HIWORD(lp);
			app->manager_minimized_ = (wp == SIZE_MINIMIZED);
			if (!app->manager_minimized_ && app->manager_renderer_ && w > 0 && h > 0) {
				app->manager_renderer_->SetViewport(w, h);
				if (app->manager_context_)
					app->manager_context_->SetDimensions({w, h});
			}
		}
		return 0;
	}

	// Forward to RmlUi
	if (app && app->manager_context_ && app->text_input_editor_) {
		RmlWin32::WindowProcedure(app->manager_context_, *app->text_input_editor_, hwnd, msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Window creation ──────────────────────────────────────────────────────

bool DesignerApp::CreatePreviewWindow() {
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = PreviewWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	wc.lpszClassName = L"RmlDesignerPreview";
	RegisterClassExW(&wc);

	preview_hwnd_ = CreateWindowExW(0, L"RmlDesignerPreview",
		L"RmlUI Designer — Preview",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
		nullptr, nullptr, wc.hInstance, nullptr);
	if (!preview_hwnd_) return false;

	SetPropW(preview_hwnd_, L"DesignerApp", this);
	return true;
}

bool DesignerApp::CreateManagerWindow() {
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = ManagerWndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	wc.lpszClassName = L"RmlDesignerManager";
	RegisterClassExW(&wc);

	manager_hwnd_ = CreateWindowExW(0, L"RmlDesignerManager",
		L"RmlUI Designer — Manager",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 500, 600,
		nullptr, nullptr, wc.hInstance, nullptr);
	if (!manager_hwnd_) return false;

	SetPropW(manager_hwnd_, L"DesignerApp", this);
	return true;
}

// ── Init / Shutdown ──────────────────────────────────────────────────────

bool DesignerApp::Init(const std::string& initial_file) {
	// Create both windows first (renderers need HWND)
	if (!CreatePreviewWindow() || !CreateManagerWindow()) {
		std::printf("[Designer] Failed to create windows\n");
		return false;
	}

	// System interface (shared)
	system_interface_ = std::make_unique<SystemInterface_Win32>();
	system_interface_->SetWindow(preview_hwnd_);

	Rml::SetSystemInterface(system_interface_.get());
	Rml::SetFileInterface(&file_interface_);

	// Create DX12 renderers (one per window)
	Backend::RmlRendererSettings settings{};
	settings.vsync = true;
	settings.msaa_sample_count = 4;

	preview_renderer_ = std::make_unique<RenderInterface_DX12>(
		static_cast<void*>(preview_hwnd_), settings);
	if (!*preview_renderer_) {
		std::printf("[Designer] Failed to create preview DX12 renderer\n");
		return false;
	}

	Backend::RmlRendererSettings manager_settings{};
	manager_settings.vsync = false;
	manager_settings.msaa_sample_count = 4;
	manager_renderer_ = std::make_unique<RenderInterface_DX12>(
		static_cast<void*>(manager_hwnd_), manager_settings);
	if (!*manager_renderer_) {
		std::printf("[Designer] Failed to create manager DX12 renderer\n");
		return false;
	}

	// Slug font engine
	slug_font_engine_ = std::make_unique<SlugFontEngine>();
	preview_renderer_->SetSlugFontEngine(slug_font_engine_.get());
	manager_renderer_->SetSlugFontEngine(slug_font_engine_.get());

	Rml::SetFontEngineInterface(slug_font_engine_.get());

	// Initialize RmlUi with the PREVIEW renderer first
	// (RmlUi captures the global render interface at context creation time)
	Rml::SetRenderInterface(preview_renderer_.get());

	if (!Rml::Initialise()) {
		std::printf("[Designer] Failed to initialise RmlUi\n");
		return false;
	}

	// DPI
	UINT dpi = GetDpiForWindow(preview_hwnd_);
	dpi_scale_ = static_cast<float>(dpi) / 96.0f;

	// Create preview context (uses preview_renderer_)
	{
		RECT rc;
		GetClientRect(preview_hwnd_, &rc);
		int w = rc.right - rc.left, h = rc.bottom - rc.top;
		preview_renderer_->SetViewport(w, h);
		preview_context_ = Rml::CreateContext("preview", {w, h});
		if (!preview_context_) {
			std::printf("[Designer] Failed to create preview context\n");
			return false;
		}
		preview_context_->SetDensityIndependentPixelRatio(dpi_scale_);
	}

	// Create manager context (uses manager_renderer_)
	Rml::SetRenderInterface(manager_renderer_.get());
	{
		RECT rc;
		GetClientRect(manager_hwnd_, &rc);
		int w = rc.right - rc.left, h = rc.bottom - rc.top;
		manager_renderer_->SetViewport(w, h);
		manager_context_ = Rml::CreateContext("manager", {w, h});
		if (!manager_context_) {
			std::printf("[Designer] Failed to create manager context\n");
			return false;
		}
		manager_context_->SetDensityIndependentPixelRatio(dpi_scale_);
	}

	// Debugger on the preview context
	Rml::Debugger::Initialise(preview_context_);

	// Text input editor (shared — IME only works on focused window)
	text_input_editor_ = std::make_unique<TextInputMethodEditor_Win32>();

	// Set up the manager's data model (for variable editor UI)
	SetupManagerDataModel();

	// File watcher callback
	file_watcher_.SetCallback([this](const std::string& path) {
		OnFileChanged(path);
	});

	// Load embedded Inter fonts (always available for manager UI)
	LoadEmbeddedFonts();

	// If an initial file was given, set up its parent as base dir + asset folder
	if (!initial_file.empty()) {
		fs::path abs = fs::absolute(initial_file);
		if (abs.has_parent_path()) {
			std::string parent = abs.parent_path().string();
			file_interface_.SetBaseDirectory(parent);
			AddAssetFolder(parent);
		}
	}

	// Also load fonts from asset folders (for the preview document)
	RegisterFontsFromAssetFolders();

	// Load manager UI
	LoadManagerUI();

	// Show windows
	ShowWindow(preview_hwnd_, SW_SHOW);
	ShowWindow(manager_hwnd_, SW_SHOW);

	// Load initial document
	if (!initial_file.empty()) {
		LoadDocument(initial_file);
	}

	std::printf("[Designer] Initialised (DPI=%.2f)\n", dpi_scale_);
	return true;
}

void DesignerApp::Shutdown() {
	text_input_editor_.reset();

	// Clear data model handles before destroying contexts
	for (auto& [name, model] : data_models_)
		model.handle = {};
	mgr_var_model_ = {};

	if (preview_context_) {
		Rml::RemoveContext("preview");
		preview_context_ = nullptr;
	}
	if (manager_context_) {
		Rml::RemoveContext("manager");
		manager_context_ = nullptr;
	}
	Rml::Shutdown();

	slug_font_engine_.reset();
	preview_renderer_.reset();
	manager_renderer_.reset();
	system_interface_.reset();

	file_watcher_.Clear();

	if (preview_hwnd_) {
		SetPropW(preview_hwnd_, L"DesignerApp", nullptr);
		DestroyWindow(preview_hwnd_);
		preview_hwnd_ = nullptr;
	}
	if (manager_hwnd_) {
		SetPropW(manager_hwnd_, L"DesignerApp", nullptr);
		DestroyWindow(manager_hwnd_);
		manager_hwnd_ = nullptr;
	}
	UnregisterClassW(L"RmlDesignerPreview", GetModuleHandleW(nullptr));
	UnregisterClassW(L"RmlDesignerManager", GetModuleHandleW(nullptr));
}

// ── Message loop ─────────────────────────────────────────────────────────

int DesignerApp::Run() {
	MSG msg{};
	bool running = true;

	while (running) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				running = false;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!running) break;

		// File watcher
		file_watcher_.Poll();

		// Debounced reload
		if (reload_pending_ && (GetTickCount() - last_change_tick_) >= 150) {
			reload_pending_ = false;
			if (font_reload_pending_) {
				font_reload_pending_ = false;
				ReloadFonts();
			}
			ReloadPreview();
		}

		// Update + render both windows
		UpdatePreview();
		RenderPreview();
		UpdateManager();
		RenderManager();

		// If both minimized, throttle
		if (preview_minimized_ && manager_minimized_)
			Sleep(16);
	}

	return static_cast<int>(msg.wParam);
}

// ── Update / Render ──────────────────────────────────────────────────────

void DesignerApp::UpdatePreview() {
	if (!preview_context_ || preview_minimized_) return;
	preview_context_->Update();
}

void DesignerApp::RenderPreview() {
	if (!preview_renderer_ || !*preview_renderer_ || preview_minimized_) return;
	preview_renderer_->BeginFrame();
	preview_renderer_->Clear();
	if (preview_context_) preview_context_->Render();
	preview_renderer_->EndFrame();
}

void DesignerApp::UpdateManager() {
	if (!manager_context_ || manager_minimized_) return;
	if (!IsWindowVisible(manager_hwnd_)) return;
	// Process deferred variable list refresh (set by toggle/set callbacks)
	if (mgr_var_dirty_) {
		mgr_var_dirty_ = false;
		if (mgr_var_model_)
			mgr_var_model_.DirtyVariable("variables");
	}
	manager_context_->Update();
}

void DesignerApp::RenderManager() {
	if (!manager_renderer_ || !*manager_renderer_ || manager_minimized_) return;
	if (!IsWindowVisible(manager_hwnd_)) return;
	manager_renderer_->BeginFrame();
	manager_renderer_->Clear();
	if (manager_context_) manager_context_->Render();
	manager_renderer_->EndFrame();
}

// ── Document loading ─────────────────────────────────────────────────────

void DesignerApp::LoadDocument(const std::string& rml_path) {
	// Resolve to absolute path so .vars lookup and base dir work regardless of CWD
	fs::path abs_path = fs::absolute(rml_path);
	document_path_ = abs_path.string();

	// Set base directory for relative path resolution
	if (abs_path.has_parent_path())
		file_interface_.SetBaseDirectory(abs_path.parent_path().string());

	// Update window title
	std::string title = "RmlUI Designer - " + abs_path.filename().string();
	SetWindowTextA(preview_hwnd_, title.c_str());

	// Auto-load .vars file if it exists alongside the .rml
	{
		auto vars_path = fs::path(abs_path).replace_extension(".vars");
		if (fs::exists(vars_path))
			LoadBindVars(vars_path.string());
	}

	// Load
	if (preview_context_) {
		preview_context_->UnloadAllDocuments();

		// Rebuild data models before loading (the .rml may reference them)
		RebuildPreviewDataModels();

		auto* doc = preview_context_->LoadDocument(document_path_);
		if (doc) {
			doc->Show();
			std::printf("[Designer] Loaded: %s\n", document_path_.c_str());
		} else {
			std::printf("[Designer] Failed to load: %s\n", document_path_.c_str());
		}
	}
}

void DesignerApp::ReloadPreview() {
	if (document_path_.empty()) return;
	std::printf("[Designer] Reloading: %s\n", document_path_.c_str());
	LoadDocument(document_path_);
}

void DesignerApp::ReloadFonts() {
	// Full re-init cycle for font changes
	// Save state
	auto doc_path = document_path_;
	auto asset_folders = file_interface_.GetSearchPaths();
	auto base_dir = file_interface_.GetSearchPaths().empty() ? "" : file_interface_.GetSearchPaths()[0];

	// Unload everything
	if (preview_context_) preview_context_->UnloadAllDocuments();
	if (manager_context_) manager_context_->UnloadAllDocuments();

	// Re-register fonts
	RegisterFontsFromAssetFolders();

	// Reload
	LoadManagerUI();
	if (!doc_path.empty()) LoadDocument(doc_path);
}

// ── Asset folders ────────────────────────────────────────────────────────

void DesignerApp::AddAssetFolder(const std::string& dir) {
	std::string abs_dir = fs::absolute(dir).string();
	file_interface_.AddSearchPath(abs_dir);
	file_watcher_.AddDirectory(abs_dir);
	RegisterFontsFromAssetFolders();
	std::printf("[Designer] Added asset folder: %s\n", abs_dir.c_str());
}

void DesignerApp::RemoveAssetFolder(const std::string& dir) {
	file_interface_.RemoveSearchPath(dir);
	file_watcher_.RemoveDirectory(dir);
	std::printf("[Designer] Removed asset folder: %s\n", dir.c_str());
}

// ── File change handling ─────────────────────────────────────────────────

void DesignerApp::OnFileChanged(const std::string& path) {
	last_change_tick_ = GetTickCount();
	reload_pending_ = true;

	// Check if it's a font file
	auto ext = fs::path(path).extension().string();
	for (auto& c : ext) c = static_cast<char>(std::tolower(c));
	if (ext == ".ttf" || ext == ".otf")
		font_reload_pending_ = true;
}

// ── Font registration ────────────────────────────────────────────────────

void DesignerApp::RegisterFontsFromAssetFolders() {
	for (auto& dir : file_interface_.GetSearchPaths()) {
		try {
			for (auto& entry : fs::recursive_directory_iterator(dir)) {
				if (!entry.is_regular_file()) continue;
				auto ext = entry.path().extension().string();
				for (auto& c : ext) c = static_cast<char>(std::tolower(c));
				if (ext == ".ttf" || ext == ".otf") {
					Rml::LoadFontFace(entry.path().string());
				}
			}
		} catch (const std::exception&) {
			// Directory may not exist
		}
	}
}

// ── Manager UI ───────────────────────────────────────────────────────────

void DesignerApp::LoadManagerUI() {
	if (!manager_context_) return;
	manager_context_->UnloadAllDocuments();

	// Try loading manager.rml from the designer/ui/ directory
	// Look relative to the executable
	char exe_path[MAX_PATH]{};
	GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
	fs::path exe_dir = fs::path(exe_path).parent_path();

	// Try several locations for the manager UI
	std::vector<std::string> candidates = {
		(exe_dir / "designer" / "ui" / "manager.rml").string(),
		(exe_dir.parent_path() / "designer" / "ui" / "manager.rml").string(),
		(exe_dir.parent_path().parent_path() / "designer" / "ui" / "manager.rml").string(),
		// Source tree (for development)
	};

	// Also try relative to CWD
	candidates.push_back("designer/ui/manager.rml");

	for (auto& candidate : candidates) {
		if (fs::exists(candidate)) {
			// Temporarily add the manager UI directory to search paths
			auto mgr_dir = fs::path(candidate).parent_path().string();
			file_interface_.AddSearchPath(mgr_dir);

			auto* doc = manager_context_->LoadDocument(candidate);
			if (doc) {
				doc->Show();
				std::printf("[Designer] Manager UI loaded from: %s\n", candidate.c_str());
				return;
			}
		}
	}

	std::printf("[Designer] Warning: manager.rml not found, manager window will be empty\n");
}

// ── Data model variable management ───────────────────────────────────

static VarType ParseVarType(const Rml::String& s) {
	if (s == "int")        return VarType::Int;
	if (s == "float")      return VarType::Float;
	if (s == "bool")       return VarType::Bool;
	if (s == "string[]")   return VarType::StringArray;
	if (s == "event")      return VarType::Event;
	return VarType::String;
}

static Rml::String VarTypeName(VarType t) {
	switch (t) {
	case VarType::String:      return "string";
	case VarType::Int:         return "int";
	case VarType::Float:       return "float";
	case VarType::Bool:        return "bool";
	case VarType::StringArray: return "string[]";
	case VarType::Event:       return "event";
	}
	return "string";
}

void DesignerApp::AddVariable(const Rml::String& model_name, const Rml::String& name, VarType type) {
	if (model_name.empty() || name.empty()) return;

	auto& model = data_models_[model_name];
	if (model.vars.count(name)) return; // already exists

	model.vars.emplace(name, DesignerVar(type));
	std::printf("[Designer] Added variable: %s.%s (%s)\n",
		model_name.c_str(), name.c_str(), VarTypeName(type).c_str());

	RebuildPreviewDataModels();
	RefreshManagerVarList();
}

void DesignerApp::RemoveVariable(const Rml::String& model_name, const Rml::String& name) {
	auto mit = data_models_.find(model_name);
	if (mit == data_models_.end()) return;
	mit->second.vars.erase(name);
	if (mit->second.vars.empty())
		data_models_.erase(mit);

	std::printf("[Designer] Removed variable: %s.%s\n", model_name.c_str(), name.c_str());
	RebuildPreviewDataModels();
	RefreshManagerVarList();
}

void DesignerApp::SetVariable(const Rml::String& model_name, const Rml::String& name, const Rml::String& value) {
	auto mit = data_models_.find(model_name);
	if (mit == data_models_.end()) return;
	auto vit = mit->second.vars.find(name);
	if (vit == mit->second.vars.end()) return;

	// Events are no-op callbacks, not settable variables
	if (vit->second.type == VarType::Event) return;

	vit->second.SetFromString(value);

	// Dirty the preview variable so RmlUi re-evaluates bindings
	if (mit->second.handle)
		mit->second.handle.DirtyVariable(name);

	// Update just the display value in the manager list (avoid full data-for rebuild)
	Rml::String full_name = model_name + "." + name;
	for (auto& entry : mgr_var_list_) {
		if (entry.name == full_name) {
			entry.value = vit->second.GetAsString();
			break;
		}
	}
	// Don't dirty "variables" here — the input already shows the typed value,
	// and dirtying would cause data-for to rebuild and steal focus.
}

void DesignerApp::RebuildPreviewDataModels() {
	if (!preview_context_) return;

	// Remove existing data models
	for (auto& [model_name, model] : data_models_) {
		if (model.handle) {
			preview_context_->RemoveDataModel(model_name);
			model.handle = {};
		}
	}

	// Recreate each model
	for (auto& [model_name, model] : data_models_) {
		auto ctor = preview_context_->CreateDataModel(model_name);
		if (!ctor) {
			std::printf("[Designer] Failed to create data model: %s\n", model_name.c_str());
			continue;
		}

		// Register array types first (required before binding arrays)
		bool has_string_array = false;
		for (auto& [var_name, var] : model.vars) {
			if (var.type == VarType::StringArray) has_string_array = true;
		}
		if (has_string_array)
			ctor.RegisterArray<Rml::Vector<Rml::String>>();

		// Bind each variable
		for (auto& [var_name, var] : model.vars) {
			switch (var.type) {
			case VarType::String:
				if (var.val_string) ctor.Bind(var_name, var.val_string.get());
				break;
			case VarType::Int:
				if (var.val_int) ctor.Bind(var_name, var.val_int.get());
				break;
			case VarType::Float:
				if (var.val_float) ctor.Bind(var_name, var.val_float.get());
				break;
			case VarType::Bool:
				if (var.val_bool) ctor.Bind(var_name, var.val_bool.get());
				break;
			case VarType::StringArray:
				if (var.val_string_array) ctor.Bind(var_name, var.val_string_array.get());
				break;
			case VarType::Event:
				// Register a no-op event callback so data-event-* expressions don't error
				ctor.BindEventCallback(var_name,
					[](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {});
				break;
			}
		}

		model.handle = ctor.GetModelHandle();
	}
}

void DesignerApp::RefreshManagerVarList() {
	mgr_var_list_.clear();
	for (auto& [model_name, model] : data_models_) {
		for (auto& [var_name, var] : model.vars) {
			// Skip events — they're no-op stubs, not user-editable
			if (var.type == VarType::Event) continue;
			VarListEntry entry;
			entry.name = model_name + "." + var_name;
			entry.type_name = VarTypeName(var.type);
			entry.value = var.GetAsString();
			mgr_var_list_.push_back(entry);
		}
	}
	if (mgr_var_model_)
		mgr_var_model_.DirtyVariable("variables");
}

void DesignerApp::SetupManagerDataModel() {
	if (!manager_context_) return;

	auto ctor = manager_context_->CreateDataModel("designer");
	if (!ctor) return;

	// Register VarListEntry struct
	if (auto s = ctor.RegisterStruct<VarListEntry>()) {
		s.RegisterMember("name", &VarListEntry::name);
		s.RegisterMember("type_name", &VarListEntry::type_name);
		s.RegisterMember("value", &VarListEntry::value);
	}
	ctor.RegisterArray<Rml::Vector<VarListEntry>>();

	// Bind the variable list
	ctor.Bind("variables", &mgr_var_list_);

	// Bind input fields for adding new variables
	mgr_new_model_name_ = "lobby";
	mgr_new_var_name_ = "";
	mgr_new_var_type_ = "string";
	mgr_edit_value_ = "";
	ctor.Bind("new_model", &mgr_new_model_name_);
	ctor.Bind("new_name", &mgr_new_var_name_);
	ctor.Bind("new_type", &mgr_new_var_type_);
	ctor.Bind("edit_value", &mgr_edit_value_);

	// Event: open document file dialog
	ctor.BindEventCallback("browse_document", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		BrowseForDocument();
	});

	// Event: add asset folder dialog
	ctor.BindEventCallback("browse_folder", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		BrowseForAssetFolder();
	});

	// Event: add variable
	ctor.BindEventCallback("add_var", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
		if (!mgr_new_var_name_.empty() && !mgr_new_model_name_.empty()) {
			AddVariable(mgr_new_model_name_, mgr_new_var_name_, ParseVarType(mgr_new_var_type_));
			mgr_new_var_name_ = "";
			if (mgr_var_model_) {
				mgr_var_model_.DirtyVariable("new_name");
			}
		}
	});

	// Event: toggle bool variable (takes "model.name" as argument)
	ctor.BindEventCallback("toggle_var", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty()) return;
		Rml::String full_name = args[0].Get<Rml::String>();
		auto dot = full_name.find('.');
		if (dot == Rml::String::npos) return;
		auto model_name = full_name.substr(0, dot);
		auto var_name = full_name.substr(dot + 1);
		auto mit = data_models_.find(model_name);
		if (mit == data_models_.end()) return;
		auto vit = mit->second.vars.find(var_name);
		if (vit == mit->second.vars.end() || vit->second.type != VarType::Bool) return;
		if (vit->second.val_bool) {
			*vit->second.val_bool = !*vit->second.val_bool;
			// Dirty the preview variable — preview updates instantly
			if (mit->second.handle)
				mit->second.handle.DirtyVariable(var_name);
			// Update manager list entry in-place
			for (auto& entry : mgr_var_list_) {
				if (entry.name == full_name) {
					entry.value = *vit->second.val_bool ? "true" : "false";
					break;
				}
			}
			// Defer the manager UI refresh to next frame so it doesn't
			// block the current frame's preview render
			mgr_var_dirty_ = true;
		}
	});

	// Event: remove variable (takes "model.name" as argument)
	ctor.BindEventCallback("remove_var", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
		if (args.empty()) return;
		Rml::String full_name = args[0].Get<Rml::String>();
		auto dot = full_name.find('.');
		if (dot == Rml::String::npos) return;
		RemoveVariable(full_name.substr(0, dot), full_name.substr(dot + 1));
	});

	// Event: set variable value (takes "model.name", "value")
	ctor.BindEventCallback("set_var", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
		if (args.size() < 2) return;
		Rml::String full_name = args[0].Get<Rml::String>();
		Rml::String value = args[1].Get<Rml::String>();
		auto dot = full_name.find('.');
		if (dot == Rml::String::npos) return;
		SetVariable(full_name.substr(0, dot), full_name.substr(dot + 1), value);
	});

	mgr_var_model_ = ctor.GetModelHandle();
}

// ── Save/Load bind variables ─────────────────────────────────────────

// JSON format (.vars):
// {
//   "lobby": {
//     "is_connected": { "type": "bool", "value": true },
//     "server_name": { "type": "string", "value": "Dev Server" },
//     "on_toggle_mute": { "type": "event" },
//     "items": { "type": "string[]", "value": ["a", "b"] }
//   }
// }

// Minimal JSON writer — no dependency needed
static void JsonWriteString(FILE* f, const Rml::String& s) {
	std::fputc('"', f);
	for (char c : s) {
		if (c == '"') std::fputs("\\\"", f);
		else if (c == '\\') std::fputs("\\\\", f);
		else if (c == '\n') std::fputs("\\n", f);
		else std::fputc(c, f);
	}
	std::fputc('"', f);
}

bool DesignerApp::SaveBindVars(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "w");
	if (!f) {
		std::printf("[Designer] Failed to save: %s\n", path.c_str());
		return false;
	}

	std::fprintf(f, "{\n");
	bool first_model = true;
	for (auto& [model_name, model] : data_models_) {
		if (!first_model) std::fprintf(f, ",\n");
		first_model = false;
		std::fprintf(f, "  \"%s\": {\n", model_name.c_str());

		bool first_var = true;
		for (auto& [var_name, var] : model.vars) {
			if (!first_var) std::fprintf(f, ",\n");
			first_var = false;
			std::fprintf(f, "    \"%s\": { \"type\": \"%s\"", var_name.c_str(), VarTypeName(var.type).c_str());

			switch (var.type) {
			case VarType::String:
				std::fprintf(f, ", \"value\": ");
				JsonWriteString(f, var.GetAsString());
				break;
			case VarType::Int:
				std::fprintf(f, ", \"value\": %s", var.GetAsString().c_str());
				break;
			case VarType::Float:
				std::fprintf(f, ", \"value\": %s", var.GetAsString().c_str());
				break;
			case VarType::Bool:
				std::fprintf(f, ", \"value\": %s", var.val_bool && *var.val_bool ? "true" : "false");
				break;
			case VarType::StringArray:
				std::fprintf(f, ", \"value\": [");
				if (var.val_string_array) {
					for (size_t i = 0; i < var.val_string_array->size(); i++) {
						if (i > 0) std::fprintf(f, ", ");
						JsonWriteString(f, (*var.val_string_array)[i]);
					}
				}
				std::fprintf(f, "]");
				break;
			case VarType::Event:
				break; // no value field
			}
			std::fprintf(f, " }");
		}
		std::fprintf(f, "\n  }");
	}
	std::fprintf(f, "\n}\n");

	std::fclose(f);
	std::printf("[Designer] Saved bind vars: %s\n", path.c_str());
	return true;
}

// Minimal JSON reader — just enough for our format
struct JsonToken {
	enum Type { String, Number, Bool, Null, ObjectOpen, ObjectClose, ArrayOpen, ArrayClose, Colon, Comma, End, Error } type;
	std::string str;
	double num = 0;
	bool bval = false;
};

static const char* SkipWs(const char* p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}

static JsonToken NextToken(const char*& p) {
	p = SkipWs(p);
	JsonToken t;
	switch (*p) {
	case '\0': t.type = JsonToken::End; return t;
	case '{': t.type = JsonToken::ObjectOpen; p++; return t;
	case '}': t.type = JsonToken::ObjectClose; p++; return t;
	case '[': t.type = JsonToken::ArrayOpen; p++; return t;
	case ']': t.type = JsonToken::ArrayClose; p++; return t;
	case ':': t.type = JsonToken::Colon; p++; return t;
	case ',': t.type = JsonToken::Comma; p++; return t;
	case '"': {
		p++;
		t.type = JsonToken::String;
		while (*p && *p != '"') {
			if (*p == '\\' && *(p + 1)) {
				p++;
				if (*p == 'n') t.str += '\n';
				else if (*p == 't') t.str += '\t';
				else t.str += *p;
			} else {
				t.str += *p;
			}
			p++;
		}
		if (*p == '"') p++;
		return t;
	}
	case 't':
		if (std::strncmp(p, "true", 4) == 0) { t.type = JsonToken::Bool; t.bval = true; p += 4; return t; }
		break;
	case 'f':
		if (std::strncmp(p, "false", 5) == 0) { t.type = JsonToken::Bool; t.bval = false; p += 5; return t; }
		break;
	case 'n':
		if (std::strncmp(p, "null", 4) == 0) { t.type = JsonToken::Null; p += 4; return t; }
		break;
	}
	// Number
	if (*p == '-' || (*p >= '0' && *p <= '9')) {
		t.type = JsonToken::Number;
		char* end;
		t.num = std::strtod(p, &end);
		p = end;
		return t;
	}
	t.type = JsonToken::Error;
	return t;
}

bool DesignerApp::LoadBindVars(const std::string& path) {
	// Read entire file
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		std::printf("[Designer] Bind vars file not found: %s\n", path.c_str());
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string json(sz, '\0');
	std::fread(json.data(), 1, sz, f);
	std::fclose(f);

	// Clear existing
	for (auto& [name, model] : data_models_) {
		if (model.handle && preview_context_)
			preview_context_->RemoveDataModel(name);
		model.handle = {};
	}
	data_models_.clear();

	// Parse: { "model": { "var": { "type": "...", "value": ... }, ... }, ... }
	const char* p = json.c_str();
	auto tok = NextToken(p);
	if (tok.type != JsonToken::ObjectOpen) { std::printf("[Designer] Invalid .vars JSON\n"); return false; }

	while (true) {
		tok = NextToken(p);
		if (tok.type == JsonToken::ObjectClose || tok.type == JsonToken::End) break;
		if (tok.type == JsonToken::Comma) continue;
		if (tok.type != JsonToken::String) break;
		std::string model_name = tok.str;

		tok = NextToken(p); // :
		tok = NextToken(p); // {
		if (tok.type != JsonToken::ObjectOpen) break;

		while (true) {
			tok = NextToken(p);
			if (tok.type == JsonToken::ObjectClose) break;
			if (tok.type == JsonToken::Comma) continue;
			if (tok.type != JsonToken::String) break;
			std::string var_name = tok.str;

			tok = NextToken(p); // :
			tok = NextToken(p); // {
			if (tok.type != JsonToken::ObjectOpen) break;

			// Parse var object: { "type": "...", "value": ... }
			std::string type_str;
			std::string value_str;
			bool has_value = false;
			double num_val = 0;
			bool bool_val = false;
			bool is_num = false;
			bool is_bool = false;
			std::vector<std::string> array_val;
			bool is_array = false;

			while (true) {
				tok = NextToken(p);
				if (tok.type == JsonToken::ObjectClose) break;
				if (tok.type == JsonToken::Comma) continue;
				if (tok.type != JsonToken::String) break;
				std::string key = tok.str;

				tok = NextToken(p); // :

				if (key == "type") {
					tok = NextToken(p);
					type_str = tok.str;
				} else if (key == "value") {
					tok = NextToken(p);
					has_value = true;
					if (tok.type == JsonToken::String) {
						value_str = tok.str;
					} else if (tok.type == JsonToken::Number) {
						num_val = tok.num; is_num = true;
					} else if (tok.type == JsonToken::Bool) {
						bool_val = tok.bval; is_bool = true;
					} else if (tok.type == JsonToken::ArrayOpen) {
						is_array = true;
						while (true) {
							tok = NextToken(p);
							if (tok.type == JsonToken::ArrayClose) break;
							if (tok.type == JsonToken::Comma) continue;
							if (tok.type == JsonToken::String) array_val.push_back(tok.str);
						}
					}
				} else {
					// skip unknown key value
					NextToken(p);
				}
			}

			VarType type = ParseVarType(type_str);
			auto& model = data_models_[model_name];
			model.vars.emplace(var_name, DesignerVar(type));
			auto& var = model.vars[var_name];

			if (has_value) {
				switch (type) {
				case VarType::String:
					if (var.val_string) *var.val_string = value_str;
					break;
				case VarType::Int:
					if (var.val_int) *var.val_int = is_num ? static_cast<int>(num_val) : std::atoi(value_str.c_str());
					break;
				case VarType::Float:
					if (var.val_float) *var.val_float = is_num ? static_cast<float>(num_val) : static_cast<float>(std::atof(value_str.c_str()));
					break;
				case VarType::Bool:
					if (var.val_bool) *var.val_bool = is_bool ? bool_val : (value_str == "true");
					break;
				case VarType::StringArray:
					if (var.val_string_array) {
						var.val_string_array->clear();
						for (auto& s : array_val)
							var.val_string_array->push_back(s);
					}
					break;
				case VarType::Event:
					break;
				}
			}
		}
	}

	std::printf("[Designer] Loaded bind vars: %s (%zu models)\n", path.c_str(), data_models_.size());

	// Don't rebuild here — caller (LoadDocument) will rebuild once before loading.
	RefreshManagerVarList();
	return true;
}

// ── File dialogs ─────────────────────────────────────────────────────────

void DesignerApp::BrowseForDocument() {
	wchar_t filename[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = manager_hwnd_;
	ofn.lpstrFilter = L"RML Documents (*.rml)\0*.rml\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = filename;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (GetOpenFileNameW(&ofn)) {
		char u8[MAX_PATH]{};
		WideCharToMultiByte(CP_UTF8, 0, filename, -1, u8, MAX_PATH, nullptr, nullptr);
		LoadDocument(u8);
	}
}

void DesignerApp::BrowseForAssetFolder() {
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	IFileDialog* pfd = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&pfd));
	if (SUCCEEDED(hr)) {
		DWORD options;
		pfd->GetOptions(&options);
		pfd->SetOptions(options | FOS_PICKFOLDERS);
		if (pfd->Show(manager_hwnd_) == S_OK) {
			IShellItem* psi;
			if (pfd->GetResult(&psi) == S_OK) {
				PWSTR path;
				psi->GetDisplayName(SIGDN_FILESYSPATH, &path);
				char u8[MAX_PATH]{};
				WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, MAX_PATH, nullptr, nullptr);
				AddAssetFolder(u8);
				CoTaskMemFree(path);
				psi->Release();
			}
		}
		pfd->Release();
	}
}

} // namespace designer

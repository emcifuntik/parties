#pragma once

// RmlUI Designer — two-window application for live-previewing RmlUI documents.
// Window 1: Preview (renders the target .rml document)
// Window 2: Manager (file browser, asset folders, settings — also RmlUI)

#include "file_watcher.h"

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class RenderInterface_DX12;
class SystemInterface_Win32;
class TextInputMethodEditor_Win32;
class SlugFontEngine;

namespace designer {

// Load Inter fonts embedded in the binary (always available)
void LoadEmbeddedFonts();

// ── Filesystem-based Rml::FileInterface ──────────────────────────────────
// Resolves paths against a list of search directories (asset folders).

class FilesystemFileInterface : public Rml::FileInterface {
public:
	void AddSearchPath(const std::string& dir);
	void RemoveSearchPath(const std::string& dir);
	void ClearSearchPaths();
	const std::vector<std::string>& GetSearchPaths() const { return search_paths_; }

	// Set the base directory (parent of the loaded .rml file)
	void SetBaseDirectory(const std::string& dir) { base_dir_ = dir; }

	Rml::FileHandle Open(const Rml::String& path) override;
	void Close(Rml::FileHandle file) override;
	size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
	bool Seek(Rml::FileHandle file, long offset, int origin) override;
	size_t Tell(Rml::FileHandle file) override;
	size_t Length(Rml::FileHandle file) override;

private:
	std::string Resolve(const std::string& path);
	std::vector<std::string> search_paths_;
	std::string base_dir_;
};

// ── Designer bind variables ───────────────────────────────────────────────
// Dynamically typed variables that get bound to the preview context's data model.
// Supports: string, int, float, bool, string array.
// Heap-allocated so pointers stay stable when the map grows.

enum class VarType { String = 0, Int, Float, Bool, StringArray, Event };

struct DesignerVar {
	VarType type;
	// Heap-allocated value — stable pointer for RmlUi data binding
	std::unique_ptr<Rml::String>               val_string;
	std::unique_ptr<int>                        val_int;
	std::unique_ptr<float>                      val_float;
	std::unique_ptr<bool>                       val_bool;
	std::unique_ptr<Rml::Vector<Rml::String>>   val_string_array;

	DesignerVar() : type(VarType::String) {}
	explicit DesignerVar(VarType t) : type(t) { Allocate(); }

	void Allocate() {
		switch (type) {
		case VarType::String:      val_string = std::make_unique<Rml::String>(); break;
		case VarType::Int:         val_int = std::make_unique<int>(0); break;
		case VarType::Float:       val_float = std::make_unique<float>(0.0f); break;
		case VarType::Bool:        val_bool = std::make_unique<bool>(false); break;
		case VarType::StringArray: val_string_array = std::make_unique<Rml::Vector<Rml::String>>(); break;
		case VarType::Event:       break; // no storage needed
		}
	}

	// Set value from a string representation
	void SetFromString(const Rml::String& s) {
		switch (type) {
		case VarType::Event:       break; // no value
		case VarType::String:      if (val_string) *val_string = s; break;
		case VarType::Int:         if (val_int) *val_int = std::atoi(s.c_str()); break;
		case VarType::Float:       if (val_float) *val_float = static_cast<float>(std::atof(s.c_str())); break;
		case VarType::Bool:        if (val_bool) *val_bool = (s == "true" || s == "1"); break;
		case VarType::StringArray: {
			// Comma-separated values
			if (!val_string_array) break;
			val_string_array->clear();
			size_t start = 0;
			while (start < s.size()) {
				size_t end = s.find(',', start);
				if (end == Rml::String::npos) end = s.size();
				auto item = s.substr(start, end - start);
				// Trim
				while (!item.empty() && item.front() == ' ') item.erase(item.begin());
				while (!item.empty() && item.back() == ' ') item.pop_back();
				if (!item.empty()) val_string_array->push_back(item);
				start = end + 1;
			}
			break;
		}
		}
	}

	// Get value as display string
	Rml::String GetAsString() const {
		switch (type) {
		case VarType::Event:       return "(event)";
		case VarType::String:      return val_string ? *val_string : "";
		case VarType::Int:         return val_int ? std::to_string(*val_int) : "0";
		case VarType::Float:       return val_float ? std::to_string(*val_float) : "0.0";
		case VarType::Bool:        return val_bool ? (*val_bool ? "true" : "false") : "false";
		case VarType::StringArray: {
			if (!val_string_array) return "";
			Rml::String result;
			for (size_t i = 0; i < val_string_array->size(); i++) {
				if (i > 0) result += ", ";
				result += (*val_string_array)[i];
			}
			return result;
		}
		}
		return "";
	}
};

// Row in the manager's variable list (for the manager data model)
struct VarListEntry {
	Rml::String name;
	Rml::String type_name;   // "string", "int", "float", "bool", "string[]"
	Rml::String value;       // display string
};

// ── DesignerApp ──────────────────────────────────────────────────────────

class DesignerApp {
public:
	DesignerApp();
	~DesignerApp();

	bool Init(const std::string& initial_file = "");
	void Shutdown();
	int Run(); // Message loop, returns exit code

	// Actions (called by manager UI or externally)
	void LoadDocument(const std::string& rml_path);
	void AddAssetFolder(const std::string& dir);
	void RemoveAssetFolder(const std::string& dir);
	void ReloadPreview();
	void ReloadFonts();

	// Variable management
	void AddVariable(const Rml::String& model_name, const Rml::String& name, VarType type);
	void RemoveVariable(const Rml::String& model_name, const Rml::String& name);
	void SetVariable(const Rml::String& model_name, const Rml::String& name, const Rml::String& value);
	void RebuildPreviewDataModels();
	void RefreshManagerVarList();

	// Save/load bind variable configurations
	bool SaveBindVars(const std::string& path);
	bool LoadBindVars(const std::string& path);

	// Accessors
	const std::string& GetDocumentPath() const { return document_path_; }
	const std::vector<std::string>& GetAssetFolders() const;
	Rml::Context* GetManagerContext() const { return manager_context_; }

private:
	static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	static LRESULT CALLBACK ManagerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

	bool CreatePreviewWindow();
	bool CreateManagerWindow();
	void UpdatePreview();
	void RenderPreview();
	void UpdateManager();
	void RenderManager();
	void OnFileChanged(const std::string& path);
	void LoadManagerUI();
	void RegisterFontsFromAssetFolders();
	void BrowseForDocument();
	void BrowseForAssetFolder();

	// Windows
	HWND preview_hwnd_ = nullptr;
	HWND manager_hwnd_ = nullptr;

	// DX12 renderers (one per window)
	std::unique_ptr<RenderInterface_DX12> preview_renderer_;
	std::unique_ptr<RenderInterface_DX12> manager_renderer_;

	// Shared interfaces
	std::unique_ptr<SystemInterface_Win32> system_interface_;
	std::unique_ptr<TextInputMethodEditor_Win32> text_input_editor_;
	FilesystemFileInterface file_interface_;

	// Slug font engine
	std::unique_ptr<SlugFontEngine> slug_font_engine_;

	// RmlUi contexts
	Rml::Context* preview_context_ = nullptr;
	Rml::Context* manager_context_ = nullptr;

	// File watcher
	FileWatcher file_watcher_;

	// State
	std::string document_path_;
	float dpi_scale_ = 1.0f;
	bool preview_minimized_ = false;
	bool manager_minimized_ = false;

	// Debounce: time of last file change, reload after 100ms
	DWORD last_change_tick_ = 0;
	bool reload_pending_ = false;
	bool font_reload_pending_ = false;
	bool mgr_var_dirty_ = false; // deferred manager variable list refresh

	// ── Data model variables ─────────────────────────────────────────────
	// Key: "model_name.var_name", maps to the actual variable storage.
	// Multiple data model names supported (e.g. "lobby", "serverlist").
	struct ModelVars {
		std::map<Rml::String, DesignerVar> vars; // stable iterators
		Rml::DataModelHandle handle;
	};
	std::map<Rml::String, ModelVars> data_models_;

	// Manager-side data model for the variable editor UI
	Rml::DataModelHandle mgr_var_model_;
	Rml::Vector<VarListEntry> mgr_var_list_;
	Rml::String mgr_new_model_name_;
	Rml::String mgr_new_var_name_;
	Rml::String mgr_new_var_type_; // "string", "int", "float", "bool", "string[]"
	Rml::String mgr_edit_value_;   // scratch for editing
	void SetupManagerDataModel();
};

} // namespace designer

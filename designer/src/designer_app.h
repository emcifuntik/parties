#pragma once

#include "file_watcher.h"

#include <parties/enum_names.h>

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class PartiesRenderInterface_DX12;
class SystemInterface_Win32;
class TextInputMethodEditor_Win32;

namespace parties::rml { class ElementRegistry; }

namespace designer {

void LoadEmbeddedFonts();

class FilesystemFileInterface : public Rml::FileInterface {
public:
	void AddSearchPath(const std::string& dir);
	void RemoveSearchPath(const std::string& dir);
	void ClearSearchPaths();
	const std::vector<std::string>& GetSearchPaths() const { return search_paths_; }

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

enum class VarType { String = 0, Int, Float, Bool, StringArray, List, Event };

}

namespace parties {

template <>
inline constexpr auto enum_names_v<designer::VarType> = EnumNames<designer::VarType, 7>{{{
	{designer::VarType::String, "string"},
	{designer::VarType::Int, "int"},
	{designer::VarType::Float, "float"},
	{designer::VarType::Bool, "bool"},
	{designer::VarType::StringArray, "string[]"},
	{designer::VarType::List, "list"},
	{designer::VarType::Event, "event"},
}}};

}

namespace designer {

inline Rml::String VarTypeName(VarType type) {
	return Rml::String(parties::enum_names_v<VarType>.name(type));
}

struct DesignerVar {
	VarType type;
	std::unique_ptr<Rml::String>               val_string;
	std::unique_ptr<int>                        val_int;
	std::unique_ptr<float>                      val_float;
	std::unique_ptr<bool>                       val_bool;
	std::unique_ptr<Rml::Vector<Rml::String>>   val_string_array;
	std::unique_ptr<rapidjson::Document>        val_list;

	DesignerVar() : type(VarType::String) {}
	explicit DesignerVar(VarType t) : type(t) { Allocate(); }

	void Allocate() {
		switch (type) {
		case VarType::String:      val_string = std::make_unique<Rml::String>(); break;
		case VarType::Int:         val_int = std::make_unique<int>(0); break;
		case VarType::Float:       val_float = std::make_unique<float>(0.0f); break;
		case VarType::Bool:        val_bool = std::make_unique<bool>(false); break;
		case VarType::StringArray: val_string_array = std::make_unique<Rml::Vector<Rml::String>>(); break;
		case VarType::List:        val_list = std::make_unique<rapidjson::Document>(rapidjson::kArrayType); break;
		case VarType::Event:       break;
		}
	}

	void SetFromString(const Rml::String& s) {
		switch (type) {
		case VarType::Event:       break;
		case VarType::String:      if (val_string) *val_string = s; break;
		case VarType::Int:         if (val_int) *val_int = std::atoi(s.c_str()); break;
		case VarType::Float:       if (val_float) *val_float = static_cast<float>(std::atof(s.c_str())); break;
		case VarType::Bool:        if (val_bool) *val_bool = (s == "true" || s == "1"); break;
		case VarType::StringArray: {
			if (!val_string_array) break;
			val_string_array->clear();
			size_t start = 0;
			while (start < s.size()) {
				size_t end = s.find(',', start);
				if (end == Rml::String::npos) end = s.size();
				auto item = s.substr(start, end - start);
				while (!item.empty() && item.front() == ' ') item.erase(item.begin());
				while (!item.empty() && item.back() == ' ') item.pop_back();
				if (!item.empty()) val_string_array->push_back(item);
				start = end + 1;
			}
			break;
		}
		case VarType::List: {
			if (!val_list) break;
			rapidjson::Document parsed;
			parsed.Parse(s.data(), s.size());
			if (!parsed.HasParseError() && parsed.IsArray()) val_list->Swap(parsed);
			break;
		}
		}
	}

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
		case VarType::List: {
			if (!val_list) return "[]";
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			val_list->Accept(writer);
			return Rml::String(buffer.GetString(), buffer.GetSize());
		}
		}
		return "";
	}
};

struct VarListEntry {
	Rml::String name;
	VarType type = VarType::String;
	Rml::String value;

	Rml::String TypeName() { return VarTypeName(type); }
};

class DesignerApp {
public:
	DesignerApp();
	~DesignerApp();

	bool Init(const std::string& initial_file = "");
	void Shutdown();
	int Run();
	int RunScreenshot(const std::string& output_path, int settle_frames = 240);
	void SetPreviewSize(int width, int height);
	void SetDensity(float density) { density_override_ = density; }
	void SetTheme(std::string theme) { preview_theme_ = std::move(theme); }
	void SetManagerVisible(bool visible) { manager_visible_ = visible; }
	void SetDebuggerEnabled(bool enabled) { debugger_enabled_ = enabled; }
	void SetAutoLoadBindVars(bool enabled) { auto_load_bind_vars_ = enabled; }
	void ConfigurePartiesFixture(std::string scenario);
	bool LastDocumentLoadSucceeded() const { return last_document_load_succeeded_; }

	void LoadDocument(const std::string& rml_path);
	void AddAssetFolder(const std::string& dir);
	void RemoveAssetFolder(const std::string& dir);
	void ReloadPreview();
	void ReloadFonts();

	void AddVariable(const Rml::String& model_name, const Rml::String& name, VarType type);
	void RemoveVariable(const Rml::String& model_name, const Rml::String& name);
	void SetVariable(const Rml::String& model_name, const Rml::String& name, const Rml::String& value);
	void RebuildPreviewDataModels();
	void RefreshManagerVarList();

	bool SaveBindVars(const std::string& path);
	bool LoadBindVars(const std::string& path);

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

	HWND preview_hwnd_ = nullptr;
	HWND manager_hwnd_ = nullptr;

	std::unique_ptr<PartiesRenderInterface_DX12> preview_renderer_;
	std::unique_ptr<PartiesRenderInterface_DX12> manager_renderer_;

	std::unique_ptr<SystemInterface_Win32> system_interface_;
	std::unique_ptr<TextInputMethodEditor_Win32> text_input_editor_;
	FilesystemFileInterface file_interface_;

	std::unique_ptr<parties::rml::ElementRegistry> element_registry_;
	struct PartiesFixture;
	std::unique_ptr<PartiesFixture> parties_fixture_;

	Rml::Context* preview_context_ = nullptr;
	Rml::Context* manager_context_ = nullptr;

	FileWatcher file_watcher_;

	std::string document_path_;
	float dpi_scale_ = 1.0f;
	float density_override_ = 0.0f;
	bool preview_minimized_ = false;
	bool manager_minimized_ = false;
	bool manager_visible_ = true;
	bool auto_load_bind_vars_ = true;
	bool last_document_load_succeeded_ = false;
	int rml_error_count_ = 0;
	int rml_data_binding_warning_count_ = 0;
	bool debugger_initialized_ = false;
	bool debugger_enabled_ = true;
	int preview_width_ = 1440;
	int preview_height_ = 900;
	std::string parties_fixture_scenario_;
	std::string preview_theme_;

	static constexpr DWORD reload_debounce_ms = 150;
	DWORD last_change_tick_ = 0;
	bool reload_pending_ = false;
	bool font_reload_pending_ = false;
	bool mgr_var_dirty_ = false;

	struct ModelVars {
		std::map<Rml::String, DesignerVar> vars;
		Rml::DataModelHandle handle;
	};
	std::map<Rml::String, ModelVars> data_models_;

	Rml::DataModelHandle mgr_var_model_;
	Rml::Vector<VarListEntry> mgr_var_list_;
	Rml::String mgr_new_model_name_;
	Rml::String mgr_new_var_name_;
	VarType mgr_new_var_type_ = VarType::String;
	Rml::String mgr_edit_value_;
	void SetupManagerDataModel();
};

}

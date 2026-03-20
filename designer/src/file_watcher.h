#pragma once

// File system watcher using ReadDirectoryChangesW (Windows).
// Watches directories recursively for .rml, .rcss, .ttf, .otf, .png, .jpg, .svg, .tga changes.
// Call poll() from the message loop — it invokes the callback on the caller's thread.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace designer {

class FileWatcher {
public:
	using Callback = std::function<void(const std::string& changed_path)>;

	FileWatcher();
	~FileWatcher();

	bool AddDirectory(const std::string& path);
	void RemoveDirectory(const std::string& path);
	void Clear();

	void SetCallback(Callback cb) { callback_ = std::move(cb); }

	// Non-blocking: checks for pending notifications, invokes callback.
	void Poll();

private:
	struct WatchedDir {
		std::string path;
		HANDLE dir_handle = INVALID_HANDLE_VALUE;
		OVERLAPPED overlapped{};
		alignas(DWORD) char buffer[4096]{};
		bool pending = false;
	};

	void IssueRead(WatchedDir& wd);
	static bool IsRelevantExtension(const std::wstring& filename);

	std::vector<std::unique_ptr<WatchedDir>> dirs_;
	Callback callback_;
};

} // namespace designer

#include "file_watcher.h"

#include <algorithm>
#include <cctype>

namespace designer {

FileWatcher::FileWatcher() = default;

FileWatcher::~FileWatcher() { Clear(); }

bool FileWatcher::AddDirectory(const std::string& path) {
	// Check not already watched
	for (auto& d : dirs_)
		if (d->path == path) return true;

	// Convert to wide string
	int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	std::wstring wpath(wlen - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

	HANDLE h = CreateFileW(wpath.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	auto wd = std::make_unique<WatchedDir>();
	wd->path = path;
	wd->dir_handle = h;
	wd->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

	IssueRead(*wd);
	dirs_.push_back(std::move(wd));
	return true;
}

void FileWatcher::RemoveDirectory(const std::string& path) {
	auto it = std::remove_if(dirs_.begin(), dirs_.end(), [&](const auto& d) {
		if (d->path == path) {
			CancelIo(d->dir_handle);
			CloseHandle(d->overlapped.hEvent);
			CloseHandle(d->dir_handle);
			return true;
		}
		return false;
	});
	dirs_.erase(it, dirs_.end());
}

void FileWatcher::Clear() {
	for (auto& d : dirs_) {
		CancelIo(d->dir_handle);
		CloseHandle(d->overlapped.hEvent);
		CloseHandle(d->dir_handle);
	}
	dirs_.clear();
}

void FileWatcher::IssueRead(WatchedDir& wd) {
	ResetEvent(wd.overlapped.hEvent);
	wd.pending = ReadDirectoryChangesW(
		wd.dir_handle,
		wd.buffer, sizeof(wd.buffer),
		TRUE, // watch subtree
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
		nullptr,
		&wd.overlapped,
		nullptr);
}

bool FileWatcher::IsRelevantExtension(const std::wstring& filename) {
	auto dot = filename.rfind(L'.');
	if (dot == std::wstring::npos) return false;
	std::wstring ext = filename.substr(dot);
	// Lowercase
	for (auto& c : ext) c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
	return ext == L".rml" || ext == L".rcss" || ext == L".ttf" || ext == L".otf" ||
	       ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".svg" || ext == L".tga";
}

void FileWatcher::Poll() {
	for (auto& d : dirs_) {
		if (!d->pending) {
			IssueRead(*d);
			continue;
		}

		DWORD bytes = 0;
		if (!GetOverlappedResult(d->dir_handle, &d->overlapped, &bytes, FALSE))
			continue; // not ready yet

		// Parse FILE_NOTIFY_INFORMATION chain
		if (bytes > 0 && callback_) {
			auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(d->buffer);
			for (;;) {
				std::wstring wname(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
				if (IsRelevantExtension(wname)) {
					// Convert to UTF-8
					std::string full = d->path + "\\";
					int u8len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0, nullptr, nullptr);
					std::string u8name(u8len - 1, '\0');
					WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, u8name.data(), u8len, nullptr, nullptr);
					full += u8name;
					callback_(full);
				}
				if (fni->NextEntryOffset == 0) break;
				fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
					reinterpret_cast<char*>(fni) + fni->NextEntryOffset);
			}
		}

		// Re-issue
		IssueRead(*d);
	}
}

} // namespace designer

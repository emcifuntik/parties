#include "designer_app.h"

#include "Parties_Renderer_DX12.h"
#include "RmlUi_Platform_Win32.h"
#include <client/chat_model.h>
#include <client/context_window_model.h>
#include <client/gradient_circle_element.h>
#include <client/level_meter_element.h>
#include <client/lobby_model.h>
#include <client/rml_elements.h>
#include <client/server_list_model.h>
#include <client/video_element.h>
#include <client/rml_binding.h>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Debugger.h>

#include <dwmapi.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

namespace designer {

namespace {

size_t CountElementTree(Rml::Element* element) {
	if (!element)
		return 0;
	size_t count = 1;
	const int child_count = element->GetNumChildren(true);
	for (int index = 0; index < child_count; ++index)
		count += CountElementTree(element->GetChild(index));
	return count;
}

size_t CountContextElements(Rml::Context* context) {
	if (!context)
		return 0;
	size_t count = 0;
	for (int index = 0; index < context->GetNumDocuments(); ++index)
		count += CountElementTree(context->GetDocument(index));
	return count;
}

bool ValidateScreenshotCoverage(const fs::path& path, double minimum_non_dominant_ratio) {
	std::ifstream stream(path, std::ios::binary);
	std::array<unsigned char, 54> header{};
	if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()) ||
		header[0] != 'B' || header[1] != 'M') return false;
	auto read_u16 = [&](size_t offset) {
		return static_cast<uint16_t>(static_cast<uint16_t>(header[offset]) |
			(static_cast<uint16_t>(header[offset + 1]) << 8));
	};
	auto read_u32 = [&](size_t offset) {
		return static_cast<uint32_t>(header[offset]) |
			(static_cast<uint32_t>(header[offset + 1]) << 8) |
			(static_cast<uint32_t>(header[offset + 2]) << 16) |
			(static_cast<uint32_t>(header[offset + 3]) << 24);
	};
	const uint32_t data_offset = read_u32(10);
	const int32_t width = static_cast<int32_t>(read_u32(18));
	const int32_t height = std::abs(static_cast<int32_t>(read_u32(22)));
	if (width <= 0 || height <= 0 || read_u16(28) != 24) return false;
	const size_t row_pitch = (static_cast<size_t>(width) * 3 + 3) & ~size_t(3);
	std::vector<unsigned char> row(row_pitch);
	std::array<uint32_t, 32768> histogram{};
	uint64_t total = 0;
	stream.seekg(data_offset);
	for (int y = 0; y < height; ++y) {
		if (!stream.read(reinterpret_cast<char*>(row.data()), row.size())) return false;
		for (int x = 0; x < width; ++x) {
			const unsigned char* bgr = row.data() + static_cast<size_t>(x) * 3;
			const uint32_t bucket = (static_cast<uint32_t>(bgr[2] >> 3) << 10) |
				(static_cast<uint32_t>(bgr[1] >> 3) << 5) | (bgr[0] >> 3);
			++histogram[bucket];
			++total;
		}
	}
	const uint32_t dominant = *std::max_element(histogram.begin(), histogram.end());
	const double non_dominant_ratio = total > 0 ? 1.0 - static_cast<double>(dominant) / total : 0.0;
	std::printf("[Designer] Screenshot non-dominant coverage: %.1f%%\n", non_dominant_ratio * 100.0);
	return non_dominant_ratio >= minimum_non_dominant_ratio;
}

class DesignerSystemInterface final : public SystemInterface_Win32 {
public:
	DesignerSystemInterface(int* error_count, int* data_binding_warning_count) :
		error_count_(error_count), data_binding_warning_count_(data_binding_warning_count) {}

	bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
		if (type == Rml::Log::LT_INFO || type == Rml::Log::LT_DEBUG)
			return true;
		const char* level = "info";
		if (type == Rml::Log::LT_ERROR) level = "error";
		else if (type == Rml::Log::LT_ASSERT) level = "assert";
		else if (type == Rml::Log::LT_WARNING) level = "warning";
		else if (type == Rml::Log::LT_DEBUG) level = "debug";
		std::fprintf(type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? stderr : stdout,
			"[RmlUI:%s] %s\n", level, message.c_str());
		if (error_count_ && (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT))
			++*error_count_;
		if (data_binding_warning_count_ && type == Rml::Log::LT_WARNING &&
			(message.find("Data array index out of bounds") != Rml::String::npos ||
			 message.find("Could not get value from data variable") != Rml::String::npos))
			++*data_binding_warning_count_;
		return true;
	}

private:
	int* error_count_ = nullptr;
	int* data_binding_warning_count_ = nullptr;
};

} // namespace

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

const std::vector<std::string>& DesignerApp::GetAssetFolders() const {
	return file_interface_.GetSearchPaths();
}

struct DesignerApp::PartiesFixture {
	parties::client::LobbyModel lobby;
	parties::client::ServerListModel server_list;
	parties::client::ChatModel chat;
	parties::client::ContextWindowModel context_window;
	int room_watch_request_count = 0;
	int room_watched_user_id = 0;

	static parties::client::ChannelUser User(const char* name, int id,
		bool speaking = false, bool muted = false, bool streaming = false, int role = 3) {
		parties::client::ChannelUser user;
		user.name = name;
		user.id = id;
		user.role = role;
		user.speaking = speaking;
		user.muted = muted;
		user.streaming = streaming;
		return user;
	}

	static parties::client::ChatMessage Message(int64_t id, int sender_id,
		const char* sender, const char* initials, const char* text, const char* time,
		int color, bool own = false) {
		parties::client::ChatMessage message;
		message.id = id;
		message.sender_id = sender_id;
		message.sender_name = sender;
		message.initials = initials;
		message.text = text;
		message.timestamp_str = time;
		message.color_index = color;
		message.is_own = own;
		message.segments.push_back({text, false});
		return message;
	}

	static parties::client::ChatMessage SegmentedMessage(int64_t id, int segment_count) {
		auto message = Message(id, static_cast<int>(id % 7) + 1, "LinkBot", "LB", "", "20:46", 4);
		message.text.clear();
		message.segments.clear();
		for (int i = 0; i < segment_count; ++i) {
			const bool is_url = (i % 2) != 0;
			Rml::String text = is_url ? "https://example.com/" : "segment-";
			text += std::to_string(i);
			text += " ";
			message.text += text;
			message.segments.push_back({std::move(text), is_url});
			message.has_url |= is_url;
		}
		return message;
	}

	void ExerciseChatSegmentChurn(int frame) {
		auto& messages = chat.messages.silent();
		if (frame == 1 && messages.size() > 47) {
			messages[43].segments.resize(3);
			messages[47].segments.resize(2);
			chat.messages.notify();
		}
		else if (frame == 2 && messages.size() > 20) {
			messages.erase(messages.begin(), messages.begin() + 18);
			chat.messages.notify();
		}
	}

	void Populate(const std::string& scenario) {
		if (scenario == "context-user") {
			context_window.user_mode = true;
			context_window.title = "IceTroll";
			context_window.subtitle = "In voice · General";
			context_window.icon_text = "IT";
			context_window.role_label = "Administrator";
			context_window.volume = 0.86f;
			context_window.volume_text = "86%";
			context_window.music_volume = 0.64f;
			context_window.music_volume_text = "64%";
			context_window.compression = true;
			context_window.compression_target = 0.68f;
			context_window.actions = Rml::Vector<parties::client::ContextWindowAction>{
				{"Make moderator", "Manage conversations", 102, false, false},
				{"Make member", "Standard permissions", 103, false, false},
				{"", "", 0, false, true},
				{"Remove from party", "Disconnect this member", 200, true, false}};
			context_window.has_actions = true;
			return;
		}
		if (scenario == "context-channel") {
			context_window.user_mode = false;
			context_window.title = "General";
			context_window.subtitle = "Voice channel";
			context_window.room_icon = true;
			context_window.actions = Rml::Vector<parties::client::ContextWindowAction>{
				{"Join channel", "Move to General", 1, false, false},
				{"Edit channel", "Name and permissions", 2, false, false},
				{"", "", 0, false, true},
				{"Delete channel", "This cannot be undone", 3, true, false}};
			context_window.has_actions = true;
			return;
		}

		lobby.is_connected = scenario != "launcher" && scenario != "party-modal" &&
			scenario != "onboarding" && scenario != "recovery";
		lobby.server_name = "Night Shift";
		lobby.server_initials = "NS";
		lobby.username = "tuxick";
		lobby.current_channel = 1;
		lobby.current_channel_name = "General";
		lobby.ping_ms = 24;
		lobby.voice_volume = 1.0f;
		lobby.secondary_volume = 0.8f;
		lobby.music_send_volume = 0.72f;
		lobby.notification_volume = 0.75f;
		lobby.denoise_enabled = true;
		lobby.normalize_enabled = false;
		lobby.normalize_target = 0.68f;
		lobby.vad_enabled = true;
		lobby.vad_threshold = 0.12f;
		lobby.voice_level = 0.0f;
		lobby.share_codec = 0;
		lobby.share_scale = 0;
		lobby.share_fps = 2;
		lobby.share_bitrate = 8.0f;
		lobby.share_preset = -1;
		lobby.selected_share_target = 0;
		lobby.can_manage_channels = true;
		lobby.my_role = 0;

		Rml::Vector<parties::client::ChannelInfo> channels;
		parties::client::ChannelInfo general;
		general.id = 1;
		general.name = "General";
		general.max_users = 64;
		general.users = {
			User("tuxick", 1, false, false, false, 0),
			User("IceTroll", 2, true),
			User("ivan", 3, false, false, true),
			User("Sara", 4),
			User("Maks", 5, false, true),
			User("android", 6),
			User("Noah", 7)
		};
		general.user_count = static_cast<int>(general.users.size());
		channels.push_back(general);
		parties::client::ChannelInfo lounge;
		lounge.id = 2;
		lounge.name = "Late night";
		lounge.max_users = 12;
		lounge.users = {User("Maya", 8), User("Liam", 9, true)};
		lounge.user_count = static_cast<int>(lounge.users.size());
		channels.push_back(lounge);
		parties::client::ChannelInfo focus;
		focus.id = 3;
		focus.name = "Quiet focus";
		focus.max_users = 8;
		channels.push_back(focus);
		lobby.channels = std::move(channels);

		lobby.capture_devices = Rml::Vector<parties::client::AudioDevice>{
			{"Microphone (fifine SC3)", 0}, {"Headset Microphone", 1}};
		lobby.playback_devices = Rml::Vector<parties::client::AudioDevice>{
			{"Speakers (fifine SC3)", 0}, {"Headphones", 1}};
		lobby.mute_key = 123;
		lobby.mute_key_name = "F12";

		Rml::Vector<parties::client::ServerEntry> servers;
		parties::client::ServerEntry night;
		night.id = 1; night.name = "Night Shift"; night.initials = "NS"; night.color_index = 1;
		night.host = "night.parties.local"; night.online = true;
		night.users_text = "12 online · 5 in General";
		servers.push_back(night);
		parties::client::ServerEntry studio;
		studio.id = 2; studio.name = "Creative Studio"; studio.initials = "CS"; studio.color_index = 2;
		studio.host = "studio.parties.local"; studio.online = true;
		studio.users_text = "7 online · 3 in Lounge";
		servers.push_back(studio);
		parties::client::ServerEntry guild;
		guild.id = 3; guild.name = "Old Guild"; guild.initials = "OG"; guild.color_index = 3;
		guild.host = "guild.parties.local"; guild.online = false; guild.locked = true;
		servers.push_back(guild);
		server_list.servers = std::move(servers);
		server_list.party_count_text = "3 parties · 19 friends online";
		server_list.connected_server_id = lobby.is_connected.get() ? 1 : 0;
		server_list.fingerprint = "5E7A 91C2 4D3F";
		server_list.has_identity = true;
		server_list.seed_phrase = "amber vessel orbit meadow copper velvet island echo marble lunar gentle harbor";

		chat.text_channels = Rml::Vector<parties::client::TextChannel>{
			{1, "general", false}, {2, "clips-and-links", true}, {3, "off-topic", false}};
		// Keep chat selected in the settings fixture to reproduce the historical
		// overlap bug. The document router must still render settings exclusively.
		chat.active_channel = (scenario == "chat" || scenario == "chat-segment-churn" ||
			scenario == "settings") ? 1 : 0;
		chat.active_channel_name = "general";
		chat.can_manage_channels = true;
		if (scenario == "chat-segment-churn") {
			Rml::Vector<parties::client::ChatMessage> messages;
			messages.reserve(50);
			for (int i = 0; i < 50; ++i)
				messages.push_back(SegmentedMessage(i + 1, i == 43 ? 13 : (i == 47 ? 11 : 4)));
			chat.messages = std::move(messages);
		}
		else {
			chat.messages = Rml::Vector<parties::client::ChatMessage>{
				Message(1, 2, "IceTroll", "IT", "Anyone up for a quick match later?", "20:41", 2),
				Message(2, 3, "ivan", "IV", "I can join after I finish this build.", "20:43", 5),
				Message(3, 4, "Sara", "SA", "Perfect. I pinned the server details above.", "20:44", 8),
				Message(4, 1, "tuxick", "TU", "Give me ten minutes and I'll be there.", "20:45", 3, true)};
		}

		lobby.router.reset();
		const bool settings_scenario = scenario == "settings" || scenario == "settings-select-open" ||
			scenario == "settings-screen-share" || scenario == "settings-hotkeys" ||
			scenario == "settings-account";
		if (settings_scenario)
			lobby.router.go(parties::client::DocumentRoute::Settings);
		else if (scenario == "chat" || scenario == "chat-segment-churn")
			lobby.router.go(parties::client::DocumentRoute::Chat);
		if (scenario == "settings-screen-share")
			lobby.router.select_settings(parties::client::SettingsSection::ScreenShare);
		else if (scenario == "settings-hotkeys")
			lobby.router.select_settings(parties::client::SettingsSection::Hotkeys);
		else if (scenario == "settings-account") {
			lobby.router.select_settings(parties::client::SettingsSection::AccountKeys);
			lobby.show_private_key = true;
			lobby.identity_private_key = "8d99c2eed598508a94eb471d7334ee80d28a57139d1a7b7273e16105106fe0a4";
			lobby.show_import_identity = true;
		}
		server_list.show_onboarding = scenario == "onboarding" || scenario == "recovery";
		server_list.onboarding_step = scenario == "recovery" ? 1 : 0;
		if (server_list.show_onboarding.get())
			server_list.has_identity = false;
		server_list.show_add_form = scenario == "party-modal";
		server_list.global_name = "tuxick";
		server_list.edit_host = "voice.example.com";
		server_list.edit_port = "7800";
		server_list.edit_nickname = "";
		if (scenario == "room") {
			lobby.someone_sharing = true;
			lobby.sharers = Rml::Vector<parties::client::ActiveSharer>{
				{3, "ivan", false}};
			lobby.on_watch_sharer = [this](int user_id) {
				++room_watch_request_count;
				room_watched_user_id = user_id;
			};
		}

		if (scenario == "share" || scenario == "audio-share") {
			lobby.router.go(parties::client::DocumentRoute::SharePicker);
			lobby.share_picker_mode = scenario == "audio-share" ? 1 : 0;
			lobby.selected_share_target = scenario == "audio-share" ? 0 : -1;
			lobby.share_preset = 1;
			lobby.share_scale = 1;
			lobby.share_bitrate = 6.0f;
			if (scenario == "audio-share") {
				lobby.share_application_targets = Rml::Vector<parties::client::ShareTarget>{
					{"Spotify — Karaoke Mix", 0, false},
					{"Zen Browser — YouTube Music", 1, false},
					{"VLC media player — Backing track", 2, false},
					{"foobar2000 — Playlist", 3, false}};
			} else {
				lobby.share_monitor_targets = Rml::Vector<parties::client::ShareTarget>{
					{"Display 1 — 2560 × 1440", 0, true},
					{"Display 2 — 1920 × 1080", 1, true},
					{"Display 3 — 1920 × 1080", 2, true}};
				lobby.share_application_targets = Rml::Vector<parties::client::ShareTarget>{
					{"Zen Browser — Parties redesign", 3, false},
					{"Visual Studio — miniaudio-rnnoise", 4, false},
					{"OBS Studio — Preview", 5, false},
					{"ChatGPT", 6, false},
					{"Docker Desktop — Containers", 7, false},
					{"File Explorer — providers", 8, false},
					{"Command Prompt", 9, false},
					{"Rockstar Games Launcher", 10, false}};
			}
			for (auto* targets : {&lobby.share_monitor_targets, &lobby.share_application_targets}) {
				for (auto& target : targets->silent())
					target.element_id = "share-thumbnail-" + Rml::ToString(target.index);
			}
		}

		lobby.show_user_menu = scenario == "member";
		if (scenario == "member") {
			lobby.menu_user_id = 2;
			lobby.menu_user_name = "IceTroll";
			lobby.menu_user_role = 1;
			lobby.menu_user_volume = 0.86f;
			lobby.menu_user_music_volume = 0.64f;
			lobby.menu_user_compress = true;
			lobby.menu_can_roles = true;
			lobby.menu_can_kick = true;
		}

		if (scenario == "stream-single" || scenario == "streams" ||
			scenario == "stream-fps-overflow") {
			lobby.router.go(parties::client::DocumentRoute::Streams);
			lobby.someone_sharing = true;
			const bool single_stream = scenario == "stream-single" || scenario == "stream-fps-overflow";
			lobby.watching_count = single_stream ? 1 : 2;
			lobby.viewing_sharer_id = 2;
			lobby.stream_fps = scenario == "stream-fps-overflow" ? 1001 : 60;
			lobby.sharers = Rml::Vector<parties::client::ActiveSharer>{
				{2, "IceTroll", true}, {3, "ivan", true}, {4, "Sara", false}};
			lobby.watched = Rml::Vector<parties::client::WatchedStream>{
				{2, "IceTroll · Counter-Strike 2", "screen-share-2"}};
			if (!single_stream)
				lobby.watched.silent().push_back({3, "ivan · Zen Browser", "screen-share-3"});
		}
	}

	bool Init(Rml::Context* context, const std::string& scenario) {
		if (scenario != "launcher" && scenario != "party-modal" && scenario != "onboarding" &&
			scenario != "recovery" && scenario != "room" && scenario != "settings" &&
			scenario != "settings-select-open" && scenario != "settings-screen-share" &&
			scenario != "settings-hotkeys" &&
			scenario != "settings-account" &&
			scenario != "chat" && scenario != "chat-segment-churn" &&
			scenario != "stream-single" && scenario != "streams" &&
			scenario != "stream-fps-overflow" &&
			scenario != "member" && scenario != "share" && scenario != "audio-share" &&
			scenario != "context-user" && scenario != "context-channel")
			return false;
		Populate(scenario);
		if (scenario == "context-user" || scenario == "context-channel")
			return context_window.init(context);
		return lobby.init(context) && server_list.init(context) && chat.init(context);
	}

	static std::vector<uint8_t> MockStreamFrame(int variant) {
		constexpr int width = 960;
		constexpr int height = 540;
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
		auto fill = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
			const int right = (std::min)(width, x + w);
			const int bottom = (std::min)(height, y + h);
			for (int py = (std::max)(0, y); py < bottom; ++py) {
				for (int px = (std::max)(0, x); px < right; ++px) {
					auto offset = (static_cast<size_t>(py) * width + px) * 4;
					pixels[offset + 0] = r;
					pixels[offset + 1] = g;
					pixels[offset + 2] = b;
					pixels[offset + 3] = 255;
				}
			}
		};

		fill(0, 0, width, height, 10, 14, 20);
		fill(0, 0, width, 44, 23, 29, 39);
		fill(16, 14, 132, 16, variant == 0 ? 94 : 167, variant == 0 ? 234 : 139,
			variant == 0 ? 212 : 250);
		if (variant == 0) {
			fill(18, 64, 178, 456, 17, 23, 32);
			fill(216, 64, 726, 284, 23, 31, 42);
			fill(238, 86, 210, 240, 32, 44, 58);
			fill(470, 86, 210, 240, 24, 59, 55);
			fill(702, 86, 218, 240, 46, 37, 65);
			fill(216, 368, 350, 152, 15, 21, 29);
			fill(584, 368, 358, 152, 18, 25, 34);
			for (int y = 84; y < 486; y += 38)
				fill(36, y, 126 - (y % 4) * 12, 9, 54, 68, 84);
		} else {
			fill(18, 64, 138, 456, 21, 25, 34);
			fill(176, 64, 766, 456, 238, 241, 244);
			fill(176, 64, 766, 48, 213, 221, 229);
			fill(208, 142, 270, 24, 49, 62, 75);
			fill(208, 188, 680, 12, 179, 189, 199);
			fill(208, 220, 618, 12, 179, 189, 199);
			fill(208, 252, 654, 12, 179, 189, 199);
			fill(208, 316, 198, 126, 94, 234, 212);
			fill(430, 316, 198, 126, 167, 139, 250);
			fill(652, 316, 198, 126, 96, 165, 250);
		}
		return pixels;
	}

	static Rml::Element* FindSliderPart(Rml::Element* slider, const Rml::String& tag) {
		if (!slider) return nullptr;
		for (int i = 0; i < slider->GetNumChildren(true); ++i) {
			Rml::Element* child = slider->GetChild(i);
			if (!child) continue;
			if (child->GetTagName() == tag) return child;
			for (int j = 0; j < child->GetNumChildren(true); ++j) {
				Rml::Element* grandchild = child->GetChild(j);
				if (grandchild && grandchild->GetTagName() == tag) return grandchild;
			}
		}
		return nullptr;
	}

	void ApplyDocumentFixture(Rml::ElementDocument* document, const std::string& scenario) {
		if (!document) return;
		if (scenario == "room") {
			document->GetContext()->Update();
			Rml::ElementList cards;
			document->GetElementsByClassName(cards, "voice-stage-card");
			Rml::Element* ordinary_card = nullptr;
			Rml::Element* streaming_card = nullptr;
			for (Rml::Element* card : cards) {
				if (!card->IsVisible(true)) continue;
				if (card->IsClassSet("streaming")) streaming_card = card;
				else if (!ordinary_card) ordinary_card = card;
			}
			// A normal member card must be inert, while the streaming member card
			// must issue exactly one watch request through the real data event.
			if (ordinary_card) ordinary_card->DispatchEvent("click", Rml::Dictionary{});
			if (streaming_card) streaming_card->DispatchEvent("click", Rml::Dictionary{});
		}
		if (scenario == "settings" || scenario == "settings-select-open") {
			// Exercise the data-bound custom element after document creation. This
			// reproduces the real route transition instead of mutating its C++ object.
			lobby.voice_level = 0.48f;
			lobby.vad_threshold = 0.36f;
			lobby.ptt_enabled = true;
			lobby.ptt_delay = 250.0f;
			lobby.ptt_delay_text = "250 ms";
			if (scenario == "settings-select-open") {
				document->GetContext()->Update();
				if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(
					document->GetElementById("settings-capture-select")))
					select->ShowSelectBox();
			}
		}
		if (scenario == "settings-screen-share") {
			document->GetContext()->Update();
			if (Rml::Element* bar = FindSliderPart(
				document->GetElementById("settings-bitrate-slider"), "sliderbar"))
				bar->SetPseudoClass("hover", true);
		}
		if (scenario == "stream-single" || scenario == "streams" ||
			scenario == "stream-fps-overflow") {
			const int frame_count = scenario == "stream-single" || scenario == "stream-fps-overflow" ? 1 : 2;
			for (int index = 0; index < frame_count; ++index) {
				const Rml::String id = index == 0 ? "screen-share-2" : "screen-share-3";
				if (auto* element = document->GetElementById(id)) {
					if (auto* video = rmlui_dynamic_cast<parties::client::VideoElement*>(element))
						video->UpdateFrame(MockStreamFrame(index), 960, 540);
				}
			}
		}
		if (scenario == "share" || scenario == "audio-share") {
			Rml::ElementList videos;
			document->GetElementsByTagName(videos, "video_frame");
			for (auto* element : videos) {
				const int target_index = element->GetAttribute<int>("thumbnailindex", -1);
				if (target_index < 0) continue;
				if (auto* video = rmlui_dynamic_cast<parties::client::VideoElement*>(element))
					video->UpdateFrame(MockStreamFrame(target_index % 2), 960, 540);
			}
		}
	}

	bool ValidateDocumentFixture(Rml::ElementDocument* document, const std::string& scenario,
		int viewport_width) const {
		if (!document)
			return false;

		if (scenario == "context-user" || scenario == "context-channel") {
			Rml::Element* shell = document->GetElementById("context-window-shell");
			Rml::Element* audio = document->GetElementById("context-window-audio");
			Rml::Element* actions = document->GetElementById("context-window-actions");
			Rml::Element* music_volume = document->GetElementById("context-window-music-volume");
			const bool user = scenario == "context-user";
			const float shell_height = shell
				? shell->GetBox().GetSize(Rml::BoxArea::Border).y : 0.0f;
			const float viewport_height = static_cast<float>(document->GetContext()->GetDimensions().y);
			const bool valid = shell && actions && actions->GetNumChildren(true) >= 3 &&
				shell_height > 0.0f && shell_height < viewport_height - 1.0f &&
				(user ? audio && audio->IsVisible(true) && music_volume && music_volume->IsVisible(true)
				      : (!audio || !audio->IsVisible(true)) && (!music_volume || !music_volume->IsVisible(true)));
			if (!valid) {
				std::fprintf(stderr, "[Designer] Context window fixture failed for %s\n", scenario.c_str());
			}
			return valid;
		}

		// Every connected top-level route uses the channel sidebar. Settings owns
		// its dedicated navigation, while disconnected flows have no sidebar. The
		// legacy server rail must not exist in any document fixture.
		const bool expects_channel_sidebar = scenario == "room" || scenario == "chat" ||
			scenario == "chat-segment-churn" ||
			scenario == "member" || scenario == "share" || scenario == "audio-share" ||
			scenario == "stream-single" || scenario == "streams" ||
			scenario == "stream-fps-overflow";
		Rml::Element* channel_sidebar = document->GetElementById("channel-sidebar");
		Rml::ElementList legacy_party_rails;
		document->GetElementsByClassName(legacy_party_rails, "stream-rail");
		const bool sidebar_visible = channel_sidebar && channel_sidebar->IsVisible(true);
		const bool shell_valid = legacy_party_rails.empty() &&
			(expects_channel_sidebar ? sidebar_visible : !sidebar_visible);
		if (!shell_valid) {
			std::fprintf(stderr,
				"[Designer] Shared sidebar invariant failed for %s: expected=%d visible=%d legacy-rails=%zu\n",
				scenario.c_str(), expects_channel_sidebar ? 1 : 0,
				sidebar_visible ? 1 : 0, legacy_party_rails.size());
			return false;
		}

		if (scenario == "launcher") {
			Rml::ElementList icons;
			document->GetElementsByClassName(icons, "party-card-icon-wrap");
			bool valid = icons.size() == 3;
			for (Rml::Element* icon : icons) {
				if (!icon || !icon->IsVisible(true)) {
					valid = false;
					continue;
				}
				const Rml::Box& box = icon->GetBox();
				valid = valid &&
					std::fabs(box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top)) < 0.1f &&
					std::fabs(box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right)) < 0.1f &&
					std::fabs(box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Bottom)) < 0.1f &&
					std::fabs(box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Left)) < 0.1f;
			}
			if (!valid)
				std::fprintf(stderr, "[Designer] Launcher server icon border validation failed\n");
			return valid;
		}

		if (scenario == "audio-share") {
			Rml::Element* quality = document->GetElementById("share-quality-panel");
			Rml::Element* screens = document->GetElementById("share-screens-section");
			Rml::Element* applications = document->GetElementById("share-applications-section");
			Rml::Element* start_label = document->GetElementById("share-audio-start-label");
			const bool valid = !quality && screens && !screens->IsVisible(true) &&
				applications && applications->IsVisible(true) && start_label && start_label->IsVisible(true);
			if (!valid)
				std::fprintf(stderr, "[Designer] Application audio picker layout validation failed\n");
			return valid;
		}

		if (scenario == "share") {
			Rml::Element* quality = document->GetElementById("share-quality-panel");
			Rml::Element* screens = document->GetElementById("share-screens-section");
			Rml::Element* applications = document->GetElementById("share-applications-section");
			const bool valid = !quality && screens && screens->IsVisible(true) &&
				applications && applications->IsVisible(true);
			if (!valid)
				std::fprintf(stderr, "[Designer] Screen share picker layout validation failed\n");
			return valid;
		}

		if (scenario == "room") {
			Rml::Element* audio_share = document->GetElementById("room-audio-share-button");
			Rml::ElementList cards;
			Rml::ElementList patterns;
			Rml::ElementList badges;
			Rml::ElementList stream_states;
			Rml::ElementList streaming_states;
			Rml::ElementList pattern_rows;
			Rml::ElementList capacities;
			Rml::ElementList legacy_sharer_cards;
			document->GetElementsByClassName(cards, "voice-stage-card");
			document->GetElementsByClassName(patterns, "voice-stage-camera-pattern");
			document->GetElementsByClassName(badges, "voice-stage-stream-badge");
			document->GetElementsByClassName(stream_states, "stream-state");
			document->GetElementsByClassName(streaming_states, "voice-stage-streaming-state");
			document->GetElementsByClassName(pattern_rows, "voice-stage-camera-row");
			document->GetElementsByClassName(capacities, "voice-stage-capacity");
			document->GetElementsByClassName(legacy_sharer_cards, "sharer-card");
			auto remove_hidden = [](Rml::ElementList& elements) {
				elements.erase(std::remove_if(elements.begin(), elements.end(),
					[](Rml::Element* element) { return !element->IsVisible(true); }), elements.end());
			};
			remove_hidden(cards);
			remove_hidden(patterns);
			remove_hidden(badges);
			remove_hidden(stream_states);
			remove_hidden(streaming_states);
			remove_hidden(pattern_rows);
			remove_hidden(capacities);
			remove_hidden(legacy_sharer_cards);
			bool packed_grid = cards.size() == 7;
			if (packed_grid) {
				const auto first = cards[0]->GetAbsoluteOffset(Rml::BoxArea::Border);
				const auto fourth = cards[3]->GetAbsoluteOffset(Rml::BoxArea::Border);
				const auto seventh = cards[6]->GetAbsoluteOffset(Rml::BoxArea::Border);
				const float card_height = cards[0]->GetBox().GetSize(Rml::BoxArea::Border).y;
				const float expected_row_step = card_height + 12.0f;
				packed_grid = std::fabs(first.x - fourth.x) < 2.0f &&
					std::fabs(first.x - seventh.x) < 2.0f &&
					std::fabs((fourth.y - first.y) - expected_row_step) < 2.0f &&
					std::fabs((seventh.y - fourth.y) - expected_row_step) < 2.0f;
			}
			const bool valid = audio_share && audio_share->IsVisible(true) && packed_grid &&
				patterns.size() == 1 && pattern_rows.size() == 6 && badges.empty() &&
				stream_states.empty() && streaming_states.size() == 1 && capacities.empty() &&
				legacy_sharer_cards.empty() && room_watch_request_count == 1 &&
				room_watched_user_id == 3;
			if (!valid)
				std::fprintf(stderr,
					"[Designer] Voice room validation failed: cards=%zu patterns=%zu rows=%zu badges=%zu old-states=%zu streaming-states=%zu capacity=%zu legacy=%zu packed=%d watch-count=%d watched=%d\n",
					cards.size(), patterns.size(), pattern_rows.size(), badges.size(), stream_states.size(), streaming_states.size(), capacities.size(),
					legacy_sharer_cards.size(), packed_grid ? 1 : 0,
					room_watch_request_count, room_watched_user_id);
			return valid;
		}

		if (scenario == "settings" || scenario == "settings-select-open" ||
			scenario == "settings-screen-share" || scenario == "settings-hotkeys" ||
			scenario == "settings-account" ||
			scenario == "chat" || scenario == "chat-segment-churn") {
			Rml::Element* settings_route = document->GetElementById("route-settings");
			Rml::Element* chat_route = document->GetElementById("route-chat");
			const bool settings_present = settings_route && settings_route->IsVisible(true);
			const bool chat_present = chat_route && chat_route->IsVisible(true);
			const bool settings_scenario = scenario != "chat" && scenario != "chat-segment-churn";
			const bool audio_settings_scenario = scenario == "settings" || scenario == "settings-select-open";
			const bool route_valid = settings_scenario
				? settings_present && !chat_present
				: chat_present && !settings_present;
			bool valid = route_valid;
			if (valid && settings_scenario) {
				Rml::Element* settings_nav = document->GetElementById("settings-nav");
				Rml::ElementList nav_items;
				document->GetElementsByClassName(nav_items, "settings-nav-item");
				size_t visible_nav_items = 0;
				for (Rml::Element* item : nav_items)
					visible_nav_items += item && item->IsVisible(true) ? 1u : 0u;
				const float expected_nav_width = viewport_width <= 1100 ? 250.0f : 286.0f;
				const float nav_width = settings_nav
					? settings_nav->GetBox().GetSize(Rml::BoxArea::Border).x : 0.0f;
				valid = settings_nav && visible_nav_items == 4 &&
					document->GetElementById("settings-nav-audio") &&
					document->GetElementById("settings-nav-screen-share") &&
					document->GetElementById("settings-nav-hotkeys") &&
					document->GetElementById("settings-nav-account") &&
					std::fabs(nav_width - expected_nav_width) < 1.0f;
				if (!valid)
					std::fprintf(stderr,
						"[Designer] Settings navigation failed: items=%zu width=%.1f expected=%.1f\n",
						visible_nav_items, nav_width, expected_nav_width);
				if (valid) {
					auto visible_typography_matches = [document](const char* class_name, float size,
						Rml::Style::FontWeight weight) {
						Rml::ElementList elements;
						document->GetElementsByClassName(elements, class_name);
						for (Rml::Element* element : elements) {
							if (!element || !element->IsVisible(true))
								continue;
							const auto& computed = element->GetComputedValues();
							if (std::fabs(computed.font_size() - size) >= 0.1f ||
								computed.font_weight() != weight)
								return false;
						}
						return true;
					};
					Rml::ElementList page_titles;
					document->GetElementsByTagName(page_titles, "h2");
					size_t visible_page_titles = 0;
					for (Rml::Element* title : page_titles) {
						if (!title || !title->IsVisible(true))
							continue;
						++visible_page_titles;
						const auto& computed = title->GetComputedValues();
						valid = valid && std::fabs(computed.font_size() - 20.0f) < 0.1f &&
							computed.font_weight() == Rml::Style::FontWeight::Bold;
					}
					valid = valid && visible_page_titles == 1 &&
						visible_typography_matches("settings-nav-item", 13.0f, Rml::Style::FontWeight::Normal) &&
						visible_typography_matches("settings-field-label", 10.0f, Rml::Style::FontWeight::Bold) &&
						visible_typography_matches("settings-feature-title", 13.0f, Rml::Style::FontWeight::Bold) &&
						visible_typography_matches("settings-feature-copy", 10.0f, Rml::Style::FontWeight::Normal) &&
						visible_typography_matches("settings-page-intro", 12.0f, Rml::Style::FontWeight::Normal) &&
						visible_typography_matches("settings-keycap", 12.0f, Rml::Style::FontWeight::Bold) &&
						visible_typography_matches("settings-secondary-action", 12.0f, Rml::Style::FontWeight::Bold) &&
						visible_typography_matches("settings-danger-action", 12.0f, Rml::Style::FontWeight::Bold) &&
						visible_typography_matches("share-codec-btn", 12.0f, Rml::Style::FontWeight::Bold);
					if (!valid)
						std::fprintf(stderr, "[Designer] Settings typography scale validation failed for %s\n", scenario.c_str());
				}
			}
			if (valid && audio_settings_scenario) {
				auto* mic_meter = rmlui_dynamic_cast<parties::client::LevelMeterElement*>(
					document->GetElementById("voice-level-meter"));
				auto* activation_meter = rmlui_dynamic_cast<parties::client::LevelMeterElement*>(
					document->GetElementById("voice-activation-meter"));
				Rml::Element* switch_track = document->GetElementById("settings-denoise-switch");
				Rml::Element* switch_knob = document->GetElementById("settings-denoise-switch-knob");
				Rml::Element* vad_switch_track = document->GetElementById("settings-vad-switch");
				Rml::Element* vad_switch_knob = document->GetElementById("settings-vad-switch-knob");
				Rml::Element* noise_card = document->GetElementById("settings-noise-card");
				Rml::Element* normalize_card = document->GetElementById("settings-normalize-card");
				Rml::Element* notification_card = document->GetElementById("settings-notification-card");
				Rml::Element* music_card = document->GetElementById("settings-music-send-card");
				Rml::Element* music_slider = document->GetElementById("settings-music-send-slider");
				Rml::Element* vad_card = document->GetElementById("settings-vad-card");
				Rml::Element* ptt_card = document->GetElementById("settings-ptt-card");
				Rml::Element* ptt_delay_slider = document->GetElementById("settings-ptt-delay-slider");
				Rml::Element* ptt_delay_value = document->GetElementById("settings-ptt-delay-value");
				const float track_center = switch_track
					? switch_track->GetAbsoluteOffset(Rml::BoxArea::Border).y +
						switch_track->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f : 0.0f;
				const float knob_center = switch_knob
					? switch_knob->GetAbsoluteOffset(Rml::BoxArea::Border).y +
						switch_knob->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f : -10.0f;
				const float vad_switch_center = vad_switch_track
					? vad_switch_track->GetAbsoluteOffset(Rml::BoxArea::Border).y +
						vad_switch_track->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f : 0.0f;
				const float vad_knob_center = vad_switch_knob
					? vad_switch_knob->GetAbsoluteOffset(Rml::BoxArea::Border).y +
						vad_switch_knob->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f : -10.0f;
				auto card_x = [](Rml::Element* element) {
					return element ? element->GetAbsoluteOffset(Rml::BoxArea::Border).x : -1.0f;
				};
				auto card_y = [](Rml::Element* element) {
					return element ? element->GetAbsoluteOffset(Rml::BoxArea::Border).y : -1.0f;
				};
				auto card_width = [](Rml::Element* element) {
					return element ? element->GetBox().GetSize(Rml::BoxArea::Border).x : -1.0f;
				};
				auto card_height = [](Rml::Element* element) {
					return element ? element->GetBox().GetSize(Rml::BoxArea::Border).y : -1.0f;
				};
				const bool ptt_delay_inside_card = ptt_card && ptt_delay_slider && ptt_delay_value &&
					card_x(ptt_delay_slider) >= card_x(ptt_card) &&
					card_x(ptt_delay_slider) - card_x(ptt_card) <= 20.0f &&
					card_x(ptt_delay_slider) + card_width(ptt_delay_slider) <=
						card_x(ptt_delay_value) &&
					card_x(ptt_delay_value) + card_width(ptt_delay_value) <=
						card_x(ptt_card) + card_width(ptt_card) &&
					card_y(ptt_delay_slider) >= card_y(ptt_card) &&
					card_y(ptt_delay_slider) + card_height(ptt_delay_slider) <=
						card_y(ptt_card) + card_height(ptt_card);
				const bool card_grid_valid = noise_card && normalize_card && notification_card &&
					music_card && music_slider && vad_card &&
					ptt_delay_inside_card &&
					std::fabs(card_width(noise_card) - card_width(normalize_card)) < 1.0f &&
					std::fabs(card_width(notification_card) - card_width(music_card)) < 1.0f &&
					std::fabs(card_width(notification_card) - card_width(vad_card)) < 1.0f &&
					card_x(notification_card) < card_x(music_card) &&
					card_x(music_card) < card_x(vad_card) &&
					std::fabs(card_y(notification_card) - card_y(music_card)) < 1.0f &&
					std::fabs(card_y(notification_card) - card_y(vad_card)) < 1.0f &&
					card_width(music_slider) >= 120.0f &&
					card_width(ptt_delay_slider) >= 180.0f;
				valid = mic_meter && activation_meter &&
					std::fabs(mic_meter->GetLevel() - 0.48f) < 0.001f &&
					std::fabs(activation_meter->GetLevel() - 0.48f) < 0.001f &&
					std::fabs(activation_meter->GetThreshold() - 0.36f) < 0.001f &&
					std::fabs(track_center - knob_center) < 0.6f &&
					std::fabs(vad_switch_center - vad_knob_center) < 0.6f && card_grid_valid;
				if (valid && scenario == "settings-select-open") {
					auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(
						document->GetElementById("settings-capture-select"));
					Rml::Element* select_box = nullptr;
					if (select) {
						for (int i = 0; i < select->GetNumChildren(true); ++i) {
							Rml::Element* child = select->GetChild(i);
							if (child && child->GetTagName() == "selectbox") {
								select_box = child;
								break;
							}
						}
					}
					const float select_width = select ? select->GetBox().GetSize(Rml::BoxArea::Border).x : 0.0f;
					const float popup_width = select_box ? select_box->GetBox().GetSize(Rml::BoxArea::Border).x : -1.0f;
					valid = select && select->IsSelectBoxVisible() && select->GetNumOptions() >= 2 &&
						select_box && std::fabs(select_width - popup_width) < 1.0f;
				}
				if (!valid) {
					std::fprintf(stderr,
						"[Designer] Settings controls failed: mic=%.3f activation=%.3f threshold=%.3f switch-dy=%.2f vad-switch-dy=%.2f cards=%d\n",
						mic_meter ? mic_meter->GetLevel() : -1.0f,
						activation_meter ? activation_meter->GetLevel() : -1.0f,
						activation_meter ? activation_meter->GetThreshold() : -1.0f,
						std::fabs(track_center - knob_center),
						std::fabs(vad_switch_center - vad_knob_center), card_grid_valid ? 1 : 0);
				}
			}
			if (valid && scenario == "settings-screen-share") {
				Rml::Element* page = document->GetElementById("settings-screen-share-page");
				Rml::Element* label = document->GetElementById("settings-bitrate-label");
				Rml::Element* value = document->GetElementById("settings-bitrate-value");
				Rml::Element* slider = document->GetElementById("settings-bitrate-slider");
				Rml::Element* bar = FindSliderPart(slider, "sliderbar");
				auto right_edge = [](Rml::Element* element) {
					return element->GetAbsoluteOffset(Rml::BoxArea::Border).x +
						element->GetBox().GetSize(Rml::BoxArea::Border).x;
				};
				auto center_y = [](Rml::Element* element) {
					return element->GetAbsoluteOffset(Rml::BoxArea::Border).y +
						element->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f;
				};
				valid = page && page->IsVisible(true) && label && value && slider &&
					bar && bar->IsPseudoClassSet("hover") &&
					label->IsVisible(true) && value->IsVisible(true) && slider->IsVisible(true) &&
					std::fabs(center_y(label) - center_y(value)) < 1.0f &&
					std::fabs(right_edge(value) - right_edge(slider)) < 2.0f &&
					std::fabs(bar->GetBox().GetSize(Rml::BoxArea::Border).x - 13.0f) < 0.5f &&
					std::fabs(bar->GetBox().GetSize(Rml::BoxArea::Border).y - 13.0f) < 0.5f &&
					value->GetInnerRML().find("Mbps") != Rml::String::npos;
				if (!valid)
					std::fprintf(stderr, "[Designer] Screen-share bitrate layout validation failed\n");
			}
			if (valid && scenario == "settings-hotkeys") {
				Rml::Element* page = document->GetElementById("settings-hotkeys-page");
				Rml::Element* intro = document->GetElementById("settings-hotkeys-intro");
				Rml::ElementList titles;
				Rml::ElementList descriptions;
				if (page) {
					page->GetElementsByClassName(titles, "settings-feature-title");
					page->GetElementsByClassName(descriptions, "settings-feature-copy");
				}
				valid = page && page->IsVisible(true) && intro &&
					std::fabs(intro->GetComputedValues().font_size() - 12.0f) < 0.1f &&
					titles.size() >= 3 && descriptions.size() >= 3;
				for (Rml::Element* title : titles)
					valid = valid && std::fabs(title->GetComputedValues().font_size() - 13.0f) < 0.1f &&
						title->GetComputedValues().font_weight() == Rml::Style::FontWeight::Bold;
				for (Rml::Element* description : descriptions)
					valid = valid && std::fabs(description->GetComputedValues().font_size() - 10.0f) < 0.1f &&
						description->GetComputedValues().font_weight() == Rml::Style::FontWeight::Normal;
				if (!valid)
					std::fprintf(stderr, "[Designer] Hotkeys typography validation failed\n");
			}
			if (valid && scenario == "settings-account") {
				Rml::Element* page = document->GetElementById("settings-account-page");
				Rml::Element* copy = document->GetElementById("settings-copy-private-key");
				Rml::Element* import = document->GetElementById("settings-show-import");
				Rml::Element* replace = document->GetElementById("settings-replace-identity");
				Rml::Element* cancel = document->GetElementById("settings-cancel-import");
				valid = page && page->IsVisible(true) && copy && import && replace && cancel &&
					copy->IsVisible(true) && replace->IsVisible(true) && cancel->IsVisible(true) &&
					copy->GetInnerRML().find("Copy") != Rml::String::npos &&
					import->GetInnerRML().find("Import") != Rml::String::npos &&
					replace->GetInnerRML().find("Replace Identity") != Rml::String::npos &&
					cancel->GetInnerRML().find("Cancel") != Rml::String::npos;
				if (!valid)
					std::fprintf(stderr, "[Designer] Account action label validation failed\n");
			}
			if (!route_valid) {
				std::fprintf(stderr,
					"[Designer] Route exclusivity failed for %s: settings=%d chat=%d\n",
					scenario.c_str(), settings_present ? 1 : 0, chat_present ? 1 : 0);
			}
			return valid;
		}

		if (scenario == "share" || scenario == "audio-share") {
			size_t target_count = 0;
			for (const auto* targets : {&lobby.share_monitor_targets, &lobby.share_application_targets}) {
				for (const auto& target : targets->get()) {
					++target_count;
					Rml::Element* preview = document->GetElementById(target.element_id);
					if (!preview || preview->GetTagName() != "video_frame") {
						std::fprintf(stderr,
							"[Designer] Missing share preview surface for target %d (%s)\n",
							target.index, target.element_id.c_str());
						return false;
					}
				}
			}

			Rml::ElementList preview_elements;
			document->GetElementsByTagName(preview_elements, "video_frame");
			size_t share_preview_count = 0;
			for (Rml::Element* preview : preview_elements) {
				if (preview->GetId().rfind("share-thumbnail-", 0) == 0)
					++share_preview_count;
			}
			if (share_preview_count != target_count) {
				std::fprintf(stderr,
					"[Designer] Duplicate share preview surfaces: targets=%zu elements=%zu\n",
					target_count, share_preview_count);
				return false;
			}
			return target_count > 0;
		}

		const bool stream_scenario = scenario == "stream-single" || scenario == "streams" ||
			scenario == "stream-fps-overflow";
		if (stream_scenario) {
			Rml::Element* slider = document->GetElementById("stream-volume-slider");
			Rml::Element* track = FindSliderPart(slider, "slidertrack");
			Rml::Element* bar = FindSliderPart(slider, "sliderbar");
			auto center_y = [](Rml::Element* element) {
				return element->GetAbsoluteOffset(Rml::BoxArea::Border).y +
					element->GetBox().GetSize(Rml::BoxArea::Border).y * 0.5f;
			};
			const float stream_slider_offset = track && bar
				? std::fabs(center_y(track) - center_y(bar)) : 999.0f;
			if (!slider || !track || !bar || stream_slider_offset >= 0.6f) {
				std::fprintf(stderr,
					"[Designer] Stream slider validation failed: slider=%d track=%d bar=%d dy=%.2f\n",
					slider ? 1 : 0, track ? 1 : 0, bar ? 1 : 0, stream_slider_offset);
				return false;
			}
		}

		if (scenario == "stream-fps-overflow") {
			Rml::Element* normal = document->GetElementById("stream-fps-normal");
			Rml::Element* overflow = document->GetElementById("stream-fps-overflow");
			Rml::Element* meter = document->GetElementById("stream-fps-meter");
			const float meter_width = meter ? meter->GetBox().GetSize(Rml::BoxArea::Border).x : 0.0f;
			const bool valid = normal && !normal->IsVisible(true) && overflow && overflow->IsVisible(true) &&
				overflow->GetInnerRML() == "999+" && std::fabs(meter_width - 66.0f) < 1.0f;
			if (!valid) {
				std::fprintf(stderr,
					"[Designer] FPS overflow validation failed: normal=%d overflow=%d text=%s width=%.1f\n",
					normal && normal->IsVisible(true) ? 1 : 0,
					overflow && overflow->IsVisible(true) ? 1 : 0,
					overflow ? overflow->GetInnerRML().c_str() : "<missing>", meter_width);
			}
			return valid;
		}

		if (scenario != "stream-single" && scenario != "streams")
			return true;

		const size_t expected_count = scenario == "stream-single" ? 1u : 2u;
		Rml::Element* first_video = document->GetElementById("screen-share-2");
		Rml::Element* second_video = document->GetElementById("screen-share-3");
		const size_t actual_count = (first_video ? 1u : 0u) + (second_video ? 1u : 0u);
		if (actual_count != expected_count) {
			std::fprintf(stderr, "[Designer] Stream layout validation expected %zu cells, got %zu\n",
				expected_count, actual_count);
			return false;
		}
		if (scenario == "stream-single")
			return true;

		Rml::Element* first_cell = first_video->GetParentNode();
		Rml::Element* second_cell = second_video->GetParentNode();
		const Rml::Vector2f first = first_cell->GetAbsoluteOffset();
		const Rml::Vector2f second = second_cell->GetAbsoluteOffset();
		const bool narrow = viewport_width <= 1280;
		const bool vertically_stacked = std::fabs(first.x - second.x) < 2.0f && second.y > first.y + 10.0f;
		const bool horizontally_tiled = std::fabs(first.y - second.y) < 2.0f && second.x > first.x + 10.0f;
		const bool valid = narrow ? vertically_stacked : horizontally_tiled;
		if (!valid) {
			std::fprintf(stderr,
				"[Designer] Stream layout validation failed at %dpx: first=(%.1f, %.1f), second=(%.1f, %.1f)\n",
				viewport_width, first.x, first.y, second.x, second.y);
		}
		return valid;
	}
};

DesignerApp::DesignerApp() = default;
DesignerApp::~DesignerApp() { Shutdown(); }

void DesignerApp::SetPreviewSize(int width, int height) {
	preview_width_ = (std::max)(1, width);
	preview_height_ = (std::max)(1, height);
}

void DesignerApp::ConfigurePartiesFixture(std::string scenario) {
	parties_fixture_scenario_ = std::move(scenario);
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
			app->dpi_scale_ = app->density_override_ > 0.0f
				? app->density_override_
				: static_cast<float>(dpi) / 96.0f;
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

	RECT window_rect{0, 0, preview_width_, preview_height_};
	AdjustWindowRectEx(&window_rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
	preview_hwnd_ = CreateWindowExW(0, L"RmlDesignerPreview",
		L"RmlUI Designer — Preview",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
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
	system_interface_ = std::make_unique<DesignerSystemInterface>(
		&rml_error_count_, &rml_data_binding_warning_count_);
	system_interface_->SetWindow(preview_hwnd_);

	Rml::SetSystemInterface(system_interface_.get());
	Rml::SetFileInterface(&file_interface_);

	// Create DX12 renderers (one per window)
	Backend::RmlRendererSettings settings{};
	settings.vsync = true;
	settings.msaa_sample_count = 4;

	preview_renderer_ = std::make_unique<PartiesRenderInterface_DX12>(
		static_cast<void*>(preview_hwnd_), settings);
	if (!*preview_renderer_) {
		std::printf("[Designer] Failed to create preview DX12 renderer\n");
		return false;
	}

	Backend::RmlRendererSettings manager_settings{};
	manager_settings.vsync = false;
	manager_settings.msaa_sample_count = 4;
	manager_renderer_ = std::make_unique<PartiesRenderInterface_DX12>(
		static_cast<void*>(manager_hwnd_), manager_settings);
	if (!*manager_renderer_) {
		std::printf("[Designer] Failed to create manager DX12 renderer\n");
		return false;
	}

	// Initialize RmlUi with the PREVIEW renderer first
	// (RmlUi captures the global render interface at context creation time)
	Rml::SetRenderInterface(preview_renderer_.get());

	if (!Rml::Initialise()) {
		std::printf("[Designer] Failed to initialise RmlUi\n");
		return false;
	}

	// Match the production Windows backend so the same RCSS parses without
	// platform-only warnings and titlebar hit-test declarations remain visible.
	Rml::StyleSheetSpecification::RegisterProperty("window-action", "none", true)
		.AddParser("keyword", "none, caption, close, minimize, maximize");

	// Register the custom elements needed for faithful Parties previews. Video
	// frames intentionally remain empty surfaces unless a fixture supplies pixels.
	element_registry_ = std::make_unique<parties::rml::ElementRegistry>();
	element_registry_->add<parties::client::GradientCircleElement>("gradient_circle");
	element_registry_->add<parties::client::LevelMeterElement>("level_meter");
	element_registry_->add<parties::client::VideoElement>("video_frame");

	// DPI
	UINT dpi = GetDpiForWindow(preview_hwnd_);
	dpi_scale_ = density_override_ > 0.0f ? density_override_ : static_cast<float>(dpi) / 96.0f;

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
		if (!preview_theme_.empty())
			preview_context_->ActivateTheme(preview_theme_, true);
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
	ShowWindow(preview_hwnd_, SW_SHOWNOACTIVATE);
	ShowWindow(manager_hwnd_, manager_visible_ ? SW_SHOWNOACTIVATE : SW_HIDE);

	// Load initial document
	if (!initial_file.empty()) {
		LoadDocument(initial_file);
	}

	std::printf("[Designer] Initialised (DPI=%.2f)\n", dpi_scale_);
	return true;
}

void DesignerApp::Shutdown() {
	text_input_editor_.reset();
	if (debugger_initialized_) {
		Rml::Debugger::Shutdown();
		debugger_initialized_ = false;
	}

	// Clear data model handles before destroying contexts
	for (auto& [name, model] : data_models_)
		model.handle = {};
	mgr_var_model_ = {};

	if (preview_context_) {
		Rml::SetRenderInterface(preview_renderer_.get());
		preview_context_->UnloadAllDocuments();
		Rml::RemoveContext("preview");
		preview_context_ = nullptr;
	}
	if (manager_context_) {
		Rml::RemoveContext("manager");
		manager_context_ = nullptr;
	}
	Rml::Shutdown();

	parties_fixture_.reset();
	element_registry_.reset();
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

int DesignerApp::RunScreenshot(const std::string& output_path, int settle_frames) {
	if (!preview_context_ || !preview_renderer_ || !last_document_load_succeeded_)
		return 1;

	fs::path absolute_output = fs::absolute(output_path);
	if (absolute_output.has_parent_path()) {
		std::error_code error;
		fs::create_directories(absolute_output.parent_path(), error);
		if (error) {
			std::fprintf(stderr, "[Designer] Failed to create screenshot directory: %s\n",
				error.message().c_str());
			return 1;
		}
	}

	const int minimum_frames = (std::max)(1, settle_frames);
	const int maximum_frames = (std::max)(minimum_frames, minimum_frames * 4);
	constexpr int required_stable_frames = 12;
	size_t previous_element_count = 0;
	int stable_frames = 0;
	int rendered_frames = 0;
	MSG message{};
	for (int frame = 0; frame < maximum_frames; ++frame) {
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		// The share picker is the renderer's geometry-churn stress case: moving
		// between cards flips :hover on a large rounded background, its border and
		// descendant text. Exercise that path on every screenshot run so the DX12
		// geometry allocator cannot silently regress to per-primitive resources.
		if (parties_fixture_scenario_ == "share" && frame > 0) {
			constexpr float hover_points[][2] = {
				{0.10f, 0.28f}, {0.30f, 0.28f}, {0.49f, 0.28f},
				{0.10f, 0.51f}, {0.30f, 0.51f}, {0.49f, 0.51f}, {0.68f, 0.51f},
			};
			const auto& point = hover_points[
				static_cast<size_t>(frame - 1) % std::size(hover_points)];
			preview_context_->ProcessMouseMove(
				static_cast<int>(preview_width_ * point[0]),
				static_cast<int>(preview_height_ * point[1]), 0);
		}
		if (parties_fixture_scenario_ == "chat-segment-churn" && parties_fixture_)
			parties_fixture_->ExerciseChatSegmentChurn(frame);
		UpdatePreview();

		const size_t element_count = CountContextElements(preview_context_);
		if (element_count == previous_element_count && element_count > 0)
			++stable_frames;
		else
			stable_frames = 0;
		previous_element_count = element_count;

		const bool minimum_elapsed = frame + 1 >= minimum_frames;
		const bool structure_settled = stable_frames >= required_stable_frames;
		const bool last_chance = frame + 1 == maximum_frames;
		const bool capture_frame = (minimum_elapsed && structure_settled) || last_chance;
		if (capture_frame)
			preview_renderer_->CaptureNextFrame(absolute_output.string());
		RenderPreview();
		rendered_frames = frame + 1;
		if (capture_frame)
			break;
	}
	std::printf("[Designer] Screenshot settled after %d frame(s), %zu elements\n",
		rendered_frames, previous_element_count);
	bool fixture_layout_valid = true;
	if (parties_fixture_)
		fixture_layout_valid = parties_fixture_->ValidateDocumentFixture(
			preview_context_->GetDocument(0), parties_fixture_scenario_, preview_width_);
	const bool screenshot_content_valid = !parties_fixture_ ||
		ValidateScreenshotCoverage(absolute_output, 0.06);
	std::fflush(stdout);
	std::fflush(stderr);
	if (rml_error_count_ > 0)
		std::fprintf(stderr, "[Designer] Screenshot failed validation with %d RmlUI error(s)\n",
			rml_error_count_);
	if (rml_data_binding_warning_count_ > 0)
		std::fprintf(stderr,
			"[Designer] Screenshot failed validation with %d RmlUI data-binding warning(s)\n",
			rml_data_binding_warning_count_);
	if (!screenshot_content_valid)
		std::fprintf(stderr, "[Designer] Screenshot is blank or insufficiently painted\n");
	return preview_renderer_->LastCaptureSucceeded() && rml_error_count_ == 0 &&
		rml_data_binding_warning_count_ == 0 &&
		fixture_layout_valid && screenshot_content_valid ? 0 : 1;
}

// ── Update / Render ──────────────────────────────────────────────────────

void DesignerApp::UpdatePreview() {
	if (!preview_context_ || preview_minimized_) return;
	preview_context_->Update();
}

void DesignerApp::RenderPreview() {
	if (!preview_renderer_ || !*preview_renderer_ || preview_minimized_) return;
	Rml::SetRenderInterface(preview_renderer_.get());
	preview_renderer_->BeginFrame();
	if (!preview_renderer_->IsFrameActive()) return;
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
	Rml::SetRenderInterface(manager_renderer_.get());
	manager_renderer_->BeginFrame();
	if (!manager_renderer_->IsFrameActive()) return;
	manager_renderer_->Clear();
	if (manager_context_) manager_context_->Render();
	manager_renderer_->EndFrame();
}

// ── Document loading ─────────────────────────────────────────────────────

void DesignerApp::LoadDocument(const std::string& rml_path) {
	if (debugger_initialized_) {
		Rml::Debugger::Shutdown();
		debugger_initialized_ = false;
	}
	last_document_load_succeeded_ = false;
	rml_error_count_ = 0;
	rml_data_binding_warning_count_ = 0;
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
		if (auto_load_bind_vars_ && fs::exists(vars_path))
			LoadBindVars(vars_path.string());
	}

	// Load
	if (preview_context_) {
		Rml::SetRenderInterface(preview_renderer_.get());
		preview_context_->UnloadAllDocuments();

		if (!parties_fixture_scenario_.empty()) {
			if (!parties_fixture_) {
				parties_fixture_ = std::make_unique<PartiesFixture>();
				if (!parties_fixture_->Init(preview_context_, parties_fixture_scenario_)) {
					std::fprintf(stderr, "[Designer] Failed to create Parties fixture: %s\n",
						parties_fixture_scenario_.c_str());
					last_document_load_succeeded_ = false;
					return;
				}
			}
		} else {
			// Rebuild scalar data models before loading (the .rml may reference them).
			RebuildPreviewDataModels();
		}

		auto* doc = preview_context_->LoadDocument(document_path_);
		if (doc) {
			// Match the production platform classes so the shared desktop design
			// system is exercised by Windows screenshot fixtures. Theme-driven iOS
			// previews remain isolated from desktop component density.
			if (preview_theme_ == "ios") {
				doc->SetClass("platform-ios", true);
			} else {
				doc->SetClass("platform-windows", true);
				doc->SetClass("platform-desktop", true);
			}
			doc->Show();
			if (parties_fixture_)
				parties_fixture_->ApplyDocumentFixture(doc, parties_fixture_scenario_);
			last_document_load_succeeded_ = rml_error_count_ == 0;
			std::printf("[Designer] Loaded: %s\n", document_path_.c_str());
			if (rml_error_count_ > 0)
				std::fprintf(stderr, "[Designer] Document loaded with %d RmlUI error(s)\n",
					rml_error_count_);
		} else {
			last_document_load_succeeded_ = false;
			std::printf("[Designer] Failed to load: %s\n", document_path_.c_str());
		}
		if (debugger_enabled_)
			debugger_initialized_ = Rml::Debugger::Initialise(preview_context_);
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
					const std::string filename = entry.path().filename().string();
					const bool fallback = filename.rfind("NotoSans-", 0) == 0;
					Rml::LoadFontFace(entry.path().string(), fallback);
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
		// Designer preview models are runtime-named and type-erased, so they use
		// the Builder facade with bind_raw / on (not the compile-time Property path).
		parties::rml::Builder b(ctor, nullptr);

		// Register array types first (required before binding arrays)
		bool has_string_array = false;
		for (auto& [var_name, var] : model.vars) {
			if (var.type == VarType::StringArray) has_string_array = true;
		}
		if (has_string_array)
			b.register_array<Rml::Vector<Rml::String>>();

		// Bind each variable
		for (auto& [var_name, var] : model.vars) {
			switch (var.type) {
			case VarType::String:
				if (var.val_string) b.bind_raw(var_name, var.val_string.get());
				break;
			case VarType::Int:
				if (var.val_int) b.bind_raw(var_name, var.val_int.get());
				break;
			case VarType::Float:
				if (var.val_float) b.bind_raw(var_name, var.val_float.get());
				break;
			case VarType::Bool:
				if (var.val_bool) b.bind_raw(var_name, var.val_bool.get());
				break;
			case VarType::StringArray:
				if (var.val_string_array) b.bind_raw(var_name, var.val_string_array.get());
				break;
			case VarType::Event:
				// Register a no-op event callback so data-event-* expressions don't error
				b.on(var_name, [] {});
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

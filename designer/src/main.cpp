// RmlUI Designer — entry point
// Usage: rmlui_designer [path/to/file.rml] [options]

#include "designer_app.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")

int main(int argc, char* argv[]) {
	// Enable per-monitor DPI awareness
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	std::string initial_file;
	std::string vars_file;
	std::string fixture;
	std::string theme;
	std::string screenshot_path;
	std::vector<std::string> asset_dirs;
	int preview_width = 1440;
	int preview_height = 900;
	int settle_frames = 240;
	float density = 0.0f;
	bool manager_visible = true;

	// Parse command line
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--asset-dir") == 0 && i + 1 < argc) {
			asset_dirs.push_back(argv[++i]);
		} else if (std::strcmp(argv[i], "--vars") == 0 && i + 1 < argc) {
			vars_file = argv[++i];
		} else if (std::strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
			fixture = argv[++i];
		} else if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
			theme = argv[++i];
		} else if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
			const char* profile = argv[++i];
			if (std::strcmp(profile, "iphone-17-pro") != 0) {
				std::fprintf(stderr, "Unknown --profile '%s'\n", profile);
				return 2;
			}
			preview_width = 1206;
			preview_height = 2622;
			density = 3.0f;
			theme = "ios";
		} else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
			screenshot_path = argv[++i];
			manager_visible = false;
		} else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			const char* size = argv[++i];
			if (std::sscanf(size, "%dx%d", &preview_width, &preview_height) != 2 ||
				preview_width <= 0 || preview_height <= 0) {
				std::fprintf(stderr, "Invalid --size '%s' (expected WIDTHxHEIGHT)\n", size);
				return 2;
			}
		} else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			settle_frames = (std::max)(1, std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--density") == 0 && i + 1 < argc) {
			density = static_cast<float>(std::atof(argv[++i]));
			if (density <= 0.0f) {
				std::fprintf(stderr, "Invalid --density (expected a positive number)\n");
				return 2;
			}
		} else if (std::strcmp(argv[i], "--no-manager") == 0) {
			manager_visible = false;
		} else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
			std::printf(
				"Usage: rmlui_designer FILE [options]\n"
				"  --asset-dir DIR       Add an asset search directory\n"
				"  --vars FILE           Load scalar preview variables before the document\n"
				"  --fixture SCENARIO    Real models: launcher, party-modal, onboarding, recovery, room, settings, settings-screen-share, settings-hotkeys, settings-account, chat, chat-segment-churn, stream-single, streams, member, share\n"
				"  --theme NAME          Activate an RCSS media theme (for example: ios)\n"
				"  --profile NAME        Device preset (currently: iphone-17-pro)\n"
				"  --size WIDTHxHEIGHT   Set exact preview client size (default 1440x900)\n"
				"  --screenshot FILE     Render, save a BMP, and exit\n"
				"  --frames COUNT        Minimum settle frames before capture (default 240)\n"
				"  --density SCALE       Override dp scale (screenshots default to 1.0)\n"
				"  --no-manager          Hide the variable manager window\n");
			return 0;
		} else if (argv[i][0] != '-') {
			initial_file = argv[i];
		} else {
			std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
			return 2;
		}
	}
	if (initial_file.empty()) {
		std::fprintf(stderr, "No RML document supplied. Use --help for usage.\n");
		return 2;
	}

	std::printf("RmlUI Designer v0.1\n");
	if (!initial_file.empty())
		std::printf("  File: %s\n", initial_file.c_str());
	if (!vars_file.empty())
		std::printf("  Vars: %s\n", vars_file.c_str());
	if (!fixture.empty())
		std::printf("  Fixture: %s\n", fixture.c_str());
	if (!theme.empty())
		std::printf("  Theme: %s\n", theme.c_str());
	if (!screenshot_path.empty())
		std::printf("  Screenshot: %s (%dx%d)\n", screenshot_path.c_str(), preview_width, preview_height);
	for (auto& d : asset_dirs)
		std::printf("  Asset dir: %s\n", d.c_str());

	designer::DesignerApp app;
	app.SetPreviewSize(preview_width, preview_height);
	app.SetTheme(theme);
	if (density > 0.0f)
		app.SetDensity(density);
	else if (!screenshot_path.empty())
		app.SetDensity(1.0f);
	app.SetManagerVisible(manager_visible);
	if (!screenshot_path.empty())
		app.SetDebuggerEnabled(false);

	if (!app.Init()) {
		std::printf("Failed to initialise designer\n");
		return 1;
	}

	// Add extra asset directories from command line
	for (auto& d : asset_dirs)
		app.AddAssetFolder(d);

	// Configure models before loading the document. Previously --vars was loaded
	// afterwards, leaving the preview bound to stale/missing variables.
	if (!fixture.empty()) {
		app.SetAutoLoadBindVars(false);
		app.ConfigurePartiesFixture(fixture);
	} else if (!vars_file.empty()) {
		app.SetAutoLoadBindVars(false);
		if (!app.LoadBindVars(vars_file)) return 1;
	}

	app.LoadDocument(initial_file);
	if (!app.LastDocumentLoadSucceeded()) return 1;

	return screenshot_path.empty() ? app.Run() : app.RunScreenshot(screenshot_path, settle_frames);
}

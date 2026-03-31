// RmlUI Designer — entry point
// Usage: rmlui_designer [path/to/file.rml] [--asset-dir DIR]...

#include "designer_app.h"

#include <cstdio>
#include <cstring>
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
	std::vector<std::string> asset_dirs;

	// Parse command line
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--asset-dir") == 0 && i + 1 < argc) {
			asset_dirs.push_back(argv[++i]);
		} else if (std::strcmp(argv[i], "--vars") == 0 && i + 1 < argc) {
			vars_file = argv[++i];
		} else if (argv[i][0] != '-') {
			initial_file = argv[i];
		}
	}

	std::printf("RmlUI Designer v0.1\n");
	if (!initial_file.empty())
		std::printf("  File: %s\n", initial_file.c_str());
	if (!vars_file.empty())
		std::printf("  Vars: %s\n", vars_file.c_str());
	for (auto& d : asset_dirs)
		std::printf("  Asset dir: %s\n", d.c_str());

	designer::DesignerApp app;

	if (!app.Init(initial_file)) {
		std::printf("Failed to initialise designer\n");
		return 1;
	}

	// Add extra asset directories from command line
	for (auto& d : asset_dirs)
		app.AddAssetFolder(d);

	// Load explicit vars file
	if (!vars_file.empty())
		app.LoadBindVars(vars_file);

	return app.Run();
}

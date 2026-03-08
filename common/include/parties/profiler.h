#pragma once

// Tracy profiler integration.
// When TRACY_ENABLE is defined (via CMake ENABLE_TRACY option), this pulls in
// Tracy's C++ API. Otherwise all macros expand to nothing.

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#ifndef TracySetThreadName
#define TracySetThreadName(name) tracy::SetThreadName(name)
#endif
#else
// No-op stubs when Tracy is disabled
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedNC(name, color)
#define ZoneText(text, size)
#define ZoneName(name, size)
#define FrameMark
#define FrameMarkNamed(name)
#define FrameMarkStart(name)
#define FrameMarkEnd(name)
#define TracyPlot(name, val)
#define TracyMessageL(msg)
#define TracySetThreadName(name)
#endif

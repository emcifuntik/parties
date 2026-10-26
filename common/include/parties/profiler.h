#pragma once

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#ifndef TracySetThreadName
#define TracySetThreadName(name) tracy::SetThreadName(name)
#endif
#else
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
#define TracySetThreadName(name) ((void)0)
#endif

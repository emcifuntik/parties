#include <client/ios_back_gesture.h>
#include <cstdio>

using namespace parties::client;

int main()
{
    int failures = 0;
    const auto check = [&](bool valid, const char* message) {
        if (!valid) { std::fprintf(stderr, "iOS back gesture: %s\n", message); ++failures; }
    };
    check(ShouldCompleteIOSBackSwipe(110, 8, 0, 390), "slow intentional edge swipe returns");
    check(ShouldCompleteIOSBackSwipe(30, 3, 700, 402), "short deliberate flick returns");
    check(!ShouldCompleteIOSBackSwipe(20, 0, 800, 390), "tiny movement stays in chat");
    check(!ShouldCompleteIOSBackSwipe(50, 0, 0, 390), "short slow swipe stays in chat");
    check(!ShouldCompleteIOSBackSwipe(-110, 0, -700, 390), "leftward movement never returns");
    check(!ShouldCompleteIOSBackSwipe(100, 180, 700, 390), "vertical scroll stays in chat");
    check(!ShouldCompleteIOSBackSwipe(100, 100, 700, 390), "diagonal scroll stays in chat");
    check(!ShouldCompleteIOSBackSwipe(150, 0, -300, 390), "reversing the swipe cancels navigation");
    check(ShouldCompleteIOSBackSwipe(64, 0, 0, 200), "small viewport has a usable threshold");
    check(!ShouldCompleteIOSBackSwipe(100, 0, 0, 1000), "large viewport requires a deliberate drag");
    check(ShouldCompleteIOSBackSwipe(120, 0, 0, 1000), "large viewport threshold remains reachable");
    return failures ? 1 : 0;
}

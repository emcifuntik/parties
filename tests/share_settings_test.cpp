#include <client/share_settings.h>

#include <cstdio>
#include <map>
#include <optional>
#include <string>

namespace {

using namespace parties::client;
using parties::VideoCodecId;

struct FakePrefs {
    std::map<std::string, std::string> values;

    std::optional<std::string> get_pref(const std::string& key) {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool set_pref(const std::string& key, const std::string& value) {
        values[key] = value;
        return true;
    }
};

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "Share settings: %s\n", message);
    return condition;
}

bool same(const ShareEncoding& a, const ShareEncoding& b) {
    return a.frame_rate == b.frame_rate && a.codec == b.codec && a.scale == b.scale;
}

}

int main() {
    bool passed = true;

    passed &= check(frames_per_second(ShareFrameRate::Fps15) == 15 &&
                    frames_per_second(ShareFrameRate::Fps30) == 30 &&
                    frames_per_second(ShareFrameRate::Fps60) == 60 &&
                    frames_per_second(ShareFrameRate::Fps120) == 120, "frame rate table");
    passed &= check(scale_factor(ShareScale::Source) == 1.0f &&
                    scale_factor(ShareScale::ThreeQuarters) == 0.75f &&
                    scale_factor(ShareScale::Half) == 0.5f &&
                    scale_factor(ShareScale::Quarter) == 0.25f, "scale table");
    passed &= check(video_codec_of(ShareCodec::AV1) == VideoCodecId::AV1 &&
                    video_codec_of(ShareCodec::H265) == VideoCodecId::H265 &&
                    video_codec_of(ShareCodec::H264) == VideoCodecId::H264, "codec mapping");
    passed &= check(static_cast<int>(ShareCodec::H265) == 1 && static_cast<int>(VideoCodecId::H265) == 2,
                    "UI codec index must stay distinct from the wire codec id");

    passed &= check(parse_share_pref<ShareFrameRate>("3") == ShareFrameRate::Fps120, "persisted fps index 3");
    passed &= check(parse_share_pref<ShareCodec>("2") == ShareCodec::H264, "persisted codec index 2");
    passed &= check(!parse_share_pref<ShareCodec>("3"), "codec index past the table accepted");
    passed &= check(!parse_share_pref<ShareScale>("-1"), "negative scale index accepted");
    passed &= check(!parse_share_pref<ShareFrameRate>(""), "empty fps accepted");
    passed &= check(!parse_share_pref<ShareFrameRate>("2x"), "trailing garbage accepted");

    FakePrefs empty;
    passed &= check(same(load_share_encoding(empty), ShareEncoding{}), "missing prefs did not yield the defaults");

    FakePrefs stored;
    stored.values = {{"video.share_fps", "1"}, {"video.share_codec", "1"}, {"video.share_scale", "3"}};
    const ShareEncoding loaded = load_share_encoding(stored);
    passed &= check(loaded.frame_rate == ShareFrameRate::Fps30 && loaded.codec == ShareCodec::H265 &&
                    loaded.scale == ShareScale::Quarter, "stored indices were not decoded");

    FakePrefs out_of_table;
    out_of_table.values = {{"video.share_fps", "9"}, {"video.share_codec", "-4"}, {"video.share_scale", "1"}};
    const ShareEncoding fallback = load_share_encoding(out_of_table);
    passed &= check(fallback.frame_rate == ShareEncoding{}.frame_rate && fallback.codec == ShareEncoding{}.codec &&
                    fallback.scale == ShareScale::ThreeQuarters, "out-of-table values did not fall back per field");

    FakePrefs saved;
    save_share_encoding(saved, {ShareFrameRate::Fps120, ShareCodec::H264, ShareScale::Half});
    passed &= check(saved.values["video.share_fps"] == "3" && saved.values["video.share_codec"] == "2" &&
                    saved.values["video.share_scale"] == "2", "persisted encoding changed");
    passed &= check(same(load_share_encoding(saved), {ShareFrameRate::Fps120, ShareCodec::H264, ShareScale::Half}),
                    "save and load did not round-trip");

    return passed ? 0 : 1;
}

#pragma once

#include <parties/enum_names.h>
#include <parties/video_common.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace parties::client {

// The numeric values are both the RML segment index and the persisted
// "video.share_*" encoding, so they must never be renumbered. ShareCodec is
// deliberately not VideoCodecId: the wire ids start at 1.
enum class ShareCodec : int { AV1 = 0, H265 = 1, H264 = 2 };
enum class ShareScale : int { Source = 0, ThreeQuarters = 1, Half = 2, Quarter = 3 };
enum class ShareFrameRate : int { Fps15 = 0, Fps30 = 1, Fps60 = 2, Fps120 = 3 };
enum class SharePickerMode : int { Screen = 0, ApplicationAudio = 1 };

}

namespace parties {

template <>
inline constexpr auto enum_names_v<client::ShareCodec> = EnumNames<client::ShareCodec, 3>{{{
    {client::ShareCodec::AV1, "av1"},
    {client::ShareCodec::H265, "h265"},
    {client::ShareCodec::H264, "h264"},
}}};

template <>
inline constexpr auto enum_names_v<client::ShareScale> = EnumNames<client::ShareScale, 4>{{{
    {client::ShareScale::Source, "source"},
    {client::ShareScale::ThreeQuarters, "75"},
    {client::ShareScale::Half, "50"},
    {client::ShareScale::Quarter, "25"},
}}};

template <>
inline constexpr auto enum_names_v<client::ShareFrameRate> = EnumNames<client::ShareFrameRate, 4>{{{
    {client::ShareFrameRate::Fps15, "15"},
    {client::ShareFrameRate::Fps30, "30"},
    {client::ShareFrameRate::Fps60, "60"},
    {client::ShareFrameRate::Fps120, "120"},
}}};

template <>
inline constexpr auto enum_names_v<client::SharePickerMode> = EnumNames<client::SharePickerMode, 2>{{{
    {client::SharePickerMode::Screen, "screen"},
    {client::SharePickerMode::ApplicationAudio, "app-audio"},
}}};

}

namespace parties::client {

constexpr uint32_t frames_per_second(ShareFrameRate rate) {
    switch (rate) {
        case ShareFrameRate::Fps15:  return 15;
        case ShareFrameRate::Fps30:  return 30;
        case ShareFrameRate::Fps60:  return 60;
        case ShareFrameRate::Fps120: return 120;
    }
    return 60;
}

constexpr float scale_factor(ShareScale scale) {
    switch (scale) {
        case ShareScale::Source:        return 1.0f;
        case ShareScale::ThreeQuarters: return 0.75f;
        case ShareScale::Half:          return 0.5f;
        case ShareScale::Quarter:       return 0.25f;
    }
    return 1.0f;
}

constexpr VideoCodecId video_codec_of(ShareCodec codec) {
    switch (codec) {
        case ShareCodec::AV1:  return VideoCodecId::AV1;
        case ShareCodec::H265: return VideoCodecId::H265;
        case ShareCodec::H264: return VideoCodecId::H264;
    }
    return VideoCodecId::AV1;
}

struct ShareEncoding {
    ShareFrameRate frame_rate = ShareFrameRate::Fps60;
    ShareCodec     codec      = ShareCodec::AV1;
    ShareScale     scale      = ShareScale::Source;
};

inline constexpr const char* kShareFrameRatePref = "video.share_fps";
inline constexpr const char* kShareCodecPref     = "video.share_codec";
inline constexpr const char* kShareScalePref     = "video.share_scale";

template <class E>
std::optional<E> parse_share_pref(std::string_view stored) {
    int raw = 0;
    const char* end = stored.data() + stored.size();
    const auto [ptr, ec] = std::from_chars(stored.data(), end, raw);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return enum_names_v<E>.from_int(raw);
}

template <class E>
std::string share_pref_text(E value) {
    return std::to_string(static_cast<int>(value));
}

template <class Store>
ShareEncoding load_share_encoding(Store& store) {
    ShareEncoding encoding;
    auto read = [&store](const char* key, auto& field) {
        using E = std::remove_reference_t<decltype(field)>;
        if (const auto stored = store.get_pref(key))
            if (const auto value = parse_share_pref<E>(*stored)) field = *value;
    };
    read(kShareFrameRatePref, encoding.frame_rate);
    read(kShareCodecPref, encoding.codec);
    read(kShareScalePref, encoding.scale);
    return encoding;
}

template <class Store>
void save_share_encoding(Store& store, const ShareEncoding& encoding) {
    store.set_pref(kShareFrameRatePref, share_pref_text(encoding.frame_rate));
    store.set_pref(kShareCodecPref, share_pref_text(encoding.codec));
    store.set_pref(kShareScalePref, share_pref_text(encoding.scale));
}

}

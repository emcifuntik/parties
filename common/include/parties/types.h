#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace parties {

using UserId    = uint32_t;
using ChannelId = uint32_t;

using PublicKey   = std::array<uint8_t, 32>;
using SecretKey   = std::array<uint8_t, 32>;
using Signature   = std::array<uint8_t, 64>;
using Fingerprint = std::string;

}

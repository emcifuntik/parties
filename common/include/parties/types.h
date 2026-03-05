#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace parties {

using UserId    = uint32_t;
using ChannelId = uint32_t;

using SessionToken = std::array<uint8_t, 32>;
using ChannelKey   = std::array<uint8_t, 32>;

// Ed25519 identity types
using PublicKey   = std::array<uint8_t, 32>;
using SecretKey   = std::array<uint8_t, 32>;  // Ed25519 seed (not expanded)
using Signature   = std::array<uint8_t, 64>;
using Fingerprint = std::string;              // "aa:bb:cc:..." SHA-256 hex

} // namespace parties

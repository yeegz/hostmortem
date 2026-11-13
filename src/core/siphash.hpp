// SipHash-2-4 (Aumasson & Bernstein): a keyed PRF. Used to pseudonymise DNS
// names (FR8) so the store can show "the same name failed 12 times" without
// recording the name. The 128-bit key is random per install and lives in the
// store's meta table; without it the hashes cannot be brute-forced offline
// from a dictionary of names.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace hm {

using SipKey = std::array<std::uint8_t, 16>;

std::uint64_t siphash24(const SipKey& key, std::string_view data) noexcept;

// "h:" + 16 hex digits, the form stored in attrs.
std::string hash_name(const SipKey& key, std::string_view name);

}  // namespace hm

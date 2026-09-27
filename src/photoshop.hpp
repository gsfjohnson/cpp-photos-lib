// Internal: Photoshop image resource blocks (8BIM), which carry IPTC in JPEG
// APP13 segments and TIFF tag 0x8649.
#pragma once

#include <photos/iptc.hpp>
#include <photos/types.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace photos::detail {

constexpr std::uint16_t kIptcResource = 0x0404;
constexpr std::uint16_t kIptcDigestResource = 0x0425;

struct ImageResource {
  std::uint32_t signature;  // "8BIM" and the like
  std::uint16_t id;
  Bytes name;  // the Pascal string, length byte first
  Bytes data;
};

// Parses resources until the data ends or stops making sense.
std::vector<ImageResource> parseImageResources(const std::uint8_t* data, std::size_t size);
Bytes serializeImageResources(const std::vector<ImageResource>& resources);

// The IPTC in resource 0x0404, if any.
std::optional<IptcData> iptcFromImageResources(const std::vector<ImageResource>& resources);
// Replaces (or removes, when empty) resource 0x0404 and updates the 0x0425
// digest Photoshop uses to notice IPTC edited elsewhere.
void setIptcImageResource(std::vector<ImageResource>& resources, const IptcData& iptc);

// MD5 digest (md5.cpp).
std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size);

}  // namespace photos::detail

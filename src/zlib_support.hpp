// Internal: zlib, when the library is built with it.
#pragma once

#include <photos/types.hpp>

#include <optional>

namespace photos::detail {

bool haveZlib() noexcept;
// Inflates a zlib stream; std::nullopt without zlib, on bad data, or when the
// output would exceed maxSize.
std::optional<Bytes> zlibInflate(const std::uint8_t* data, std::size_t size, std::size_t maxSize);
// Deflates into a zlib stream; std::nullopt without zlib.
std::optional<Bytes> zlibDeflate(const std::uint8_t* data, std::size_t size);

}  // namespace photos::detail

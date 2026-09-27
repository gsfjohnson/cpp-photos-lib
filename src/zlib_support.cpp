#include "zlib_support.hpp"

#ifdef PHOTOS_HAVE_ZLIB
#include <zlib.h>
#endif

namespace photos::detail {

#ifdef PHOTOS_HAVE_ZLIB

bool haveZlib() noexcept { return true; }

std::optional<Bytes> zlibInflate(const std::uint8_t* data, std::size_t size, std::size_t maxSize) {
  z_stream z{};
  if (inflateInit(&z) != Z_OK) return std::nullopt;
  Bytes out;
  std::uint8_t buffer[16384];
  z.next_in = const_cast<Bytef*>(data);
  z.avail_in = static_cast<uInt>(size);
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    z.next_out = buffer;
    z.avail_out = sizeof buffer;
    rc = inflate(&z, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) break;
    out.insert(out.end(), buffer, buffer + (sizeof buffer - z.avail_out));
    if (out.size() > maxSize) break;
    if (rc == Z_OK && z.avail_in == 0 && z.avail_out != 0) break;  // truncated
  }
  inflateEnd(&z);
  if (rc != Z_STREAM_END || out.size() > maxSize) return std::nullopt;
  return out;
}

std::optional<Bytes> zlibDeflate(const std::uint8_t* data, std::size_t size) {
  uLongf n = compressBound(static_cast<uLong>(size));
  Bytes out(n);
  if (compress2(out.data(), &n, data, static_cast<uLong>(size), Z_BEST_COMPRESSION) != Z_OK) return std::nullopt;
  out.resize(n);
  return out;
}

#else

bool haveZlib() noexcept { return false; }
std::optional<Bytes> zlibInflate(const std::uint8_t*, std::size_t, std::size_t) { return std::nullopt; }
std::optional<Bytes> zlibDeflate(const std::uint8_t*, std::size_t) { return std::nullopt; }

#endif

}  // namespace photos::detail

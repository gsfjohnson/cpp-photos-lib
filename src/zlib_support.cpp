#include "zlib_support.hpp"

#include <algorithm>

#ifdef LUMENLIB_HAVE_ZLIB
#include <zlib.h>
#endif

namespace lumenlib::detail {

#ifdef LUMENLIB_HAVE_ZLIB

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
// Only streams of stored (uncompressed) blocks, such as zlibDeflate writes
// here; anything compressed needs zlib.
std::optional<Bytes> zlibInflate(const std::uint8_t* data, std::size_t size, std::size_t maxSize) {
  if (size < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) {
    return std::nullopt;
  }
  Bytes out;
  std::size_t pos = 2;
  for (;;) {
    if (pos + 5 > size) return std::nullopt;
    const std::uint8_t header = data[pos];
    if (((header >> 1) & 3) != 0) return std::nullopt;  // a compressed block
    const std::size_t n = data[pos + 1] | (data[pos + 2] << 8);
    const std::size_t check = data[pos + 3] | (data[pos + 4] << 8);
    if ((n ^ 0xffff) != check || pos + 5 + n > size || out.size() + n > maxSize) return std::nullopt;
    out.insert(out.end(), data + pos + 5, data + pos + 5 + n);
    pos += 5 + n;
    if (header & 1) return out;
  }
}
// A zlib stream of stored (uncompressed) deflate blocks, which every
// inflater reads: larger than compressed, but always possible.
std::optional<Bytes> zlibDeflate(const std::uint8_t* data, std::size_t size) {
  Bytes out = {0x78, 0x01};
  std::size_t pos = 0;
  do {
    const std::size_t n = std::min<std::size_t>(size - pos, 0xffff);
    out.push_back(pos + n == size ? 1 : 0);  // BFINAL, BTYPE 00
    out.push_back(static_cast<std::uint8_t>(n));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.push_back(static_cast<std::uint8_t>(~n));
    out.push_back(static_cast<std::uint8_t>(~n >> 8));
    out.insert(out.end(), data + pos, data + pos + n);
    pos += n;
  } while (pos < size);
  std::uint32_t a = 1, b = 0;  // Adler-32
  for (std::size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  const std::uint32_t adler = (b << 16) | a;
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(adler >> shift));
  return out;
}

#endif

}  // namespace lumenlib::detail

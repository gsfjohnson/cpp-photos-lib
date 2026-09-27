// Internal: byte-order helpers and bounds-checked access to untrusted data.
#pragma once

#include <lumenlib/error.hpp>
#include <lumenlib/types.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lumenlib::detail {

inline std::uint16_t get16(const std::uint8_t* p, ByteOrder order) noexcept {
  return order == ByteOrder::little ? static_cast<std::uint16_t>(p[0] | (p[1] << 8))
                                    : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t get32(const std::uint8_t* p, ByteOrder order) noexcept {
  if (order == ByteOrder::little) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  }
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t get64(const std::uint8_t* p, ByteOrder order) noexcept {
  const std::uint64_t a = get32(p, order);
  const std::uint64_t b = get32(p + 4, order);
  return order == ByteOrder::little ? (b << 32) | a : (a << 32) | b;
}

inline std::uint16_t getBe16(const std::uint8_t* p) noexcept { return get16(p, ByteOrder::big); }
inline std::uint32_t getBe32(const std::uint8_t* p) noexcept { return get32(p, ByteOrder::big); }
inline std::uint64_t getBe64(const std::uint8_t* p) noexcept { return get64(p, ByteOrder::big); }
inline std::uint16_t getLe16(const std::uint8_t* p) noexcept { return get16(p, ByteOrder::little); }
inline std::uint32_t getLe32(const std::uint8_t* p) noexcept { return get32(p, ByteOrder::little); }

inline void put16(std::uint8_t* p, std::uint16_t v, ByteOrder order) noexcept {
  if (order == ByteOrder::little) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
  } else {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
  }
}

inline void put32(std::uint8_t* p, std::uint32_t v, ByteOrder order) noexcept {
  if (order == ByteOrder::little) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
  } else {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * (3 - i)));
  }
}

inline void append16(Bytes& out, std::uint16_t v, ByteOrder order) {
  std::uint8_t b[2];
  put16(b, v, order);
  out.insert(out.end(), b, b + 2);
}

inline void append32(Bytes& out, std::uint32_t v, ByteOrder order) {
  std::uint8_t b[4];
  put32(b, v, order);
  out.insert(out.end(), b, b + 4);
}

inline void appendBe16(Bytes& out, std::uint16_t v) { append16(out, v, ByteOrder::big); }
inline void appendBe32(Bytes& out, std::uint32_t v) { append32(out, v, ByteOrder::big); }
inline void appendLe32(Bytes& out, std::uint32_t v) { append32(out, v, ByteOrder::little); }

inline void append(Bytes& out, std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }
inline void append(Bytes& out, const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
inline void append(Bytes& out, const std::uint8_t* p, std::size_t n) { out.insert(out.end(), p, p + n); }

// Whether [offset, offset + n) lies within a buffer of `size` bytes, without
// overflowing.
inline bool inBounds(std::uint64_t size, std::uint64_t offset, std::uint64_t n) noexcept {
  return offset <= size && n <= size - offset;
}

inline bool startsWith(const std::uint8_t* data, std::size_t size, std::string_view prefix) noexcept {
  return size >= prefix.size() && std::memcmp(data, prefix.data(), prefix.size()) == 0;
}

inline bool startsWith(const Bytes& data, std::string_view prefix) noexcept {
  return startsWith(data.data(), data.size(), prefix);
}

// Whether a size fits a 32-bit length field (always, on 32-bit targets).
inline bool fits32(std::size_t n) noexcept { return (static_cast<std::uint64_t>(n) >> 32) == 0; }

[[noreturn]] inline void corrupt(const std::string& what) { throw Error(ErrorCode::corruptData, what); }

// Passes a warning to the handler set with setWarningHandler (error.cpp).
void warn(const std::string& message);

inline std::string toText(const std::uint8_t* p, std::size_t n) {
  return std::string(reinterpret_cast<const char*>(p), n);
}

}  // namespace lumenlib::detail

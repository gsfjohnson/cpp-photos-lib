// A typed TIFF/Exif value: one or more components of a TypeId.
#pragma once

#include <photos/export.hpp>
#include <photos/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photos {

class PHOTOS_EXPORT Value {
 public:
  // An empty value of the given type.
  explicit Value(TypeId type = TypeId::undefined);

  // Text for an Ascii value; the terminating NUL is added when serialised.
  static Value ascii(std::string_view text);
  // Raw bytes for an Undefined, Byte or SByte value.
  static Value bytes(TypeId type, Bytes data);
  // Integer components for Byte, SByte, Short, SShort, Long, SLong or Ifd.
  // Throws Error(invalidArgument) when a component is out of the type's range.
  static Value integers(TypeId type, const std::vector<std::int64_t>& components);
  // Rational components for Rational or SRational.
  static Value rationals(TypeId type, const std::vector<Rational>& components);
  // Floating-point components for Float or Double.
  static Value reals(TypeId type, const std::vector<double>& components);

  // Parses text the way toString() prints it: an Ascii value takes the text
  // as is; numeric types take space-separated components, rationals as "n/d"
  // (or a decimal, which is converted); Undefined takes space-separated
  // decimal bytes. Throws Error(invalidArgument) on malformed text.
  static Value fromString(TypeId type, std::string_view text);

  // Decodes `count` components serialised in `order`. `size` must be at least
  // count * typeSize(type).
  static Value fromBytes(TypeId type, const std::uint8_t* data, std::size_t size, std::size_t count, ByteOrder order);

  TypeId typeId() const noexcept { return type_; }
  // Number of components (for Ascii, bytes including the terminating NUL).
  std::size_t count() const noexcept;
  // Size of the serialised value in bytes.
  std::size_t sizeInBytes() const noexcept { return count() * typeSize(type_); }
  bool empty() const noexcept { return count() == 0; }

  // Component i as an integer (rationals and reals are truncated).
  // Throws Error(invalidArgument) when i is out of range.
  std::int64_t toInt64(std::size_t i = 0) const;
  double toDouble(std::size_t i = 0) const;
  Rational toRational(std::size_t i = 0) const;

  // Ascii: the text up to the first NUL. Numbers: components separated by a
  // space; rationals as "n/d". Undefined and bytes: decimal bytes separated
  // by a space.
  std::string toString() const;
  // Component i as text.
  std::string toString(std::size_t i) const;

  // The serialised components in the given byte order.
  Bytes toBytes(ByteOrder order) const;

  // The raw bytes of an Ascii, Byte, SByte or Undefined value (Ascii
  // including its terminating NUL); empty for other types.
  const Bytes& rawBytes() const noexcept { return bytes_; }

  friend PHOTOS_EXPORT bool operator==(const Value& a, const Value& b) noexcept;
  friend bool operator!=(const Value& a, const Value& b) noexcept { return !(a == b); }

 private:
  TypeId type_;
  Bytes bytes_;                      // Ascii, Byte, SByte, Undefined
  std::vector<std::int64_t> ints_;   // Short, SShort, Long, SLong, Ifd
  std::vector<Rational> rationals_;  // Rational, SRational
  std::vector<double> reals_;        // Float, Double
};

}  // namespace photos

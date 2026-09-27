// A typed TIFF field value: one or more components of a FieldType.
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lumenlib {

class LUMENLIB_EXPORT FieldValue {
 public:
  // An empty value of the given type.
  explicit FieldValue(FieldType type = FieldType::undefined);

  // Text for an ASCII value; the terminating NUL is added here.
  static FieldValue ascii(std::string_view text);
  // Raw bytes for an UNDEFINED, BYTE or SBYTE value.
  static FieldValue fromBytes(FieldType type, Bytes data);
  // Integer components for BYTE, SBYTE, SHORT, SSHORT, LONG, SLONG, IFD (or
  // UNDEFINED, one byte each). Throws Error(invalidArgument) when a component
  // is out of the type's range.
  static FieldValue integers(FieldType type, const std::vector<std::int64_t>& components);
  // Rational components for RATIONAL or SRATIONAL.
  static FieldValue rationals(FieldType type, const std::vector<Rational>& components);
  // Floating-point components for FLOAT or DOUBLE (or a rational type, whose
  // components become the closest fractions).
  static FieldValue reals(FieldType type, const std::vector<double>& components);

  // Parses text the way text() prints it: an ASCII value takes the text as
  // is; numeric types take space-separated components, rationals as "n/d"
  // (or a decimal, which is converted); UNDEFINED takes space-separated
  // decimal bytes. Throws Error(invalidArgument) on malformed text.
  static FieldValue parse(FieldType type, std::string_view text);

  // Decodes `count` components serialised in `order`. `size` must be at least
  // count * fieldTypeSize(type); throws Error(corruptData) otherwise.
  static FieldValue decode(FieldType type, const std::uint8_t* data, std::size_t size, std::size_t count,
                           ByteOrder order);

  FieldType type() const noexcept { return type_; }
  // Number of components (for ASCII, bytes including the terminating NUL).
  std::size_t count() const noexcept;
  // Size of the serialised value in bytes.
  std::size_t byteSize() const noexcept { return count() * fieldTypeSize(type_); }
  bool empty() const noexcept { return count() == 0; }

  // Component i as an integer (rationals and reals are truncated).
  // Throws Error(invalidArgument) when i is out of range.
  std::int64_t asInt(std::size_t i = 0) const;
  double asDouble(std::size_t i = 0) const;
  Rational asRational(std::size_t i = 0) const;

  // ASCII: the text up to the first NUL. Numbers: components separated by a
  // space; rationals as "n/d". UNDEFINED and bytes: decimal bytes separated
  // by a space.
  std::string text() const;
  // Component i as text.
  std::string text(std::size_t i) const;

  // The serialised components in the given byte order.
  Bytes encode(ByteOrder order) const;

  // The bytes of an ASCII, BYTE, SBYTE or UNDEFINED value (ASCII including
  // its terminating NUL); empty for other types.
  const Bytes& bytes() const noexcept { return bytes_; }

  friend LUMENLIB_EXPORT bool operator==(const FieldValue& a, const FieldValue& b) noexcept;
  friend bool operator!=(const FieldValue& a, const FieldValue& b) noexcept { return !(a == b); }

 private:
  FieldType type_;
  Bytes bytes_;                      // ASCII, BYTE, SBYTE, UNDEFINED
  std::vector<std::int64_t> ints_;   // SHORT, SSHORT, LONG, SLONG, IFD
  std::vector<Rational> rationals_;  // RATIONAL, SRATIONAL
  std::vector<double> reals_;        // FLOAT, DOUBLE
};

}  // namespace lumenlib

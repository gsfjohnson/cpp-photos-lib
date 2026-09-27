// Basic types shared by the metadata classes.
#pragma once

#include <lumenlib/export.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lumenlib {

using Bytes = std::vector<std::uint8_t>;

enum class ByteOrder { little, big };

// A TIFF rational. Unsigned rationals use the same type; their components are
// never negative.
struct LUMENLIB_EXPORT Rational {
  std::int64_t numerator = 0;
  std::int64_t denominator = 1;

  // numerator / denominator, or 0 when the denominator is 0.
  double toDouble() const noexcept;
  friend bool operator==(const Rational& a, const Rational& b) noexcept {
    return a.numerator == b.numerator && a.denominator == b.denominator;
  }
  friend bool operator!=(const Rational& a, const Rational& b) noexcept { return !(a == b); }
};

// TIFF field types, numbered as in TIFF 6.0 (section 2) and TIFF supplement 1
// (IFD). The names say the component: u = unsigned, i = signed, f = floating.
enum class FieldType : std::uint16_t {
  u8 = 1,          // BYTE
  ascii = 2,       // ASCII
  u16 = 3,         // SHORT
  u32 = 4,         // LONG
  urational = 5,   // RATIONAL
  i8 = 6,          // SBYTE
  undefined = 7,   // UNDEFINED
  i16 = 8,         // SSHORT
  i32 = 9,         // SLONG
  srational = 10,  // SRATIONAL
  f32 = 11,        // FLOAT
  f64 = 12,        // DOUBLE
  ifd = 13,        // IFD
};

// Size in bytes of one component of the type; 0 for an unknown type.
LUMENLIB_EXPORT std::size_t fieldTypeSize(FieldType type) noexcept;
// The TIFF specification's name for the type ("BYTE", "ASCII", "SHORT", ...).
LUMENLIB_EXPORT const char* fieldTypeName(FieldType type) noexcept;
// Whether the number is one of the types above.
LUMENLIB_EXPORT bool isFieldType(std::uint16_t type) noexcept;

}  // namespace lumenlib

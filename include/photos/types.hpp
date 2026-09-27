// Basic types shared by the metadata classes.
#pragma once

#include <photos/export.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace photos {

using Bytes = std::vector<std::uint8_t>;

enum class ByteOrder { littleEndian, bigEndian };

// A TIFF rational. Unsigned rationals use the same type; their components are
// never negative.
struct PHOTOS_EXPORT Rational {
  std::int64_t numerator = 0;
  std::int64_t denominator = 1;

  // numerator / denominator, or 0 when the denominator is 0.
  double toDouble() const noexcept;
  friend bool operator==(const Rational& a, const Rational& b) noexcept {
    return a.numerator == b.numerator && a.denominator == b.denominator;
  }
  friend bool operator!=(const Rational& a, const Rational& b) noexcept { return !(a == b); }
};

// TIFF field types, numbered as in the TIFF 6.0 and Exif specifications.
enum class TypeId : std::uint16_t {
  unsignedByte = 1,
  asciiString = 2,
  unsignedShort = 3,
  unsignedLong = 4,
  unsignedRational = 5,
  signedByte = 6,
  undefined = 7,
  signedShort = 8,
  signedLong = 9,
  signedRational = 10,
  tiffFloat = 11,
  tiffDouble = 12,
  tiffIfd = 13,
};

// Size in bytes of one component of the type; 0 for an unknown type.
PHOTOS_EXPORT std::size_t typeSize(TypeId type) noexcept;
// exiv2's name for the type ("Ascii", "Short", "Rational", ...).
PHOTOS_EXPORT const char* typeName(TypeId type) noexcept;
// Whether the numeric value is one of the types above.
PHOTOS_EXPORT bool isValidType(std::uint16_t type) noexcept;

}  // namespace photos

// Internal: the TIFF structure behind ExifData.
#pragma once

#include <photos/exif.hpp>
#include <photos/io.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace photos::detail {

// Where a decoded entry lives in the block it came from.
struct OriginEntry {
  IfdId ifd;
  std::uint16_t tag;
  Value value;
  std::size_t entryOffset;  // the 12-byte IFD entry
  std::size_t valueOffset;  // the value (inside the entry when it fits in 4 bytes)
  std::size_t capacity;     // bytes available at valueOffset
};

struct ExifOrigin {
  Bytes tiff;
  ByteOrder order = ByteOrder::littleEndian;
  std::vector<OriginEntry> entries;
  Bytes thumbnail;
  bool patchable = true;
};

struct ExifAccess {
  static std::shared_ptr<const ExifOrigin>& origin(ExifData& d) noexcept { return d.origin_; }
  static const std::shared_ptr<const ExifOrigin>& origin(const ExifData& d) noexcept { return d.origin_; }
  static Bytes& thumbnail(ExifData& d) noexcept { return d.thumbnail_; }
};

struct TiffDecodeOptions {
  // The IFD the header's first offset points to (CR3 keeps the Exif and GPS
  // IFDs in TIFF structures of their own).
  IfdId root = IfdId::ifd0;
  // Keep the block so an unchanged or same-size edit is written back as is.
  bool keepOrigin = true;
  // Accept the raw formats' variants of the TIFF magic number (ORF, RW2).
  bool allowRawMagic = false;
};

// Whether the bytes start with a TIFF header.
bool isTiffHeader(const std::uint8_t* data, std::size_t size, bool allowRawMagic = false) noexcept;

// Adds the entries of a TIFF structure to `exif` (and sets its byte order).
// A damaged IFD or entry is skipped; a missing or bad header throws
// Error(corruptData).
void decodeTiff(const std::uint8_t* data, std::size_t size, ExifData& exif, const TiffDecodeOptions& options = {});
void decodeTiff(const InputSource& source, ExifData& exif, const TiffDecodeOptions& options = {});

// Encodes the data as a TIFF structure (the payload of an Exif APP1 segment,
// eXIf chunk or EXIF chunk). Empty when there is nothing to write.
Bytes encodeExif(const ExifData& exif);

}  // namespace photos::detail

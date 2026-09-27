// Internal: the TIFF structure behind ExifMetadata.
#pragma once

#include <lumenlib/exif.hpp>
#include <lumenlib/io.hpp>
#include <lumenlib/makernote.hpp>

#include "makernote_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lumenlib::detail {

constexpr std::uint16_t kExifIfdPointer = 0x8769;
constexpr std::uint16_t kGpsIfdPointer = 0x8825;
constexpr std::uint16_t kInteropIfdPointer = 0xa005;
constexpr std::uint16_t kThumbnailOffset = 0x0201;
constexpr std::uint16_t kThumbnailLength = 0x0202;
constexpr std::uint16_t kMakerNote = 0x927c;

// Where a decoded entry lives in the block it came from.
struct OriginEntry {
  Ifd ifd;
  std::uint16_t tag;
  FieldValue value;
  std::size_t entryOffset;  // the 12-byte IFD entry
  std::size_t valueOffset;  // the value (inside the entry when it fits in 4 bytes)
  std::size_t capacity;     // bytes available at valueOffset
};

struct ExifOrigin {
  Bytes tiff;
  ByteOrder order = ByteOrder::little;
  std::vector<OriginEntry> entries;
  Bytes thumbnail;
  bool patchable = true;
};

struct MakerNoteOrigin {
  MakerNoteLayout layout;
  std::uint64_t offset;  // in the block it was read from
  FieldValue value;      // as read, to tell whether it was changed
};

struct ExifAccess {
  static std::shared_ptr<const ExifOrigin>& origin(ExifMetadata& d) noexcept { return d.origin_; }
  static const std::shared_ptr<const ExifOrigin>& origin(const ExifMetadata& d) noexcept { return d.origin_; }
  static Bytes& thumbnail(ExifMetadata& d) noexcept { return d.thumbnail_; }
  static std::shared_ptr<const MakerNote>& makerNote(ExifMetadata& d) noexcept { return d.makerNote_; }
  static std::shared_ptr<const MakerNoteOrigin>& makerNoteOrigin(ExifMetadata& d) noexcept {
    return d.makerNoteOrigin_;
  }
  static const std::shared_ptr<const MakerNoteOrigin>& makerNoteOrigin(const ExifMetadata& d) noexcept {
    return d.makerNoteOrigin_;
  }
};

struct TiffDecodeOptions {
  // The IFD the header's first offset points to (CR3 keeps the Exif and GPS
  // IFDs in TIFF structures of their own).
  Ifd root = Ifd::ifd0;
  // Keep the block so an unchanged or same-size edit is written back as is.
  bool keepOrigin = true;
  // Accept the raw formats' variants of the TIFF magic number (ORF, RW2).
  bool allowRawMagic = false;
  // Follow IFD0's next-IFD link to IFD1 (not in a TIFF file, whose later
  // IFDs are pages and previews rather than an Exif thumbnail).
  bool readIfd1 = true;
  // Values larger than this are skipped rather than read (raw files hold
  // multi-megabyte strips; metadata never is).
  std::uint64_t maxValueSize = 64u << 20;
};

// Whether the bytes start with a TIFF header.
bool isTiffHeader(const std::uint8_t* data, std::size_t size, bool allowRawMagic = false) noexcept;

// Adds the entries of a TIFF structure to `exif` (and sets its byte order),
// and decodes the maker note. A damaged IFD or entry is skipped; a missing
// or bad header throws Error(corruptData).
void decodeTiff(const std::uint8_t* data, std::size_t size, ExifMetadata& exif, const TiffDecodeOptions& options = {});
void decodeTiff(const InputSource& source, ExifMetadata& exif, const TiffDecodeOptions& options = {});

// ExifMetadata::encode.
Bytes encodeExif(const ExifMetadata& exif);

// The structural tags written from the layout rather than the data.
bool isStructuralTag(Ifd ifd, std::uint16_t tag) noexcept;

// One IFD laid out for writing: entries sorted by tag, their values after
// the entry table, each at an even offset.
struct IfdEntry {
  std::uint16_t tag;
  FieldType type;
  std::uint32_t count;
  Bytes data;
  // A value kept where it is in the file being written (its offset is data).
  bool inPlace = false;
};

struct IfdLayout {
  std::vector<IfdEntry> entries;

  void sort();
  // Bytes the IFD takes: the table, the next-IFD link and the values.
  std::size_t size() const;
  // Where each out-of-line value goes when the IFD is at `offset`; 0 for a
  // value inside its entry or kept in place.
  std::vector<std::uint64_t> valueOffsets(std::uint64_t offset) const;
  // Appends the IFD to `out`, whose first byte is at `base` in the block
  // being written (0 for a block written from its start), so the IFD is at
  // base + out.size(). Throws Error(dataTooLarge) past 4 GB.
  void write(Bytes& out, std::uint64_t base, std::uint32_t next, ByteOrder order) const;
};

}  // namespace lumenlib::detail

// Exif metadata: the TIFF tags of IFD0, the Exif IFD, the GPS IFD, the
// interoperability IFD and IFD1 (the thumbnail's).
//
// A tag is named by its IFD and its name in the TIFF, Exif or DNG
// specification:
//
//   ifd0.Make   exif.DateTimeOriginal   gps.GPSLatitude   interop.InteroperabilityIndex   ifd1.Compression
//
// The IFD may be left out when the name says it ("DateTimeOriginal" is
// exif.DateTimeOriginal, "Make" is ifd0.Make), and a tag without a name in the
// built-in table is written by number: "ifd0.0xabcd".
//
// The structural tags (the Exif, GPS and interoperability IFD pointers, and
// IFD1's JPEGInterchangeFormat/JPEGInterchangeFormatLength) are not data: they
// are written from the layout. The thumbnail is ExifMetadata::thumbnail().
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/field_value.hpp>
#include <lumenlib/types.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lumenlib {

class MakerNote;

enum class Ifd : std::uint8_t {
  ifd0,     // the primary image (TIFF's 0th IFD)
  exif,     // the Exif IFD
  gps,      // the GPS IFD
  interop,  // the interoperability IFD
  ifd1,     // the thumbnail (TIFF's 1st IFD)
};

// "ifd0", "exif", "gps", "interop" or "ifd1".
LUMENLIB_EXPORT const char* ifdName(Ifd ifd) noexcept;
LUMENLIB_EXPORT std::optional<Ifd> ifdFromName(std::string_view name) noexcept;

struct ExifTagSpec {
  std::uint16_t number;
  Ifd ifd;
  const char* name;
  FieldType type;      // the type written for a new value
  std::int32_t count;  // components expected, or -1 for any
};

// The tag's entry in the built-in table, or nullptr. IFD1 shares IFD0's.
LUMENLIB_EXPORT const ExifTagSpec* lookupExifTag(Ifd ifd, std::uint16_t number) noexcept;
LUMENLIB_EXPORT const ExifTagSpec* lookupExifTag(Ifd ifd, std::string_view name) noexcept;
// Every tag in the built-in table.
LUMENLIB_EXPORT const std::vector<ExifTagSpec>& exifTagTable();

class LUMENLIB_EXPORT ExifTag {
 public:
  constexpr ExifTag(Ifd ifd, std::uint16_t number) noexcept : ifd_(ifd), number_(number) {}
  // Parses "ifd.Name", "ifd.0xhhhh" or a bare "Name" (in the IFD the
  // specifications put it: the Exif, GPS, interoperability IFD, then IFD0).
  // Throws Error(invalidArgument).
  explicit ExifTag(std::string_view text);
  // As above, without throwing.
  static std::optional<ExifTag> parse(std::string_view text) noexcept;

  Ifd ifd() const noexcept { return ifd_; }
  std::uint16_t number() const noexcept { return number_; }
  // The tag's name, or "0xhhhh" for one without.
  std::string name() const;
  // "ifd.Name".
  std::string str() const;
  // The table entry, or nullptr for a tag without one.
  const ExifTagSpec* spec() const noexcept { return lookupExifTag(ifd_, number_); }

  friend bool operator==(const ExifTag& a, const ExifTag& b) noexcept {
    return a.ifd_ == b.ifd_ && a.number_ == b.number_;
  }
  friend bool operator!=(const ExifTag& a, const ExifTag& b) noexcept { return !(a == b); }
  friend bool operator<(const ExifTag& a, const ExifTag& b) noexcept {
    return a.ifd_ != b.ifd_ ? a.ifd_ < b.ifd_ : a.number_ < b.number_;
  }

 private:
  Ifd ifd_;
  std::uint16_t number_;
};

class LUMENLIB_EXPORT ExifEntry {
 public:
  explicit ExifEntry(ExifTag tag, FieldValue value = FieldValue());

  const ExifTag& tag() const noexcept { return tag_; }
  Ifd ifd() const noexcept { return tag_.ifd(); }
  std::uint16_t number() const noexcept { return tag_.number(); }
  std::string name() const { return tag_.name(); }

  const FieldValue& value() const noexcept { return value_; }
  void setValue(FieldValue value) { value_ = std::move(value); }
  // Text converted to the tag's type (from the table, or the current value's
  // for a tag without an entry) with FieldValue::parse. UserComment and the
  // GPS text tags get their character-code prefix. Throws
  // Error(invalidArgument) for text the type cannot hold.
  void setText(std::string_view text);
  // An integer or a rational in the tag's type.
  void setInt(std::int64_t value);
  void setRational(const Rational& value);

  FieldType type() const noexcept { return value_.type(); }
  std::size_t count() const noexcept { return value_.count(); }
  std::string text() const { return value_.text(); }
  std::int64_t asInt(std::size_t i = 0) const { return value_.asInt(i); }
  double asDouble(std::size_t i = 0) const { return value_.asDouble(i); }
  Rational asRational(std::size_t i = 0) const { return value_.asRational(i); }

  // The value as a person reads it: "1/250 s", "F2.8", "Rotated 90° clockwise",
  // "Flash fired, auto mode", "55° 40' 33.99\"". Tags without a special
  // rendering print as text() (long binary values are summarised).
  std::string describe() const;

 private:
  FieldType defaultType() const noexcept;

  ExifTag tag_;
  FieldValue value_;
};

namespace detail {
struct ExifOrigin;
struct MakerNoteOrigin;
struct ExifAccess;
}  // namespace detail

class LUMENLIB_EXPORT ExifMetadata {
 public:
  using iterator = std::vector<ExifEntry>::iterator;
  using const_iterator = std::vector<ExifEntry>::const_iterator;

  ExifMetadata();
  ~ExifMetadata();
  ExifMetadata(const ExifMetadata&);
  ExifMetadata(ExifMetadata&&) noexcept;
  ExifMetadata& operator=(const ExifMetadata&);
  ExifMetadata& operator=(ExifMetadata&&) noexcept;

  // The first entry with the tag, or nullptr (also for a malformed name).
  ExifEntry* find(const ExifTag& tag);
  const ExifEntry* find(const ExifTag& tag) const;
  ExifEntry* find(std::string_view tag);
  const ExifEntry* find(std::string_view tag) const;
  bool contains(std::string_view tag) const { return find(tag) != nullptr; }

  // The entry for the tag, added with an empty value of the tag's type if
  // there is none. Throws Error(invalidArgument) for a malformed name.
  ExifEntry& entry(const ExifTag& tag);
  ExifEntry& entry(std::string_view tag);

  // Replace the tag's value (adding it if absent, dropping duplicates).
  ExifEntry& set(const ExifTag& tag, FieldValue value);
  ExifEntry& set(std::string_view tag, FieldValue value);
  // As ExifEntry::setText / setInt / setRational on the tag's entry.
  ExifEntry& setText(std::string_view tag, std::string_view text);
  ExifEntry& setInt(std::string_view tag, std::int64_t value);
  ExifEntry& setRational(std::string_view tag, const Rational& value);

  // Adds an entry, even when the tag is already there.
  void append(ExifEntry entry);
  void append(const ExifTag& tag, FieldValue value) { append(ExifEntry(tag, std::move(value))); }

  iterator erase(iterator pos);
  // Removes every entry with the tag; returns how many there were.
  std::size_t remove(const ExifTag& tag);
  std::size_t remove(std::string_view tag);
  // Removes every entry the predicate is true for; returns how many.
  template <typename Predicate>
  std::size_t removeIf(Predicate predicate) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), predicate), entries_.end());
    return before - entries_.size();
  }
  // Every entry, the thumbnail and what was read.
  void clear();

  // Sorts by IFD, then tag number: the order they are written in.
  void sort();

  bool empty() const noexcept { return entries_.empty() && thumbnail_.empty(); }
  std::size_t size() const noexcept { return entries_.size(); }
  iterator begin() noexcept { return entries_.begin(); }
  iterator end() noexcept { return entries_.end(); }
  const_iterator begin() const noexcept { return entries_.begin(); }
  const_iterator end() const noexcept { return entries_.end(); }

  // The JPEG thumbnail in IFD1, if any.
  const Bytes& thumbnail() const noexcept { return thumbnail_; }
  // Sets the thumbnail (a JPEG); IFD1 gets Compression = 6 if it has none.
  void setThumbnail(Bytes jpeg);
  // Removes the thumbnail and every IFD1 tag.
  void removeThumbnail();

  // The byte order the data are written in (that of the block they came
  // from; little-endian for new data).
  ByteOrder byteOrder() const noexcept { return byteOrder_; }
  void setByteOrder(ByteOrder order) noexcept { byteOrder_ = order; }

  // The camera maker's note (exif.MakerNote) decoded, as it was read; nullptr
  // when there is none or its format is not known. Editing or removing
  // exif.MakerNote does not change it.
  const MakerNote* makerNote() const noexcept { return makerNote_.get(); }

  // The data as a TIFF structure: the payload of a JPEG's Exif APP1 segment
  // (after "Exif\0\0"), a PNG eXIf chunk or a HEIF Exif item. Empty when
  // there is nothing to write. Unchanged data, or data whose edited values
  // still fit where the old ones were, is written back as it was read;
  // otherwise it is laid out anew, and a maker note whose offsets count from
  // the TIFF header is corrected for where it moves.
  // Throws Error(dataTooLarge) past 4 GB.
  Bytes encode() const;
  // Parses a TIFF structure (with or without the "Exif\0\0" prefix). A
  // damaged IFD or entry is skipped; a missing or bad header throws
  // Error(corruptData).
  static ExifMetadata decode(const std::uint8_t* data, std::size_t size);
  static ExifMetadata decode(const Bytes& data) { return decode(data.data(), data.size()); }

 private:
  friend struct detail::ExifAccess;

  std::vector<ExifEntry> entries_;
  Bytes thumbnail_;
  ByteOrder byteOrder_ = ByteOrder::little;
  // The block the data were read from, so an unchanged or same-size edit is
  // written back byte for byte (keeping maker notes' internal offsets valid).
  std::shared_ptr<const detail::ExifOrigin> origin_;
  std::shared_ptr<const MakerNote> makerNote_;
  // Where the maker note was and how its offsets count, so it can be moved.
  std::shared_ptr<const detail::MakerNoteOrigin> makerNoteOrigin_;
};

}  // namespace lumenlib

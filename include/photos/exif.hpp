// Exif metadata: TIFF tags from IFD0 ("Exif.Image"), the Exif IFD
// ("Exif.Photo"), the GPS IFD ("Exif.GPSInfo"), the interoperability IFD
// ("Exif.Iop") and IFD1, the thumbnail's ("Exif.Thumbnail").
//
// Keys follow exiv2's naming, "Exif.<group>.<tag name>", so code written
// against exiv2 ports by changing types rather than strings. A tag this
// library has no name for is "Exif.<group>.0xhhhh".
//
// The structural tags (the Exif, GPS and interoperability IFD pointers, and
// IFD1's JPEGInterchangeFormat/JPEGInterchangeFormatLength) are not data: they
// are written from the layout. The thumbnail is ExifData::thumbnail().
#pragma once

#include <photos/export.hpp>
#include <photos/types.hpp>
#include <photos/value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace photos {

enum class IfdId : std::uint8_t {
  ifd0,     // Exif.Image
  exif,     // Exif.Photo
  gps,      // Exif.GPSInfo
  interop,  // Exif.Iop
  ifd1,     // Exif.Thumbnail
};

// "Image", "Photo", "GPSInfo", "Iop" or "Thumbnail".
PHOTOS_EXPORT const char* groupName(IfdId ifd) noexcept;

struct ExifTagInfo {
  std::uint16_t tag;
  IfdId ifd;
  const char* name;
  TypeId type;         // the type written for a new value
  std::int32_t count;  // components expected, or -1 for any
};

// The tag's entry in the built-in table, or nullptr.
PHOTOS_EXPORT const ExifTagInfo* findExifTag(IfdId ifd, std::uint16_t tag) noexcept;
PHOTOS_EXPORT const ExifTagInfo* findExifTag(IfdId ifd, std::string_view name) noexcept;
// Every tag in the built-in table.
PHOTOS_EXPORT const std::vector<ExifTagInfo>& exifTagList();

class PHOTOS_EXPORT ExifKey {
 public:
  ExifKey(IfdId ifd, std::uint16_t tag) noexcept : ifd_(ifd), tag_(tag) {}
  // Parses "Exif.<group>.<tag name or 0xhhhh>". Throws Error(invalidArgument).
  explicit ExifKey(std::string_view key);

  IfdId ifd() const noexcept { return ifd_; }
  std::uint16_t tag() const noexcept { return tag_; }
  std::string tagName() const;
  std::string str() const;
  // The table entry, or nullptr for a tag without one.
  const ExifTagInfo* info() const noexcept { return findExifTag(ifd_, tag_); }

  friend bool operator==(const ExifKey& a, const ExifKey& b) noexcept { return a.ifd_ == b.ifd_ && a.tag_ == b.tag_; }
  friend bool operator!=(const ExifKey& a, const ExifKey& b) noexcept { return !(a == b); }

 private:
  IfdId ifd_;
  std::uint16_t tag_;
};

class PHOTOS_EXPORT ExifDatum {
 public:
  explicit ExifDatum(ExifKey key, Value value = Value());

  const ExifKey& exifKey() const noexcept { return key_; }
  std::string key() const { return key_.str(); }
  IfdId ifd() const noexcept { return key_.ifd(); }
  std::uint16_t tag() const noexcept { return key_.tag(); }
  std::string tagName() const { return key_.tagName(); }
  std::string groupName() const { return photos::groupName(key_.ifd()); }

  const Value& value() const noexcept { return value_; }
  void setValue(Value value) { value_ = std::move(value); }

  // Assignment converts to the tag's type (from the table, or the current
  // value's type for an unknown tag). Text is parsed with Value::fromString.
  ExifDatum& operator=(std::string_view text);
  ExifDatum& operator=(const char* text) { return *this = std::string_view(text); }
  ExifDatum& operator=(const std::string& text) { return *this = std::string_view(text); }
  ExifDatum& operator=(std::uint16_t v);
  ExifDatum& operator=(std::uint32_t v);
  ExifDatum& operator=(std::int32_t v);
  ExifDatum& operator=(const Rational& v);
  ExifDatum& operator=(const Value& v) {
    value_ = v;
    return *this;
  }

  TypeId typeId() const noexcept { return value_.typeId(); }
  std::size_t count() const noexcept { return value_.count(); }
  std::string toString() const { return value_.toString(); }
  std::int64_t toInt64(std::size_t i = 0) const { return value_.toInt64(i); }
  double toDouble(std::size_t i = 0) const { return value_.toDouble(i); }
  Rational toRational(std::size_t i = 0) const { return value_.toRational(i); }

 private:
  TypeId defaultType() const noexcept;

  ExifKey key_;
  Value value_;
};

namespace detail {
struct ExifOrigin;
struct ExifAccess;
}  // namespace detail

class PHOTOS_EXPORT ExifData {
 public:
  using iterator = std::vector<ExifDatum>::iterator;
  using const_iterator = std::vector<ExifDatum>::const_iterator;

  // The datum for the key, added with an empty value if there is none.
  // Throws Error(invalidArgument) for a malformed key.
  ExifDatum& operator[](std::string_view key);

  void add(ExifDatum datum);
  void add(const ExifKey& key, Value value) { add(ExifDatum(key, std::move(value))); }

  iterator findKey(const ExifKey& key);
  const_iterator findKey(const ExifKey& key) const;
  // The datum for the key, or nullptr (also for a malformed key).
  ExifDatum* find(std::string_view key);
  const ExifDatum* find(std::string_view key) const;

  iterator erase(iterator pos);
  // Removes every datum with the key; returns how many there were.
  std::size_t erase(std::string_view key);
  void clear();

  // Sorts by group, then tag number: the order the data are written in.
  void sortByKey();

  bool empty() const noexcept { return data_.empty() && thumbnail_.empty(); }
  std::size_t size() const noexcept { return data_.size(); }
  iterator begin() noexcept { return data_.begin(); }
  iterator end() noexcept { return data_.end(); }
  const_iterator begin() const noexcept { return data_.begin(); }
  const_iterator end() const noexcept { return data_.end(); }

  // The JPEG thumbnail in IFD1, if any.
  const Bytes& thumbnail() const noexcept { return thumbnail_; }
  // Sets the thumbnail (a JPEG); IFD1 gets Compression = 6 if it has none.
  void setThumbnail(Bytes jpeg);
  // Removes the thumbnail and every Exif.Thumbnail tag.
  void eraseThumbnail();

  // The byte order the data are written in (that of the file they came
  // from; little-endian for new data).
  ByteOrder byteOrder() const noexcept { return byteOrder_; }
  void setByteOrder(ByteOrder order) noexcept { byteOrder_ = order; }

 private:
  friend struct detail::ExifAccess;

  std::vector<ExifDatum> data_;
  Bytes thumbnail_;
  ByteOrder byteOrder_ = ByteOrder::littleEndian;
  // The block the data were read from, so an unchanged or same-size edit is
  // written back byte for byte (keeping maker notes' internal offsets valid).
  std::shared_ptr<const detail::ExifOrigin> origin_;
};

}  // namespace photos

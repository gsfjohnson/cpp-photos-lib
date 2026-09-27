// IPTC-IIM metadata (the envelope record 1 and the application record 2), as
// stored in a Photoshop image resource block (JPEG APP13, TIFF) or a PNG raw
// profile.
//
// A dataset is named as in the IIM 4.2 specification, run together:
// "ObjectName", "Keywords", "CaptionAbstract", "ByLine", "CopyrightNotice",
// "CodedCharacterSet". The names are unique across the two records. A dataset
// without a name is written "record:dataset", such as "2:250".
//
// Values are UTF-8 text in the API. Data read with a non-UTF-8 character set
// is converted from ISO 8859-1; when written, CodedCharacterSet is set to
// UTF-8 if any value needs it. RecordVersion, ModelVersion and the other
// 2-byte binary datasets are shown and set as decimal text.
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/types.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lumenlib {

struct IptcDatasetSpec {
  std::uint8_t record;  // 1 envelope, 2 application
  std::uint8_t dataset;
  const char* name;
  bool repeatable;
  std::uint16_t maxBytes;  // the IIM limit; 0 when there is none
};

LUMENLIB_EXPORT const IptcDatasetSpec* lookupIptcDataset(std::uint8_t record, std::uint8_t dataset) noexcept;
LUMENLIB_EXPORT const IptcDatasetSpec* lookupIptcDataset(std::string_view name) noexcept;
LUMENLIB_EXPORT const std::vector<IptcDatasetSpec>& iptcDatasetTable();

class LUMENLIB_EXPORT IptcTag {
 public:
  constexpr IptcTag(std::uint8_t record, std::uint8_t dataset) noexcept : record_(record), dataset_(dataset) {}
  // Parses a dataset name or "record:dataset". Throws Error(invalidArgument).
  explicit IptcTag(std::string_view text);
  static std::optional<IptcTag> parse(std::string_view text) noexcept;

  std::uint8_t record() const noexcept { return record_; }
  std::uint8_t dataset() const noexcept { return dataset_; }
  // The name, or "record:dataset".
  std::string str() const;
  const IptcDatasetSpec* spec() const noexcept { return lookupIptcDataset(record_, dataset_); }

  friend bool operator==(const IptcTag& a, const IptcTag& b) noexcept {
    return a.record_ == b.record_ && a.dataset_ == b.dataset_;
  }
  friend bool operator!=(const IptcTag& a, const IptcTag& b) noexcept { return !(a == b); }

 private:
  std::uint8_t record_;
  std::uint8_t dataset_;
};

class LUMENLIB_EXPORT IptcEntry {
 public:
  IptcEntry(IptcTag tag, std::string value) : tag_(tag), value_(std::move(value)) {}

  const IptcTag& tag() const noexcept { return tag_; }
  std::uint8_t record() const noexcept { return tag_.record(); }
  std::uint8_t dataset() const noexcept { return tag_.dataset(); }
  std::string name() const { return tag_.str(); }

  const std::string& value() const noexcept { return value_; }
  void setValue(std::string value) { value_ = std::move(value); }

 private:
  IptcTag tag_;
  std::string value_;
};

class LUMENLIB_EXPORT IptcMetadata {
 public:
  using iterator = std::vector<IptcEntry>::iterator;
  using const_iterator = std::vector<IptcEntry>::const_iterator;

  // The first entry of the dataset, or nullptr (also for a malformed name).
  IptcEntry* find(const IptcTag& tag);
  const IptcEntry* find(const IptcTag& tag) const;
  IptcEntry* find(std::string_view tag);
  const IptcEntry* find(std::string_view tag) const;
  bool contains(std::string_view tag) const { return find(tag) != nullptr; }
  // The first value of the dataset, or std::nullopt.
  std::optional<std::string> value(std::string_view tag) const;
  // Every value of a (repeatable) dataset, in order.
  std::vector<std::string> values(std::string_view tag) const;

  // Replaces every value of the dataset with this one.
  void set(std::string_view tag, std::string value);
  // Replaces every value of the dataset; the new values go where the first
  // old one was. An empty list removes the dataset.
  void setValues(std::string_view tag, const std::vector<std::string>& values);
  // Adds a value, after any the dataset has.
  void append(const IptcTag& tag, std::string value);
  void append(std::string_view tag, std::string value) { append(IptcTag(tag), std::move(value)); }

  iterator erase(iterator pos) { return entries_.erase(pos); }
  // Removes every value of the dataset; returns how many there were.
  std::size_t remove(std::string_view tag);
  template <typename Predicate>
  std::size_t removeIf(Predicate predicate) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), predicate), entries_.end());
    return before - entries_.size();
  }
  void clear() noexcept { entries_.clear(); }
  // Sorts by record and dataset, keeping repeated datasets in order.
  void sort();

  bool empty() const noexcept { return entries_.empty(); }
  std::size_t size() const noexcept { return entries_.size(); }
  iterator begin() noexcept { return entries_.begin(); }
  iterator end() noexcept { return entries_.end(); }
  const_iterator begin() const noexcept { return entries_.begin(); }
  const_iterator end() const noexcept { return entries_.end(); }

  // IIM encoding and decoding (the payload of Photoshop resource 0x0404).
  // decode throws Error(corruptData); encode throws Error(invalidArgument)
  // for a binary dataset whose text is not a number in range.
  static IptcMetadata decode(const std::uint8_t* data, std::size_t size);
  Bytes encode() const;

 private:
  std::vector<IptcEntry> entries_;
};

}  // namespace lumenlib

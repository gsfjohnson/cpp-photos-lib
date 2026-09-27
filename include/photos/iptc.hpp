// IPTC-IIM metadata ("Iptc.Envelope.*" and "Iptc.Application2.*", exiv2's
// keys), as stored in a Photoshop image resource block (JPEG APP13, TIFF) or a
// PNG raw profile.
//
// Values are UTF-8 text in the API. Data read with a non-UTF-8 character set
// is converted from ISO 8859-1; when written, Iptc.Envelope.CharacterSet is
// set to UTF-8 if any value needs it. Iptc.Application2.RecordVersion and
// Iptc.Envelope.ModelVersion are 2-byte binary numbers, shown and set as
// decimal text.
#pragma once

#include <photos/export.hpp>
#include <photos/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photos {

struct IptcDataSetInfo {
  std::uint8_t record;  // 1 envelope, 2 application
  std::uint8_t dataset;
  const char* name;
  bool repeatable;
  std::uint16_t maxBytes;  // the IIM limit; 0 when there is none
};

PHOTOS_EXPORT const IptcDataSetInfo* findIptcDataSet(std::uint8_t record, std::uint8_t dataset) noexcept;
PHOTOS_EXPORT const IptcDataSetInfo* findIptcDataSet(std::uint8_t record, std::string_view name) noexcept;

class PHOTOS_EXPORT IptcKey {
 public:
  IptcKey(std::uint8_t record, std::uint8_t dataset) noexcept : record_(record), dataset_(dataset) {}
  // Parses "Iptc.<Envelope|Application2|0xhh>.<name or 0xhhhh>".
  // Throws Error(invalidArgument).
  explicit IptcKey(std::string_view key);

  std::uint8_t record() const noexcept { return record_; }
  std::uint8_t dataset() const noexcept { return dataset_; }
  std::string recordName() const;
  std::string tagName() const;
  std::string str() const;

  friend bool operator==(const IptcKey& a, const IptcKey& b) noexcept {
    return a.record_ == b.record_ && a.dataset_ == b.dataset_;
  }
  friend bool operator!=(const IptcKey& a, const IptcKey& b) noexcept { return !(a == b); }

 private:
  std::uint8_t record_;
  std::uint8_t dataset_;
};

class PHOTOS_EXPORT IptcDatum {
 public:
  IptcDatum(IptcKey key, std::string value) : key_(key), value_(std::move(value)) {}

  const IptcKey& iptcKey() const noexcept { return key_; }
  std::string key() const { return key_.str(); }
  std::uint8_t record() const noexcept { return key_.record(); }
  std::uint8_t dataset() const noexcept { return key_.dataset(); }
  std::string tagName() const { return key_.tagName(); }

  const std::string& toString() const noexcept { return value_; }
  void setValue(std::string value) { value_ = std::move(value); }
  IptcDatum& operator=(std::string value) {
    value_ = std::move(value);
    return *this;
  }

 private:
  IptcKey key_;
  std::string value_;
};

class PHOTOS_EXPORT IptcData {
 public:
  using iterator = std::vector<IptcDatum>::iterator;
  using const_iterator = std::vector<IptcDatum>::const_iterator;

  // The first datum for the key, added empty if there is none.
  IptcDatum& operator[](std::string_view key);
  void add(IptcDatum datum) { data_.push_back(std::move(datum)); }
  void add(std::string_view key, std::string value) { add(IptcDatum(IptcKey(key), std::move(value))); }

  IptcDatum* find(std::string_view key);
  const IptcDatum* find(std::string_view key) const;
  // Every value of a (repeatable) dataset, in order.
  std::vector<std::string> values(std::string_view key) const;
  // Replaces every value of the dataset.
  void setValues(std::string_view key, const std::vector<std::string>& values);

  iterator erase(iterator pos) { return data_.erase(pos); }
  std::size_t erase(std::string_view key);
  void clear() noexcept { data_.clear(); }
  // Sorts by record and dataset, keeping repeated datasets in order.
  void sortByKey();

  bool empty() const noexcept { return data_.empty(); }
  std::size_t size() const noexcept { return data_.size(); }
  iterator begin() noexcept { return data_.begin(); }
  iterator end() noexcept { return data_.end(); }
  const_iterator begin() const noexcept { return data_.begin(); }
  const_iterator end() const noexcept { return data_.end(); }

  // IIM encoding and decoding (the payload of Photoshop resource 0x0404).
  // decode throws Error(corruptData).
  static IptcData decode(const std::uint8_t* data, std::size_t size);
  Bytes encode() const;

 private:
  std::vector<IptcDatum> data_;
};

}  // namespace photos

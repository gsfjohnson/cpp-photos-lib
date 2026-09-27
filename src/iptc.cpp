#include <photos/error.hpp>
#include <photos/iptc.hpp>

#include "bytes.hpp"
#include "strings.hpp"

#include <algorithm>

namespace photos {
namespace {

// Record, dataset, exiv2's name, repeatable, IIM maximum length.
const IptcDataSetInfo kDataSets[] = {
    {1, 0, "ModelVersion", false, 2},
    {1, 5, "Destination", true, 1024},
    {1, 20, "FileFormat", false, 2},
    {1, 22, "FileVersion", false, 2},
    {1, 30, "ServiceId", false, 10},
    {1, 40, "EnvelopeNumber", false, 8},
    {1, 50, "ProductId", true, 32},
    {1, 60, "EnvelopePriority", false, 1},
    {1, 70, "DateSent", false, 8},
    {1, 80, "TimeSent", false, 11},
    {1, 90, "CharacterSet", false, 32},
    {1, 100, "UNO", false, 80},
    {1, 120, "ARMId", false, 2},
    {1, 122, "ARMVersion", false, 2},
    {2, 0, "RecordVersion", false, 2},
    {2, 3, "ObjectType", false, 67},
    {2, 4, "ObjectAttribute", true, 68},
    {2, 5, "ObjectName", false, 64},
    {2, 7, "EditStatus", false, 64},
    {2, 8, "EditorialUpdate", false, 2},
    {2, 10, "Urgency", false, 1},
    {2, 12, "Subject", true, 236},
    {2, 15, "Category", false, 3},
    {2, 20, "SuppCategory", true, 32},
    {2, 22, "FixtureId", false, 32},
    {2, 25, "Keywords", true, 64},
    {2, 26, "LocationCode", true, 3},
    {2, 27, "LocationName", true, 64},
    {2, 30, "ReleaseDate", false, 8},
    {2, 35, "ReleaseTime", false, 11},
    {2, 37, "ExpirationDate", false, 8},
    {2, 38, "ExpirationTime", false, 11},
    {2, 40, "SpecialInstructions", false, 256},
    {2, 42, "ActionAdvised", false, 2},
    {2, 45, "ReferenceService", true, 10},
    {2, 47, "ReferenceDate", true, 8},
    {2, 50, "ReferenceNumber", true, 8},
    {2, 55, "DateCreated", false, 8},
    {2, 60, "TimeCreated", false, 11},
    {2, 62, "DigitizationDate", false, 8},
    {2, 63, "DigitizationTime", false, 11},
    {2, 65, "Program", false, 32},
    {2, 70, "ProgramVersion", false, 10},
    {2, 75, "ObjectCycle", false, 1},
    {2, 80, "Byline", true, 32},
    {2, 85, "BylineTitle", true, 32},
    {2, 90, "City", false, 32},
    {2, 92, "SubLocation", false, 32},
    {2, 95, "ProvinceState", false, 32},
    {2, 100, "CountryCode", false, 3},
    {2, 101, "CountryName", false, 64},
    {2, 103, "TransmissionReference", false, 32},
    {2, 105, "Headline", false, 256},
    {2, 110, "Credit", false, 32},
    {2, 115, "Source", false, 32},
    {2, 116, "Copyright", false, 128},
    {2, 118, "Contact", true, 128},
    {2, 120, "Caption", false, 2000},
    {2, 122, "Writer", true, 32},
    {2, 125, "RasterizedCaption", false, 7360},
    {2, 130, "ImageType", false, 2},
    {2, 131, "ImageOrientation", false, 1},
    {2, 135, "Language", false, 3},
    {2, 150, "AudioType", false, 2},
    {2, 151, "AudioRate", false, 6},
    {2, 152, "AudioResolution", false, 2},
    {2, 153, "AudioDuration", false, 6},
    {2, 154, "AudioOutcue", false, 64},
    {2, 200, "PreviewFormat", false, 2},
    {2, 201, "PreviewVersion", false, 2},
    {2, 202, "Preview", false, 0},
};

const char* const kUtf8CharacterSet = "\x1b%G";

// Datasets stored as a 2-byte big-endian number.
bool isShortDataSet(std::uint8_t record, std::uint8_t dataset) {
  return (record == 1 && (dataset == 0 || dataset == 20 || dataset == 22 || dataset == 120 || dataset == 122)) ||
         (record == 2 && (dataset == 0 || dataset == 200 || dataset == 201));
}

bool isBinaryDataSet(std::uint8_t record, std::uint8_t dataset) { return record == 2 && dataset == 202; }

}  // namespace

const IptcDataSetInfo* findIptcDataSet(std::uint8_t record, std::uint8_t dataset) noexcept {
  for (const auto& d : kDataSets) {
    if (d.record == record && d.dataset == dataset) return &d;
  }
  return nullptr;
}

const IptcDataSetInfo* findIptcDataSet(std::uint8_t record, std::string_view name) noexcept {
  for (const auto& d : kDataSets) {
    if (d.record == record && name == d.name) return &d;
  }
  return nullptr;
}

// ---- IptcKey ------------------------------------------------------------------

IptcKey::IptcKey(std::string_view key) : record_(0), dataset_(0) {
  const auto bad = [&] { return Error(ErrorCode::invalidArgument, "invalid IPTC key '" + std::string(key) + "'"); };
  if (key.substr(0, 5) != "Iptc.") throw bad();
  const auto rest = key.substr(5);
  const auto dot = rest.find('.');
  if (dot == std::string_view::npos) throw bad();
  const auto rec = rest.substr(0, dot);
  const auto name = rest.substr(dot + 1);
  if (rec == "Envelope") {
    record_ = 1;
  } else if (rec == "Application2") {
    record_ = 2;
  } else if (auto n = detail::parseInt(rec); n && *n >= 0 && *n <= 255) {
    record_ = static_cast<std::uint8_t>(*n);
  } else {
    throw bad();
  }
  if (const auto* info = findIptcDataSet(record_, name)) {
    dataset_ = info->dataset;
    return;
  }
  if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
    if (auto n = detail::parseInt(name); n && *n >= 0 && *n <= 255) {
      dataset_ = static_cast<std::uint8_t>(*n);
      return;
    }
  }
  throw bad();
}

std::string IptcKey::recordName() const {
  if (record_ == 1) return "Envelope";
  if (record_ == 2) return "Application2";
  return detail::hex2(record_);
}

std::string IptcKey::tagName() const {
  if (const auto* info = findIptcDataSet(record_, dataset_)) return info->name;
  return detail::hex4(dataset_);
}

std::string IptcKey::str() const { return "Iptc." + recordName() + "." + tagName(); }

// ---- IptcData -----------------------------------------------------------------

IptcDatum& IptcData::operator[](std::string_view key) {
  const IptcKey k(key);
  for (auto& d : data_) {
    if (d.iptcKey() == k) return d;
  }
  data_.emplace_back(k, std::string());
  return data_.back();
}

IptcDatum* IptcData::find(std::string_view key) {
  try {
    const IptcKey k(key);
    for (auto& d : data_) {
      if (d.iptcKey() == k) return &d;
    }
  } catch (const Error&) {
  }
  return nullptr;
}

const IptcDatum* IptcData::find(std::string_view key) const { return const_cast<IptcData*>(this)->find(key); }

std::vector<std::string> IptcData::values(std::string_view key) const {
  const IptcKey k(key);
  std::vector<std::string> out;
  for (const auto& d : data_) {
    if (d.iptcKey() == k) out.push_back(d.toString());
  }
  return out;
}

void IptcData::setValues(std::string_view key, const std::vector<std::string>& values) {
  const IptcKey k(key);
  // New values go where the first old one was, keeping the order of the rest.
  auto first = std::find_if(data_.begin(), data_.end(), [&](const IptcDatum& d) { return d.iptcKey() == k; });
  std::ptrdiff_t at = first - data_.begin();
  erase(key);
  at = std::min<std::ptrdiff_t>(at, static_cast<std::ptrdiff_t>(data_.size()));
  for (const auto& v : values) {
    data_.insert(data_.begin() + at, IptcDatum(k, v));
    ++at;
  }
}

std::size_t IptcData::erase(std::string_view key) {
  const IptcKey k(key);
  const auto before = data_.size();
  data_.erase(std::remove_if(data_.begin(), data_.end(), [&](const IptcDatum& d) { return d.iptcKey() == k; }),
              data_.end());
  return before - data_.size();
}

void IptcData::sortByKey() {
  std::stable_sort(data_.begin(), data_.end(), [](const IptcDatum& a, const IptcDatum& b) {
    if (a.record() != b.record()) return a.record() < b.record();
    return a.dataset() < b.dataset();
  });
}

IptcData IptcData::decode(const std::uint8_t* data, std::size_t size) {
  struct Raw {
    std::uint8_t record, dataset;
    std::string value;
  };
  std::vector<Raw> raw;
  std::size_t pos = 0;
  while (pos < size) {
    if (data[pos] != 0x1c) {
      ++pos;  // padding
      continue;
    }
    if (!detail::inBounds(size, pos, 5)) detail::corrupt("truncated IPTC dataset header");
    const std::uint8_t record = data[pos + 1];
    const std::uint8_t dataset = data[pos + 2];
    std::uint64_t length = detail::getBe16(data + pos + 3);
    pos += 5;
    if (length & 0x8000) {
      const std::size_t lengthOfLength = length & 0x7fff;
      if (lengthOfLength == 0 || lengthOfLength > 4 || !detail::inBounds(size, pos, lengthOfLength)) {
        detail::corrupt("bad IPTC extended dataset length");
      }
      length = 0;
      for (std::size_t i = 0; i < lengthOfLength; ++i) length = (length << 8) | data[pos + i];
      pos += lengthOfLength;
    }
    if (!detail::inBounds(size, pos, length)) detail::corrupt("IPTC dataset runs past the end");
    raw.push_back({record, dataset, detail::toText(data + pos, static_cast<std::size_t>(length))});
    pos += static_cast<std::size_t>(length);
  }

  bool utf8 = false;
  for (const auto& r : raw) {
    if (r.record == 1 && r.dataset == 90 && r.value == kUtf8CharacterSet) utf8 = true;
  }
  IptcData out;
  for (auto& r : raw) {
    std::string value = std::move(r.value);
    if (isShortDataSet(r.record, r.dataset)) {
      if (value.size() == 2) {
        value = std::to_string(detail::getBe16(reinterpret_cast<const std::uint8_t*>(value.data())));
      }
    } else if (!isBinaryDataSet(r.record, r.dataset) && !(r.record == 1 && r.dataset == 90)) {
      if (!utf8 && !detail::isValidUtf8(value)) value = detail::latin1ToUtf8(value);
    }
    out.add(IptcDatum(IptcKey(r.record, r.dataset), std::move(value)));
  }
  return out;
}

Bytes IptcData::encode() const {
  if (data_.empty()) return {};
  IptcData copy = *this;
  bool needsUtf8 = false;
  bool hasRecord2 = false;
  for (const auto& d : copy.data_) {
    if (d.record() == 2) hasRecord2 = true;
    if (!isBinaryDataSet(d.record(), d.dataset()) && !detail::isAscii(d.toString())) needsUtf8 = true;
  }
  if (needsUtf8) copy["Iptc.Envelope.CharacterSet"] = kUtf8CharacterSet;
  if (hasRecord2 && !copy.find("Iptc.Application2.RecordVersion")) copy["Iptc.Application2.RecordVersion"] = "4";
  copy.sortByKey();

  Bytes out;
  for (const auto& d : copy.data_) {
    std::string value = d.toString();
    if (isShortDataSet(d.record(), d.dataset())) {
      const auto n = detail::parseInt(value);
      if (!n || *n < 0 || *n > 0xffff) {
        throw Error(ErrorCode::invalidArgument, d.key() + " must be a number from 0 to 65535");
      }
      value.assign({static_cast<char>((*n >> 8) & 0xff), static_cast<char>(*n & 0xff)});
    }
    out.push_back(0x1c);
    out.push_back(d.record());
    out.push_back(d.dataset());
    if (value.size() < 0x8000) {
      detail::appendBe16(out, static_cast<std::uint16_t>(value.size()));
    } else {
      if (!detail::fits32(value.size())) throw Error(ErrorCode::dataTooLarge, d.key() + " is too large");
      detail::appendBe16(out, 0x8004);
      detail::appendBe32(out, static_cast<std::uint32_t>(value.size()));
    }
    detail::append(out, value);
  }
  return out;
}

}  // namespace photos

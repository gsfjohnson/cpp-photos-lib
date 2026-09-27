#include <lumenlib/error.hpp>
#include <lumenlib/iptc.hpp>

#include "bytes.hpp"
#include "strings.hpp"

#include <algorithm>

namespace lumenlib {
namespace {

// Record, dataset, the IIM 4.2 name run together, repeatable, IIM maximum
// length.
const std::vector<IptcDatasetSpec> kDatasets = {
    {1, 0, "ModelVersion", false, 2},
    {1, 5, "Destination", true, 1024},
    {1, 20, "FileFormat", false, 2},
    {1, 22, "FileFormatVersion", false, 2},
    {1, 30, "ServiceIdentifier", false, 10},
    {1, 40, "EnvelopeNumber", false, 8},
    {1, 50, "ProductID", true, 32},
    {1, 60, "EnvelopePriority", false, 1},
    {1, 70, "DateSent", false, 8},
    {1, 80, "TimeSent", false, 11},
    {1, 90, "CodedCharacterSet", false, 32},
    {1, 100, "UniqueNameOfObject", false, 80},
    {1, 120, "ARMIdentifier", false, 2},
    {1, 122, "ARMVersion", false, 2},
    {2, 0, "RecordVersion", false, 2},
    {2, 3, "ObjectTypeReference", false, 67},
    {2, 4, "ObjectAttributeReference", true, 68},
    {2, 5, "ObjectName", false, 64},
    {2, 7, "EditStatus", false, 64},
    {2, 8, "EditorialUpdate", false, 2},
    {2, 10, "Urgency", false, 1},
    {2, 12, "SubjectReference", true, 236},
    {2, 15, "Category", false, 3},
    {2, 20, "SupplementalCategory", true, 32},
    {2, 22, "FixtureIdentifier", false, 32},
    {2, 25, "Keywords", true, 64},
    {2, 26, "ContentLocationCode", true, 3},
    {2, 27, "ContentLocationName", true, 64},
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
    {2, 62, "DigitalCreationDate", false, 8},
    {2, 63, "DigitalCreationTime", false, 11},
    {2, 65, "OriginatingProgram", false, 32},
    {2, 70, "ProgramVersion", false, 10},
    {2, 75, "ObjectCycle", false, 1},
    {2, 80, "ByLine", true, 32},
    {2, 85, "ByLineTitle", true, 32},
    {2, 90, "City", false, 32},
    {2, 92, "SubLocation", false, 32},
    {2, 95, "ProvinceState", false, 32},
    {2, 100, "CountryPrimaryLocationCode", false, 3},
    {2, 101, "CountryPrimaryLocationName", false, 64},
    {2, 103, "OriginalTransmissionReference", false, 32},
    {2, 105, "Headline", false, 256},
    {2, 110, "Credit", false, 32},
    {2, 115, "Source", false, 32},
    {2, 116, "CopyrightNotice", false, 128},
    {2, 118, "Contact", true, 128},
    {2, 120, "CaptionAbstract", false, 2000},
    {2, 122, "WriterEditor", true, 32},
    {2, 125, "RasterizedCaption", false, 7360},
    {2, 130, "ImageType", false, 2},
    {2, 131, "ImageOrientation", false, 1},
    {2, 135, "LanguageIdentifier", false, 3},
    {2, 150, "AudioType", false, 2},
    {2, 151, "AudioSamplingRate", false, 6},
    {2, 152, "AudioSamplingResolution", false, 2},
    {2, 153, "AudioDuration", false, 6},
    {2, 154, "AudioOutcue", false, 64},
    {2, 200, "ObjectDataPreviewFileFormat", false, 2},
    {2, 201, "ObjectDataPreviewFileFormatVersion", false, 2},
    {2, 202, "ObjectDataPreviewData", false, 0},
};

const char* const kUtf8CharacterSet = "\x1b%G";
constexpr IptcTag kCodedCharacterSet(1, 90);
constexpr IptcTag kRecordVersion(2, 0);

// Datasets stored as a 2-byte big-endian number.
bool isShortDataset(std::uint8_t record, std::uint8_t dataset) {
  return (record == 1 && (dataset == 0 || dataset == 20 || dataset == 22 || dataset == 120 || dataset == 122)) ||
         (record == 2 && (dataset == 0 || dataset == 200 || dataset == 201));
}

bool isBinaryDataset(std::uint8_t record, std::uint8_t dataset) { return record == 2 && dataset == 202; }

}  // namespace

const std::vector<IptcDatasetSpec>& iptcDatasetTable() { return kDatasets; }

const IptcDatasetSpec* lookupIptcDataset(std::uint8_t record, std::uint8_t dataset) noexcept {
  for (const auto& d : kDatasets) {
    if (d.record == record && d.dataset == dataset) return &d;
  }
  return nullptr;
}

const IptcDatasetSpec* lookupIptcDataset(std::string_view name) noexcept {
  for (const auto& d : kDatasets) {
    if (name == d.name) return &d;
  }
  return nullptr;
}

// ---- IptcTag ------------------------------------------------------------------

std::optional<IptcTag> IptcTag::parse(std::string_view text) noexcept {
  if (const auto* spec = lookupIptcDataset(text)) return IptcTag(spec->record, spec->dataset);
  const auto colon = text.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const auto record = detail::parseInt(text.substr(0, colon));
  const auto dataset = detail::parseInt(text.substr(colon + 1));
  if (!record || !dataset || *record < 0 || *record > 255 || *dataset < 0 || *dataset > 255) return std::nullopt;
  return IptcTag(static_cast<std::uint8_t>(*record), static_cast<std::uint8_t>(*dataset));
}

IptcTag::IptcTag(std::string_view text) : record_(0), dataset_(0) {
  const auto tag = parse(text);
  if (!tag) throw Error(ErrorCode::invalidArgument, "invalid IPTC dataset '" + std::string(text) + "'");
  *this = *tag;
}

std::string IptcTag::str() const {
  if (const auto* s = spec()) return s->name;
  return std::to_string(record_) + ":" + std::to_string(dataset_);
}

// ---- IptcMetadata -------------------------------------------------------------

IptcEntry* IptcMetadata::find(const IptcTag& tag) {
  for (auto& e : entries_) {
    if (e.tag() == tag) return &e;
  }
  return nullptr;
}

const IptcEntry* IptcMetadata::find(const IptcTag& tag) const { return const_cast<IptcMetadata*>(this)->find(tag); }

IptcEntry* IptcMetadata::find(std::string_view tag) {
  const auto t = IptcTag::parse(tag);
  return t ? find(*t) : nullptr;
}

const IptcEntry* IptcMetadata::find(std::string_view tag) const { return const_cast<IptcMetadata*>(this)->find(tag); }

std::optional<std::string> IptcMetadata::value(std::string_view tag) const {
  const auto* e = find(tag);
  if (!e) return std::nullopt;
  return e->value();
}

std::vector<std::string> IptcMetadata::values(std::string_view tag) const {
  const IptcTag t(tag);
  std::vector<std::string> out;
  for (const auto& e : entries_) {
    if (e.tag() == t) out.push_back(e.value());
  }
  return out;
}

void IptcMetadata::set(std::string_view tag, std::string value) { setValues(tag, {std::move(value)}); }

void IptcMetadata::setValues(std::string_view tag, const std::vector<std::string>& values) {
  const IptcTag t(tag);
  // New values go where the first old one was, keeping the order of the rest.
  auto first = std::find_if(entries_.begin(), entries_.end(), [&](const IptcEntry& e) { return e.tag() == t; });
  std::ptrdiff_t at = first - entries_.begin();
  removeIf([&](const IptcEntry& e) { return e.tag() == t; });
  at = std::min<std::ptrdiff_t>(at, static_cast<std::ptrdiff_t>(entries_.size()));
  for (const auto& v : values) {
    entries_.insert(entries_.begin() + at, IptcEntry(t, v));
    ++at;
  }
}

void IptcMetadata::append(const IptcTag& tag, std::string value) {
  // After the dataset's last value, or at the end.
  auto last = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->tag() == tag) last = it + 1;
  }
  entries_.insert(last, IptcEntry(tag, std::move(value)));
}

std::size_t IptcMetadata::remove(std::string_view tag) {
  const IptcTag t(tag);
  return removeIf([&](const IptcEntry& e) { return e.tag() == t; });
}

void IptcMetadata::sort() {
  std::stable_sort(entries_.begin(), entries_.end(), [](const IptcEntry& a, const IptcEntry& b) {
    if (a.record() != b.record()) return a.record() < b.record();
    return a.dataset() < b.dataset();
  });
}

IptcMetadata IptcMetadata::decode(const std::uint8_t* data, std::size_t size) {
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
  IptcMetadata out;
  for (auto& r : raw) {
    std::string value = std::move(r.value);
    if (isShortDataset(r.record, r.dataset)) {
      if (value.size() == 2) {
        value = std::to_string(detail::getBe16(reinterpret_cast<const std::uint8_t*>(value.data())));
      }
    } else if (!isBinaryDataset(r.record, r.dataset) && !(r.record == 1 && r.dataset == 90)) {
      if (!utf8 && !detail::isValidUtf8(value)) value = detail::latin1ToUtf8(value);
    }
    out.entries_.emplace_back(IptcTag(r.record, r.dataset), std::move(value));
  }
  return out;
}

Bytes IptcMetadata::encode() const {
  if (entries_.empty()) return {};
  IptcMetadata copy = *this;
  bool needsUtf8 = false;
  bool hasRecord2 = false;
  for (const auto& e : copy.entries_) {
    if (e.record() == 2) hasRecord2 = true;
    if (!isBinaryDataset(e.record(), e.dataset()) && !detail::isAscii(e.value())) needsUtf8 = true;
  }
  if (needsUtf8) {
    copy.removeIf([](const IptcEntry& e) { return e.tag() == kCodedCharacterSet; });
    copy.entries_.emplace_back(kCodedCharacterSet, kUtf8CharacterSet);
  }
  if (hasRecord2 && !copy.find(kRecordVersion)) copy.entries_.emplace_back(kRecordVersion, "4");
  copy.sort();

  Bytes out;
  for (const auto& e : copy.entries_) {
    std::string value = e.value();
    if (isShortDataset(e.record(), e.dataset())) {
      const auto n = detail::parseInt(value);
      if (!n || *n < 0 || *n > 0xffff) {
        throw Error(ErrorCode::invalidArgument, e.name() + " must be a number from 0 to 65535");
      }
      value.assign({static_cast<char>((*n >> 8) & 0xff), static_cast<char>(*n & 0xff)});
    }
    out.push_back(0x1c);
    out.push_back(e.record());
    out.push_back(e.dataset());
    if (value.size() < 0x8000) {
      detail::appendBe16(out, static_cast<std::uint16_t>(value.size()));
    } else {
      if (!detail::fits32(value.size())) throw Error(ErrorCode::dataTooLarge, e.name() + " is too large");
      detail::appendBe16(out, 0x8004);
      detail::appendBe32(out, static_cast<std::uint32_t>(value.size()));
    }
    detail::append(out, value);
  }
  return out;
}

}  // namespace lumenlib

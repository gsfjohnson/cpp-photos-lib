// Maker notes: which format a note is in, its entries, and the lens it names.
//
// Every format here is an IFD after a signature. What differs is where the
// IFD starts, its byte order and what its offsets count from; for the
// formats where that has varied between cameras, the base that puts the
// entries' values inside the note wins.
#include <lumenlib/error.hpp>
#include <lumenlib/makernote.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"
#include "makernote_internal.hpp"
#include "strings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace lumenlib {
namespace {

using detail::MakerNoteBase;
using detail::MakerNoteLayout;

constexpr std::uint32_t kMaxEntries = 1000;
constexpr std::uint64_t kMaxValue = 1u << 20;  // maker note values are small
constexpr int kMaxDepth = 2;

struct Name {
  std::uint16_t number;
  const char* name;
};

// The tags each group names; the rest print by number.
const Name kCanon[] = {
    {0x0001, "CameraSettings"},       {0x0002, "FocalLength"},     {0x0004, "ShotInfo"},       {0x0006, "ImageType"},
    {0x0007, "FirmwareVersion"},      {0x0008, "FileNumber"},      {0x0009, "OwnerName"},      {0x000c, "SerialNumber"},
    {0x000d, "CameraInfo"},           {0x0010, "ModelID"},         {0x0026, "AFInfo2"},        {0x0095, "LensModel"},
    {0x0096, "InternalSerialNumber"}, {0x0097, "DustRemovalData"}, {0x00a0, "ProcessingInfo"}, {0x00b4, "ColorSpace"},
    {0x00e0, "SensorInfo"},           {0x4001, "ColorData"}};
const Name kNikon[] = {{0x0001, "MakerNoteVersion"},
                       {0x0002, "ISO"},
                       {0x0003, "ColorMode"},
                       {0x0004, "Quality"},
                       {0x0005, "WhiteBalance"},
                       {0x0007, "FocusMode"},
                       {0x0008, "FlashSetting"},
                       {0x000b, "WhiteBalanceFineTune"},
                       {0x0011, "PreviewIFD"},
                       {0x0012, "FlashExposureComp"},
                       {0x001d, "SerialNumber"},
                       {0x0022, "ActiveDLighting"},
                       {0x0083, "LensType"},
                       {0x0084, "Lens"},
                       {0x0085, "ManualFocusDistance"},
                       {0x0086, "DigitalZoom"},
                       {0x0088, "AFInfo"},
                       {0x0089, "ShootingMode"},
                       {0x008b, "LensFStops"},
                       {0x0098, "LensData"},
                       {0x00a7, "ShutterCount"}};
const Name kSony[] = {{0xb000, "FileFormat"},
                      {0xb001, "SonyModelID"},
                      {0xb020, "CreativeStyle"},
                      {0xb027, "LensType"},
                      {0xb02a, "LensSpec"}};
const Name kOlympus[] = {{0x0200, "SpecialMode"},    {0x0207, "CameraType"},     {0x2010, "Equipment"},
                         {0x2020, "CameraSettings"}, {0x2030, "RawDevelopment"}, {0x2040, "ImageProcessing"},
                         {0x2050, "FocusInfo"}};
const Name kOlympusEquipment[] = {{0x0000, "EquipmentVersion"},
                                  {0x0100, "CameraType"},
                                  {0x0101, "SerialNumber"},
                                  {0x0201, "LensType"},
                                  {0x0202, "LensSerialNumber"},
                                  {0x0203, "LensModel"},
                                  {0x0204, "LensFirmwareVersion"},
                                  {0x0205, "MaxApertureAtMinFocal"},
                                  {0x0206, "MaxApertureAtMaxFocal"},
                                  {0x0207, "MinFocalLength"},
                                  {0x0208, "MaxFocalLength"},
                                  {0x020a, "MaxAperture"},
                                  {0x1000, "FlashType"}};
const Name kPanasonic[] = {{0x0001, "ImageQuality"}, {0x0002, "FirmwareVersion"},    {0x0003, "WhiteBalance"},
                           {0x0007, "FocusMode"},    {0x001a, "ImageStabilization"}, {0x0025, "InternalSerialNumber"},
                           {0x0051, "LensType"},     {0x0052, "LensSerialNumber"},   {0x0053, "AccessoryType"}};
const Name kPentax[] = {
    {0x0000, "PentaxVersion"}, {0x0005, "PentaxModelID"}, {0x003f, "LensType"}, {0x0229, "SerialNumber"}};
const Name kFujifilm[] = {
    {0x0000, "Version"},        {0x0010, "InternalSerialNumber"},  {0x1000, "Quality"},
    {0x1001, "Sharpness"},      {0x1002, "WhiteBalance"},          {0x1404, "MinFocalLength"},
    {0x1405, "MaxFocalLength"}, {0x1406, "MaxApertureAtMinFocal"}, {0x1407, "MaxApertureAtMaxFocal"}};
const Name kSamsung[] = {
    {0x0001, "MakerNoteVersion"}, {0x0002, "DeviceType"}, {0x0003, "SamsungModelID"}, {0xa003, "LensType"}};
const Name kApple[] = {{0x0001, "MakerNoteVersion"}, {0x0008, "AccelerationVector"}, {0x0011, "ContentIdentifier"}};

template <std::size_t N>
const char* lookup(const Name (&names)[N], std::uint16_t number) {
  for (const auto& n : names) {
    if (n.number == number) return n.name;
  }
  return nullptr;
}

std::string tagName(const std::string& group, std::uint16_t number) {
  const char* name = nullptr;
  if (group == "canon")
    name = lookup(kCanon, number);
  else if (group == "nikon")
    name = lookup(kNikon, number);
  else if (group == "sony")
    name = lookup(kSony, number);
  else if (group == "olympus")
    name = lookup(kOlympus, number);
  else if (group == "olympus.equipment")
    name = lookup(kOlympusEquipment, number);
  else if (group == "panasonic")
    name = lookup(kPanasonic, number);
  else if (group == "pentax")
    name = lookup(kPentax, number);
  else if (group == "fujifilm")
    name = lookup(kFujifilm, number);
  else if (group == "samsung")
    name = lookup(kSamsung, number);
  else if (group == "apple")
    name = lookup(kApple, number);
  return name ? name : detail::hex4(number);
}

const char* groupOf(MakerNoteFormat format) {
  switch (format) {
    case MakerNoteFormat::canon:
      return "canon";
    case MakerNoteFormat::nikon:
    case MakerNoteFormat::nikonOld:
      return "nikon";
    case MakerNoteFormat::sony:
      return "sony";
    case MakerNoteFormat::olympus:
    case MakerNoteFormat::olympusOld:
      return "olympus";
    case MakerNoteFormat::panasonic:
      return "panasonic";
    case MakerNoteFormat::pentax:
      return "pentax";
    case MakerNoteFormat::fujifilm:
      return "fujifilm";
    case MakerNoteFormat::samsung:
      return "samsung";
    case MakerNoteFormat::apple:
      return "apple";
  }
  return "makernote";
}

// Olympus keeps groups of settings in IFDs of their own.
const char* olympusSubGroup(std::uint16_t number) {
  switch (number) {
    case 0x2010:
      return "olympus.equipment";
    case 0x2020:
      return "olympus.camera";
    case 0x2030:
      return "olympus.rawdev";
    case 0x2040:
      return "olympus.imageproc";
    case 0x2050:
      return "olympus.focus";
    default:
      return nullptr;
  }
}

bool startsWithCaseless(const std::string& text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const auto a = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    const auto b = static_cast<char>(std::toupper(static_cast<unsigned char>(prefix[i])));
    if (a != b) return false;
  }
  return true;
}

std::uint64_t baseOf(const MakerNoteLayout& layout, std::uint64_t noteOffset) {
  switch (layout.base) {
    case MakerNoteBase::note:
      return noteOffset;
    case MakerNoteBase::tiff:
      return 0;
    case MakerNoteBase::own:
      return noteOffset + layout.ownHeader;
  }
  return noteOffset;
}

// A plausible IFD at `at`: its entry count, or 0.
std::uint32_t entryCount(const InputSource& src, std::uint64_t at, std::uint64_t limit, ByteOrder order) {
  if (!src.contains(at, 2) || at + 2 > limit) return 0;
  std::uint8_t b[2];
  src.read(at, b, 2);
  const std::uint32_t n = detail::get16(b, order);
  if (n == 0 || n > kMaxEntries || at + 2 + 12ull * n > limit) return 0;
  return n;
}

// How many of the IFD's out-of-line values the base puts inside the note.
int fit(const InputSource& src, std::uint64_t noteOffset, std::size_t noteSize, const MakerNoteLayout& layout) {
  const std::uint64_t at = noteOffset + layout.ifd;
  const std::uint64_t end = noteOffset + noteSize;
  const std::uint32_t n = entryCount(src, at, end, layout.order);
  if (n == 0) return -1;
  const Bytes entries = src.readBytes(at + 2, 12 * static_cast<std::size_t>(n));
  const std::uint64_t base = baseOf(layout, noteOffset);
  int inside = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint8_t* e = entries.data() + 12 * static_cast<std::size_t>(i);
    const std::uint16_t type = detail::get16(e + 2, layout.order);
    if (!isFieldType(type)) continue;
    const std::uint64_t bytes = std::uint64_t{detail::get32(e + 4, layout.order)} * fieldTypeSize(FieldType(type));
    if (bytes <= 4) continue;
    const std::uint64_t valueAt = base + detail::get32(e + 8, layout.order);
    inside += (valueAt >= noteOffset && valueAt + bytes <= end) ? 1 : -1;
  }
  return inside;
}

class Walker {
 public:
  Walker(const InputSource& src, std::uint64_t noteOffset, const MakerNoteLayout& layout)
      : src_(src), layout_(layout), base_(baseOf(layout, noteOffset)) {}

  void ifd(std::uint64_t at, const std::string& group, int depth) {
    if (depth > kMaxDepth || !visited_.insert(at).second) return;
    const auto order = layout_.order;
    const std::uint32_t n = entryCount(src_, at, src_.size(), order);
    if (n == 0) return;
    const Bytes entries = src_.readBytes(at + 2, 12 * static_cast<std::size_t>(n));
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint8_t* e = entries.data() + 12 * static_cast<std::size_t>(i);
      const std::uint16_t number = detail::get16(e, order);
      const std::uint16_t type = detail::get16(e + 2, order);
      const std::uint32_t count = detail::get32(e + 4, order);
      if (!isFieldType(type)) continue;
      const auto fieldType = static_cast<FieldType>(type);
      const std::uint64_t bytes = std::uint64_t{count} * fieldTypeSize(fieldType);
      if (bytes > kMaxValue) continue;
      const std::uint64_t entryAt = at + 2 + 12ull * i;
      const std::uint64_t valueAt = bytes <= 4 ? entryAt + 8 : base_ + detail::get32(e + 8, order);
      if (!src_.contains(valueAt, bytes)) continue;
      const Bytes raw = src_.readBytes(valueAt, static_cast<std::size_t>(bytes));
      FieldValue value = FieldValue::decode(fieldType, raw.data(), raw.size(), count, order);

      if (group == "olympus") {
        if (const char* sub = olympusSubGroup(number)) {
          if ((fieldType == FieldType::ifd || fieldType == FieldType::u32) && count == 1) {
            ifd(base_ + static_cast<std::uint64_t>(value.asInt()), sub, depth + 1);
            continue;
          }
          if (fieldType == FieldType::undefined && bytes > 4) {
            ifd(valueAt, sub, depth + 1);  // an IFD stored as the value
            continue;
          }
        }
      }
      entries_.push_back({group, number, tagName(group, number), std::move(value)});
    }
  }

  std::vector<MakerNoteEntry> release() { return std::move(entries_); }

 private:
  const InputSource& src_;
  const MakerNoteLayout& layout_;
  std::uint64_t base_;
  std::set<std::uint64_t> visited_;
  std::vector<MakerNoteEntry> entries_;
};

// ---- Lens ---------------------------------------------------------------------

std::optional<std::string> cleanLensText(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
  std::size_t start = 0;
  while (start < text.size() && text[start] == ' ') ++start;
  text.erase(0, start);
  if (text.empty() || text.front() == '(' || startsWithCaseless(text, "unknown") || startsWithCaseless(text, "n/a")) {
    return std::nullopt;
  }
  if (text.find_first_not_of("-0123456789 ") == std::string::npos) return std::nullopt;
  return text;
}

std::string number(double v) {
  char b[32];
  if (std::fabs(v - std::round(v)) < 0.05) {
    std::snprintf(b, sizeof b, "%.0f", v);
  } else {
    std::snprintf(b, sizeof b, "%.1f", v);
  }
  return b;
}

// "18-55mm F3.5-5.6", "50mm F1.8", "18-55mm".
std::optional<std::string> lensRange(double minFocal, double maxFocal, double minFocalAperture,
                                     double maxFocalAperture) {
  if (!(minFocal > 0) || !(maxFocal >= minFocal) || maxFocal > 10000) return std::nullopt;
  std::string s = number(minFocal);
  if (number(maxFocal) != s) s += "-" + number(maxFocal);
  s += "mm";
  if (minFocalAperture > 0 && minFocalAperture < 100) {
    s += " F" + number(minFocalAperture);
    if (maxFocalAperture > 0 && maxFocalAperture < 100 && number(maxFocalAperture) != number(minFocalAperture)) {
      s += "-" + number(maxFocalAperture);
    }
  }
  return s;
}

std::optional<std::string> rangeOfRationals(const FieldValue& v) {
  if (v.count() < 4 || (v.type() != FieldType::urational && v.type() != FieldType::srational)) return std::nullopt;
  return lensRange(v.asDouble(0), v.asDouble(1), v.asDouble(2), v.asDouble(3));
}

// Sony's LensSpec: flags, the focal range and the apertures as BCD.
std::optional<std::string> sonyLensSpec(const FieldValue& v) {
  const Bytes& b = v.bytes();
  if (b.size() != 8) return std::nullopt;
  const auto bcd = [&](std::initializer_list<std::uint8_t> bytes, bool& ok) {
    int n = 0;
    for (auto byte : bytes) {
      const int hi = byte >> 4, lo = byte & 0xf;
      if (hi > 9 || lo > 9) ok = false;
      n = n * 100 + hi * 10 + lo;
    }
    return n;
  };
  bool ok = true;
  const int minFocal = bcd({b[1], b[2]}, ok);
  const int maxFocal = bcd({b[3], b[4]}, ok);
  const int minAperture = bcd({b[5]}, ok);
  const int maxAperture = bcd({b[6]}, ok);
  if (!ok) return std::nullopt;
  return lensRange(minFocal, maxFocal, minAperture / 10.0, maxAperture / 10.0);
}

}  // namespace

const char* makerNoteFormatName(MakerNoteFormat format) noexcept {
  switch (format) {
    case MakerNoteFormat::canon:
      return "Canon";
    case MakerNoteFormat::nikon:
      return "Nikon";
    case MakerNoteFormat::nikonOld:
      return "Nikon (type 1)";
    case MakerNoteFormat::sony:
      return "Sony";
    case MakerNoteFormat::olympus:
      return "Olympus";
    case MakerNoteFormat::olympusOld:
      return "Olympus (old)";
    case MakerNoteFormat::panasonic:
      return "Panasonic";
    case MakerNoteFormat::pentax:
      return "Pentax";
    case MakerNoteFormat::fujifilm:
      return "Fujifilm";
    case MakerNoteFormat::samsung:
      return "Samsung";
    case MakerNoteFormat::apple:
      return "Apple";
  }
  return "unknown";
}

const MakerNoteEntry* MakerNote::find(std::string_view name) const {
  const auto dot = name.rfind('.');
  for (const auto& e : entries_) {
    if (dot == std::string_view::npos) {
      if (e.name == name) return &e;
    } else if (name.substr(0, dot) == e.group && name.substr(dot + 1) == e.name) {
      return &e;
    }
  }
  return nullptr;
}

std::optional<std::string> lensDescription(const ExifMetadata& exif) {
  if (const auto* e = exif.find(ExifTag(Ifd::exif, 0xa434)); e && e->type() == FieldType::ascii) {
    if (auto text = cleanLensText(e->text())) return text;
  }
  const MakerNote* note = exif.makerNote();
  if (note) {
    for (const char* name : {"canon.LensModel", "olympus.equipment.LensModel", "panasonic.LensType"}) {
      const auto* e = note->find(name);
      if (e && e->value.type() == FieldType::ascii) {
        if (auto text = cleanLensText(e->value.text())) return text;
      }
    }
    if (const auto* e = note->find("nikon.Lens")) {
      if (auto r = rangeOfRationals(e->value)) return r;
    }
    if (const auto* e = note->find("sony.LensSpec")) {
      if (auto r = sonyLensSpec(e->value)) return r;
    }
    const auto* minF = note->find("fujifilm.MinFocalLength");
    const auto* maxF = note->find("fujifilm.MaxFocalLength");
    if (minF && maxF && minF->value.count() && maxF->value.count()) {
      const auto* minA = note->find("fujifilm.MaxApertureAtMinFocal");
      const auto* maxA = note->find("fujifilm.MaxApertureAtMaxFocal");
      if (auto r = lensRange(minF->value.asDouble(), maxF->value.asDouble(),
                             minA && minA->value.count() ? minA->value.asDouble() : 0,
                             maxA && maxA->value.count() ? maxA->value.asDouble() : 0)) {
        return r;
      }
    }
  }
  if (const auto* e = exif.find(ExifTag(Ifd::exif, 0xa432))) {
    if (auto r = rangeOfRationals(e->value())) return r;
  }
  if (const auto* e = exif.find(ExifTag(Ifd::ifd0, 0xc630))) {
    if (auto r = rangeOfRationals(e->value())) return r;
  }
  if (note) {
    // Canon's camera settings give the focal range in focal units.
    if (const auto* e = note->find("canon.CameraSettings"); e && e->value.count() > 25) {
      const double units = e->value.asInt(25) > 0 ? static_cast<double>(e->value.asInt(25)) : 1.0;
      if (auto r = lensRange(static_cast<double>(e->value.asInt(24)) / units,
                             static_cast<double>(e->value.asInt(23)) / units, 0, 0)) {
        return r;
      }
    }
  }
  return std::nullopt;
}

namespace detail {

std::optional<MakerNoteLayout> identifyMakerNote(const InputSource& tiff, std::uint64_t offset, std::size_t size,
                                                 ByteOrder exifOrder, const std::string& make) {
  if (size < 8 || !tiff.contains(offset, size)) return std::nullopt;
  const Bytes head = tiff.readBytes(offset, std::min<std::size_t>(size, 32));
  const auto has = [&](std::string_view signature) { return startsWith(head, signature); };
  const auto orderAt = [&](std::size_t at, ByteOrder fallback) {
    if (head.size() >= at + 2 && head[at] == 'I' && head[at + 1] == 'I') return ByteOrder::little;
    if (head.size() >= at + 2 && head[at] == 'M' && head[at + 1] == 'M') return ByteOrder::big;
    return fallback;
  };
  using F = MakerNoteFormat;
  using B = MakerNoteBase;
  std::optional<MakerNoteLayout> layout;
  bool fixedBase = true;  // whether the format's base is known for certain
  if (has(std::string_view("Nikon\0\x02", 7)) && head.size() >= 18 &&
      isTiffHeader(head.data() + 10, head.size() - 10)) {
    const ByteOrder order = head[10] == 'I' ? ByteOrder::little : ByteOrder::big;
    layout =
        MakerNoteLayout{F::nikon, order, 10 + static_cast<std::size_t>(get32(head.data() + 14, order)), B::own, 10};
  } else if (has(std::string_view("Nikon\0\x01\0", 8))) {
    layout = MakerNoteLayout{F::nikonOld, exifOrder, 8, B::tiff, 0};
    fixedBase = false;
  } else if (has(std::string_view("OLYMPUS\0", 8))) {
    layout = MakerNoteLayout{F::olympus, orderAt(8, exifOrder), 12, B::note, 0};
  } else if (has(std::string_view("OM SYSTEM\0\0\0", 12))) {
    layout = MakerNoteLayout{F::olympus, orderAt(12, exifOrder), 16, B::note, 0};
  } else if (has(std::string_view("OLYMP\0", 6))) {
    layout = MakerNoteLayout{F::olympusOld, exifOrder, 8, B::tiff, 0};
  } else if (has(std::string_view("SONY DSC \0\0\0", 12)) || has(std::string_view("SONY CAM \0\0\0", 12)) ||
             has(std::string_view("SONY MOBILE\0", 12))) {
    layout = MakerNoteLayout{F::sony, exifOrder, 12, B::tiff, 0};
  } else if (has(std::string_view("Panasonic\0\0\0", 12))) {
    layout = MakerNoteLayout{F::panasonic, exifOrder, 12, B::tiff, 0};
  } else if (has(std::string_view("AOC\0", 4))) {
    layout = MakerNoteLayout{F::pentax, orderAt(4, exifOrder), 6, B::tiff, 0};
    fixedBase = false;
  } else if (has(std::string_view("PENTAX \0", 8))) {
    layout = MakerNoteLayout{F::pentax, orderAt(8, exifOrder), 10, B::note, 0};
  } else if (has("FUJIFILM") || has("GENERALE")) {
    layout = MakerNoteLayout{F::fujifilm, ByteOrder::little, getLe32(head.data() + 8), B::note, 0};
  } else if (has(std::string_view("Apple iOS\0", 10))) {
    layout = MakerNoteLayout{F::apple, orderAt(12, ByteOrder::big), 14, B::note, 0};
  } else if (startsWithCaseless(make, "Canon")) {
    layout = MakerNoteLayout{F::canon, exifOrder, 0, B::tiff, 0};
  } else if (startsWithCaseless(make, "NIKON")) {
    layout = MakerNoteLayout{F::nikonOld, exifOrder, 0, B::tiff, 0};
    fixedBase = false;
  } else if (startsWithCaseless(make, "SAMSUNG")) {
    layout = MakerNoteLayout{F::samsung, exifOrder, 0, B::note, 0};
    fixedBase = false;
  } else if (startsWithCaseless(make, "SONY")) {
    layout = MakerNoteLayout{F::sony, exifOrder, 0, B::tiff, 0};
  }
  if (!layout || layout->ifd >= size) return std::nullopt;

  // Some cameras write the note in the other byte order.
  if (entryCount(tiff, offset + layout->ifd, offset + size, layout->order) == 0) {
    layout->order = layout->order == ByteOrder::little ? ByteOrder::big : ByteOrder::little;
    if (entryCount(tiff, offset + layout->ifd, offset + size, layout->order) == 0) return std::nullopt;
  }
  if (!fixedBase) {
    MakerNoteLayout other = *layout;
    other.base = layout->base == B::tiff ? B::note : B::tiff;
    if (fit(tiff, offset, size, other) > fit(tiff, offset, size, *layout)) layout = other;
  }
  return layout;
}

std::shared_ptr<const MakerNote> decodeMakerNote(const InputSource& tiff, std::uint64_t offset, std::size_t size,
                                                 const MakerNoteLayout& layout) {
  try {
    (void)size;
    Walker walker(tiff, offset, layout);
    walker.ifd(offset + layout.ifd, groupOf(layout.format), 0);
    auto entries = walker.release();
    if (entries.empty()) return nullptr;
    return std::make_shared<const MakerNote>(layout.format, std::move(entries));
  } catch (const Error& e) {
    warn(std::string(makerNoteFormatName(layout.format)) + " maker note not decoded: " + e.what());
    return nullptr;
  }
}

bool relocateMakerNote(Bytes& note, const MakerNoteLayout& layout, std::uint64_t from, std::uint64_t to) {
  if (layout.base != MakerNoteBase::tiff || from == to) return true;
  const ByteOrder order = layout.order;
  const std::size_t at = layout.ifd;
  if (!inBounds(note.size(), at, 2)) return false;
  const std::uint32_t n = get16(note.data() + at, order);
  if (n == 0 || n > kMaxEntries || !inBounds(note.size(), at + 2, 12ull * n)) return false;
  const std::uint64_t end = from + note.size();
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint8_t* e = note.data() + at + 2 + 12 * static_cast<std::size_t>(i);
    const std::uint16_t type = get16(e + 2, order);
    if (!isFieldType(type)) continue;
    const std::uint64_t bytes = std::uint64_t{get32(e + 4, order)} * fieldTypeSize(FieldType(type));
    if (bytes <= 4) continue;
    const std::uint64_t old = get32(e + 8, order);
    if (old < from || old >= end) continue;
    const std::uint64_t moved = old - from + to;
    if (moved > 0xffffffffULL) return false;
    put32(e + 8, static_cast<std::uint32_t>(moved), order);
  }
  return true;
}

}  // namespace detail
}  // namespace lumenlib

#include <lumenlib/error.hpp>
#include <lumenlib/exif.hpp>
#include <lumenlib/makernote.hpp>

#include "exif_internal.hpp"
#include "formats.hpp"
#include "strings.hpp"

#include <algorithm>

namespace lumenlib {

const char* ifdName(Ifd ifd) noexcept {
  switch (ifd) {
    case Ifd::ifd0:
      return "ifd0";
    case Ifd::exif:
      return "exif";
    case Ifd::gps:
      return "gps";
    case Ifd::interop:
      return "interop";
    case Ifd::ifd1:
      return "ifd1";
  }
  return "unknown";
}

std::optional<Ifd> ifdFromName(std::string_view name) noexcept {
  for (auto ifd : {Ifd::ifd0, Ifd::exif, Ifd::gps, Ifd::interop, Ifd::ifd1}) {
    if (name == ifdName(ifd)) return ifd;
  }
  return std::nullopt;
}

namespace {

// Where a bare tag name is looked for: the Exif-specific IFDs first, so the
// TIFF/EP copies of Exif tags in IFD0 do not win.
constexpr Ifd kBareNameOrder[] = {Ifd::exif, Ifd::gps, Ifd::interop, Ifd::ifd0};

// The Exif comment tags hold a character code and text.
bool isCommentTag(const ExifTag& tag) {
  return (tag.ifd() == Ifd::exif && tag.number() == 0x9286) ||
         (tag.ifd() == Ifd::gps && (tag.number() == 0x001b || tag.number() == 0x001c));
}

FieldValue commentValue(std::string_view text) {
  Bytes b;
  if (detail::isAscii(text)) {
    const char code[8] = {'A', 'S', 'C', 'I', 'I', 0, 0, 0};
    b.assign(code, code + 8);
    b.insert(b.end(), text.begin(), text.end());
  } else {
    // "UNICODE" is UCS-2; written little-endian with a byte-order mark so
    // readers need not guess.
    const char code[8] = {'U', 'N', 'I', 'C', 'O', 'D', 'E', 0};
    b.assign(code, code + 8);
    b.push_back(0xff);
    b.push_back(0xfe);
    std::size_t i = 0;
    while (i < text.size()) {
      auto c = static_cast<unsigned char>(text[i]);
      std::uint32_t cp = c;
      std::size_t n = 0;
      if (c >= 0xf0) {
        cp = c & 0x07u;
        n = 3;
      } else if (c >= 0xe0) {
        cp = c & 0x0fu;
        n = 2;
      } else if (c >= 0xc0) {
        cp = c & 0x1fu;
        n = 1;
      }
      for (std::size_t k = 1; k <= n && i + k < text.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3fu);
      }
      i += n + 1;
      auto put = [&](std::uint32_t u) {
        b.push_back(static_cast<std::uint8_t>(u & 0xff));
        b.push_back(static_cast<std::uint8_t>(u >> 8));
      };
      if (cp >= 0x10000) {
        cp -= 0x10000;
        put(0xd800 + (cp >> 10));
        put(0xdc00 + (cp & 0x3ff));
      } else {
        put(cp);
      }
    }
  }
  return FieldValue::fromBytes(FieldType::undefined, std::move(b));
}

FieldValue integerValue(FieldType type, std::int64_t v) {
  switch (type) {
    case FieldType::u8:
    case FieldType::i8:
    case FieldType::undefined:
    case FieldType::u16:
    case FieldType::i16:
    case FieldType::u32:
    case FieldType::i32:
    case FieldType::ifd:
    case FieldType::urational:
    case FieldType::srational:
      return FieldValue::integers(type, {v});
    case FieldType::f32:
    case FieldType::f64:
      return FieldValue::reals(type, {static_cast<double>(v)});
    case FieldType::ascii:
      return FieldValue::ascii(std::to_string(v));
  }
  return FieldValue::integers(FieldType::i32, {v});
}

}  // namespace

// ---- ExifTag ------------------------------------------------------------------

std::optional<ExifTag> ExifTag::parse(std::string_view text) noexcept {
  const auto dot = text.find('.');
  if (dot == std::string_view::npos) {
    if (text.empty()) return std::nullopt;
    for (auto ifd : kBareNameOrder) {
      if (const auto* spec = lookupExifTag(ifd, text)) return ExifTag(ifd, spec->number);
    }
    return std::nullopt;
  }
  const auto ifd = ifdFromName(text.substr(0, dot));
  const auto name = text.substr(dot + 1);
  if (!ifd || name.empty()) return std::nullopt;
  if (const auto* spec = lookupExifTag(*ifd, name)) return ExifTag(*ifd, spec->number);
  if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
    const auto n = detail::parseInt(name);
    if (n && *n >= 0 && *n <= 0xffff) return ExifTag(*ifd, static_cast<std::uint16_t>(*n));
  }
  return std::nullopt;
}

ExifTag::ExifTag(std::string_view text) : ifd_(Ifd::ifd0), number_(0) {
  const auto tag = parse(text);
  if (!tag) throw Error(ErrorCode::invalidArgument, "invalid Exif tag '" + std::string(text) + "'");
  *this = *tag;
}

std::string ExifTag::name() const {
  if (const auto* s = spec()) return s->name;
  return detail::hex4(number_);
}

std::string ExifTag::str() const { return std::string(ifdName(ifd_)) + "." + name(); }

// ---- ExifEntry ----------------------------------------------------------------

ExifEntry::ExifEntry(ExifTag tag, FieldValue value) : tag_(tag), value_(std::move(value)) {}

FieldType ExifEntry::defaultType() const noexcept {
  if (const auto* s = tag_.spec()) return s->type;
  return value_.type();
}

void ExifEntry::setText(std::string_view text) {
  if (isCommentTag(tag_)) {
    value_ = commentValue(text);
  } else {
    value_ = FieldValue::parse(defaultType(), text);
  }
}

void ExifEntry::setInt(std::int64_t v) { value_ = integerValue(defaultType(), v); }

void ExifEntry::setRational(const Rational& v) {
  FieldType type = defaultType();
  if (type != FieldType::urational && type != FieldType::srational) {
    type = (v.numerator < 0 || v.denominator < 0) ? FieldType::srational : FieldType::urational;
  }
  value_ = FieldValue::rationals(type, {v});
}

// ---- ExifMetadata -------------------------------------------------------------

ExifMetadata::ExifMetadata() = default;
ExifMetadata::~ExifMetadata() = default;
ExifMetadata::ExifMetadata(const ExifMetadata&) = default;
ExifMetadata::ExifMetadata(ExifMetadata&&) noexcept = default;
ExifMetadata& ExifMetadata::operator=(const ExifMetadata&) = default;
ExifMetadata& ExifMetadata::operator=(ExifMetadata&&) noexcept = default;

ExifEntry* ExifMetadata::find(const ExifTag& tag) {
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ExifEntry& e) { return e.tag() == tag; });
  return it == entries_.end() ? nullptr : &*it;
}

const ExifEntry* ExifMetadata::find(const ExifTag& tag) const { return const_cast<ExifMetadata*>(this)->find(tag); }

ExifEntry* ExifMetadata::find(std::string_view tag) {
  const auto t = ExifTag::parse(tag);
  return t ? find(*t) : nullptr;
}

const ExifEntry* ExifMetadata::find(std::string_view tag) const { return const_cast<ExifMetadata*>(this)->find(tag); }

ExifEntry& ExifMetadata::entry(const ExifTag& tag) {
  if (auto* e = find(tag)) return *e;
  const auto* spec = tag.spec();
  entries_.emplace_back(tag, FieldValue(spec ? spec->type : FieldType::undefined));
  return entries_.back();
}

ExifEntry& ExifMetadata::entry(std::string_view tag) { return entry(ExifTag(tag)); }

ExifEntry& ExifMetadata::set(const ExifTag& tag, FieldValue value) {
  // Duplicates after the first go; the first keeps its position.
  bool first = true;
  removeIf([&](const ExifEntry& e) {
    if (e.tag() != tag) return false;
    if (first) {
      first = false;
      return false;
    }
    return true;
  });
  ExifEntry& e = entry(tag);
  e.setValue(std::move(value));
  return e;
}

ExifEntry& ExifMetadata::set(std::string_view tag, FieldValue value) { return set(ExifTag(tag), std::move(value)); }

ExifEntry& ExifMetadata::setText(std::string_view tag, std::string_view text) {
  const ExifTag t(tag);
  ExifEntry probe(t, find(t) ? find(t)->value() : FieldValue(t.spec() ? t.spec()->type : FieldType::undefined));
  probe.setText(text);  // parse before changing anything
  return set(t, probe.value());
}

ExifEntry& ExifMetadata::setInt(std::string_view tag, std::int64_t value) {
  const ExifTag t(tag);
  ExifEntry probe(t, find(t) ? find(t)->value() : FieldValue(t.spec() ? t.spec()->type : FieldType::undefined));
  probe.setInt(value);
  return set(t, probe.value());
}

ExifEntry& ExifMetadata::setRational(std::string_view tag, const Rational& value) {
  const ExifTag t(tag);
  ExifEntry probe(t, find(t) ? find(t)->value() : FieldValue(t.spec() ? t.spec()->type : FieldType::undefined));
  probe.setRational(value);
  return set(t, probe.value());
}

void ExifMetadata::append(ExifEntry entry) { entries_.push_back(std::move(entry)); }

ExifMetadata::iterator ExifMetadata::erase(iterator pos) { return entries_.erase(pos); }

std::size_t ExifMetadata::remove(const ExifTag& tag) {
  return removeIf([&](const ExifEntry& e) { return e.tag() == tag; });
}

std::size_t ExifMetadata::remove(std::string_view tag) {
  const auto t = ExifTag::parse(tag);
  if (!t) throw Error(ErrorCode::invalidArgument, "invalid Exif tag '" + std::string(tag) + "'");
  return remove(*t);
}

void ExifMetadata::clear() {
  entries_.clear();
  thumbnail_.clear();
  origin_.reset();
  makerNote_.reset();
  makerNoteOrigin_.reset();
}

void ExifMetadata::sort() {
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const ExifEntry& a, const ExifEntry& b) { return a.tag() < b.tag(); });
}

void ExifMetadata::setThumbnail(Bytes jpeg) {
  thumbnail_ = std::move(jpeg);
  if (!thumbnail_.empty() && !find(ExifTag(Ifd::ifd1, 0x0103))) {
    append(ExifTag(Ifd::ifd1, 0x0103), FieldValue::integers(FieldType::u16, {6}));
  }
}

void ExifMetadata::removeThumbnail() {
  thumbnail_.clear();
  removeIf([](const ExifEntry& e) { return e.ifd() == Ifd::ifd1; });
}

Bytes ExifMetadata::encode() const { return detail::encodeExif(*this); }

ExifMetadata ExifMetadata::decode(const std::uint8_t* data, std::size_t size) {
  detail::stripExifPrefix(data, size);
  ExifMetadata exif;
  detail::decodeTiff(data, size, exif);
  return exif;
}

}  // namespace lumenlib

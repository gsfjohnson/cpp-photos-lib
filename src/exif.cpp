#include <photos/error.hpp>
#include <photos/exif.hpp>

#include "exif_internal.hpp"
#include "strings.hpp"

#include <algorithm>

namespace photos {

const char* groupName(IfdId ifd) noexcept {
  switch (ifd) {
    case IfdId::ifd0:
      return "Image";
    case IfdId::exif:
      return "Photo";
    case IfdId::gps:
      return "GPSInfo";
    case IfdId::interop:
      return "Iop";
    case IfdId::ifd1:
      return "Thumbnail";
  }
  return "Unknown";
}

namespace {

constexpr IfdId kAllIfds[] = {IfdId::ifd0, IfdId::exif, IfdId::gps, IfdId::interop, IfdId::ifd1};

// The Exif comment tags hold a character code and text.
bool isCommentTag(const ExifKey& key) {
  return (key.ifd() == IfdId::exif && key.tag() == 0x9286) ||
         (key.ifd() == IfdId::gps && (key.tag() == 0x001b || key.tag() == 0x001c));
}

Value commentValue(std::string_view text) {
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
  return Value::bytes(TypeId::undefined, std::move(b));
}

}  // namespace

// ---- ExifKey ------------------------------------------------------------------

ExifKey::ExifKey(std::string_view key) : ifd_(IfdId::ifd0), tag_(0) {
  const auto bad = [&] { return Error(ErrorCode::invalidArgument, "invalid Exif key '" + std::string(key) + "'"); };
  if (key.substr(0, 5) != "Exif.") throw bad();
  const auto rest = key.substr(5);
  const auto dot = rest.find('.');
  if (dot == std::string_view::npos) throw bad();
  const auto group = rest.substr(0, dot);
  const auto name = rest.substr(dot + 1);
  bool found = false;
  for (auto ifd : kAllIfds) {
    if (group == photos::groupName(ifd)) {
      ifd_ = ifd;
      found = true;
      break;
    }
  }
  if (!found || name.empty()) throw bad();
  if (const auto* info = findExifTag(ifd_, name)) {
    tag_ = info->tag;
    return;
  }
  if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
    const auto n = detail::parseInt(name);
    if (n && *n >= 0 && *n <= 0xffff) {
      tag_ = static_cast<std::uint16_t>(*n);
      return;
    }
  }
  throw bad();
}

std::string ExifKey::tagName() const {
  if (const auto* i = info()) return i->name;
  return detail::hex4(tag_);
}

std::string ExifKey::str() const { return std::string("Exif.") + photos::groupName(ifd_) + "." + tagName(); }

// ---- ExifDatum ----------------------------------------------------------------

ExifDatum::ExifDatum(ExifKey key, Value value) : key_(key), value_(std::move(value)) {}

TypeId ExifDatum::defaultType() const noexcept {
  if (const auto* i = key_.info()) return i->type;
  return value_.typeId();
}

ExifDatum& ExifDatum::operator=(std::string_view text) {
  if (isCommentTag(key_)) {
    value_ = commentValue(text);
  } else {
    value_ = Value::fromString(defaultType(), text);
  }
  return *this;
}

namespace {

Value integerValue(TypeId type, std::int64_t v) {
  switch (type) {
    case TypeId::unsignedByte:
    case TypeId::signedByte:
    case TypeId::undefined:
    case TypeId::unsignedShort:
    case TypeId::signedShort:
    case TypeId::unsignedLong:
    case TypeId::signedLong:
    case TypeId::tiffIfd:
    case TypeId::unsignedRational:
    case TypeId::signedRational:
      return Value::integers(type, {v});
    case TypeId::tiffFloat:
    case TypeId::tiffDouble:
      return Value::reals(type, {static_cast<double>(v)});
    case TypeId::asciiString:
      return Value::ascii(std::to_string(v));
  }
  return Value::integers(TypeId::signedLong, {v});
}

}  // namespace

ExifDatum& ExifDatum::operator=(std::uint16_t v) {
  value_ = integerValue(defaultType(), v);
  return *this;
}

ExifDatum& ExifDatum::operator=(std::uint32_t v) {
  value_ = integerValue(defaultType(), v);
  return *this;
}

ExifDatum& ExifDatum::operator=(std::int32_t v) {
  value_ = integerValue(defaultType(), v);
  return *this;
}

ExifDatum& ExifDatum::operator=(const Rational& v) {
  TypeId type = defaultType();
  if (type != TypeId::unsignedRational && type != TypeId::signedRational) {
    type = (v.numerator < 0 || v.denominator < 0) ? TypeId::signedRational : TypeId::unsignedRational;
  }
  value_ = Value::rationals(type, {v});
  return *this;
}

// ---- ExifData -----------------------------------------------------------------

ExifDatum& ExifData::operator[](std::string_view key) {
  const ExifKey k(key);
  auto it = findKey(k);
  if (it != data_.end()) return *it;
  const auto* info = k.info();
  data_.emplace_back(k, Value(info ? info->type : TypeId::undefined));
  return data_.back();
}

void ExifData::add(ExifDatum datum) { data_.push_back(std::move(datum)); }

ExifData::iterator ExifData::findKey(const ExifKey& key) {
  return std::find_if(data_.begin(), data_.end(), [&](const ExifDatum& d) { return d.exifKey() == key; });
}

ExifData::const_iterator ExifData::findKey(const ExifKey& key) const {
  return std::find_if(data_.begin(), data_.end(), [&](const ExifDatum& d) { return d.exifKey() == key; });
}

ExifDatum* ExifData::find(std::string_view key) {
  try {
    auto it = findKey(ExifKey(key));
    return it == data_.end() ? nullptr : &*it;
  } catch (const Error&) {
    return nullptr;
  }
}

const ExifDatum* ExifData::find(std::string_view key) const {
  try {
    auto it = findKey(ExifKey(key));
    return it == data_.end() ? nullptr : &*it;
  } catch (const Error&) {
    return nullptr;
  }
}

ExifData::iterator ExifData::erase(iterator pos) { return data_.erase(pos); }

std::size_t ExifData::erase(std::string_view key) {
  const ExifKey k(key);
  const auto before = data_.size();
  data_.erase(std::remove_if(data_.begin(), data_.end(), [&](const ExifDatum& d) { return d.exifKey() == k; }),
              data_.end());
  return before - data_.size();
}

void ExifData::clear() {
  data_.clear();
  thumbnail_.clear();
  origin_.reset();
}

void ExifData::sortByKey() {
  std::stable_sort(data_.begin(), data_.end(), [](const ExifDatum& a, const ExifDatum& b) {
    if (a.ifd() != b.ifd()) return a.ifd() < b.ifd();
    return a.tag() < b.tag();
  });
}

void ExifData::setThumbnail(Bytes jpeg) {
  thumbnail_ = std::move(jpeg);
  if (!thumbnail_.empty() && findKey(ExifKey(IfdId::ifd1, 0x0103)) == data_.end()) {
    add(ExifKey(IfdId::ifd1, 0x0103), Value::integers(TypeId::unsignedShort, {6}));
  }
}

void ExifData::eraseThumbnail() {
  thumbnail_.clear();
  data_.erase(std::remove_if(data_.begin(), data_.end(), [](const ExifDatum& d) { return d.ifd() == IfdId::ifd1; }),
              data_.end());
}

}  // namespace photos

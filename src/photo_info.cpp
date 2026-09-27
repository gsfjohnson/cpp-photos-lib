#include <photos/error.hpp>
#include <photos/photo_info.hpp>

#include "strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace photos {
namespace {

std::string trimmed(std::string_view s) {
  // Exif strings are often padded with spaces or NULs.
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return std::string(s);
}

std::string exifText(const Image& image, std::string_view key) {
  const auto* d = image.exifData().find(key);
  return d ? trimmed(d->toString()) : std::string();
}

std::optional<double> exifReal(const Image& image, std::string_view key) {
  const auto* d = image.exifData().find(key);
  if (!d || d->count() == 0) return std::nullopt;
  const Rational r = d->toRational();
  if ((d->typeId() == TypeId::unsignedRational || d->typeId() == TypeId::signedRational) && r.denominator == 0) {
    return std::nullopt;
  }
  return d->toDouble();
}

std::optional<std::int64_t> exifInt(const Image& image, std::string_view key) {
  const auto* d = image.exifData().find(key);
  if (!d || d->count() == 0) return std::nullopt;
  return d->toInt64();
}

const XmpDatum* xmp(const Image& image, std::string_view key) { return image.xmpData().find(key); }

std::string xmpText(const Image& image, std::string_view key) {
  const auto* d = xmp(image, key);
  if (!d) return {};
  switch (d->kind()) {
    case XmpValue::Kind::text:
      return d->value().text();
    case XmpValue::Kind::langAlt:
      return d->value().langText().value_or(std::string());
    case XmpValue::Kind::bag:
    case XmpValue::Kind::seq:
    case XmpValue::Kind::alt:
      return d->value().items().empty() ? std::string() : d->value().items().front();
    default:
      return {};
  }
}

// "1/250" or "0.004".
std::optional<double> parseNumber(const std::string& s) {
  const auto slash = s.find('/');
  if (slash != std::string::npos) {
    const auto n = detail::parseDouble(s.substr(0, slash));
    const auto d = detail::parseDouble(s.substr(slash + 1));
    if (!n || !d || *d == 0) return std::nullopt;
    return *n / *d;
  }
  return detail::parseDouble(s);
}

std::optional<double> xmpReal(const Image& image, std::string_view key) {
  const std::string t = xmpText(image, key);
  if (t.empty()) return std::nullopt;
  return parseNumber(t);
}

std::string iptcText(const Image& image, std::string_view key) {
  const auto* d = image.iptcData().find(key);
  return d ? d->toString() : std::string();
}

// Windows' XP* tags: UTF-16LE, NUL-terminated.
std::string utf16leToUtf8(const Bytes& b) {
  std::string out;
  for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
    std::uint32_t cp = b[i] | (b[i + 1] << 8);
    if (cp == 0) break;
    if (cp >= 0xd800 && cp <= 0xdbff && i + 3 < b.size()) {
      const std::uint32_t lo = b[i + 2] | (b[i + 3] << 8);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i += 2;
      }
    }
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  return out;
}

std::string exifXpText(const Image& image, std::string_view key) {
  const auto* d = image.exifData().find(key);
  return d ? trimmed(utf16leToUtf8(d->value().toBytes(ByteOrder::littleEndian))) : std::string();
}

std::string firstOf(std::initializer_list<std::string> candidates) {
  for (const auto& c : candidates) {
    if (!c.empty()) return c;
  }
  return {};
}

// Exif's date, with its sub-second and offset companions.
std::optional<DateTime> exifDate(const Image& image, std::string_view date, std::string_view subsec,
                                 std::string_view offset) {
  auto dt = DateTime::parse(exifText(image, date));
  if (!dt) return std::nullopt;
  const std::string ss = exifText(image, subsec);
  if (!ss.empty() && detail::isDigit(ss[0])) {
    std::string digits;
    for (char c : ss) {
      if (!detail::isDigit(c)) break;
      digits += c;
    }
    digits.resize(3, '0');
    dt->millisecond = std::stoi(digits);
  }
  const std::string off = exifText(image, offset);
  if (off.size() >= 6 && (off[0] == '+' || off[0] == '-')) {
    const auto h = detail::parseInt(off.substr(1, 2));
    const auto m = detail::parseInt(off.substr(4, 2));
    if (h && m && *h <= 23 && *m <= 59)
      dt->utcOffsetMinutes = static_cast<int>((off[0] == '-' ? -1 : 1) * (*h * 60 + *m));
  }
  return dt;
}

std::optional<DateTime> iptcDate(const Image& image) {
  const std::string date = iptcText(image, "Iptc.Application2.DateCreated");
  if (date.size() < 8) return std::nullopt;
  std::string text = date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2);
  const std::string time = iptcText(image, "Iptc.Application2.TimeCreated");
  if (time.size() >= 6) {
    text += "T" + time.substr(0, 2) + ":" + time.substr(2, 2) + ":" + time.substr(4, 2);
    if (time.size() >= 11) text += time.substr(6, 3) + ":" + time.substr(9, 2);
  }
  return DateTime::parse(text);
}

std::optional<double> gpsCoordinate(const Image& image, std::string_view valueKey, std::string_view refKey) {
  const auto* v = image.exifData().find(valueKey);
  if (!v || v->count() == 0) return std::nullopt;
  double degrees = 0;
  const double scale[] = {1, 60, 3600};
  for (std::size_t i = 0; i < v->count() && i < 3; ++i) {
    const Rational r = v->toRational(i);
    if (r.denominator == 0) {
      if (i == 0) return std::nullopt;
      continue;
    }
    degrees += r.toDouble() / scale[i];
  }
  const std::string ref = exifText(image, refKey);
  if (!ref.empty() && (ref[0] == 'S' || ref[0] == 'W')) degrees = -degrees;
  return degrees;
}

// XMP's "DDD,MM.mmmk" or "DDD,MM,SSk".
std::optional<double> xmpGpsCoordinate(const std::string& text) {
  if (text.size() < 3) return std::nullopt;
  const char ref = text.back();
  if (ref != 'N' && ref != 'S' && ref != 'E' && ref != 'W') return std::nullopt;
  const std::string body = text.substr(0, text.size() - 1);
  double parts[3] = {0, 0, 0};
  std::size_t n = 0, start = 0;
  while (n < 3) {
    const auto comma = body.find(',', start);
    const auto p = detail::parseDouble(body.substr(start, comma - start));
    if (!p) return std::nullopt;
    parts[n++] = *p;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  const double degrees = parts[0] + parts[1] / 60 + parts[2] / 3600;
  return (ref == 'S' || ref == 'W') ? -degrees : degrees;
}

std::string pad2(int v) {
  char b[16];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

std::string offsetText(int minutes, bool colon = true) {
  const char sign = minutes < 0 ? '-' : '+';
  minutes = std::abs(minutes);
  return std::string(1, sign) + pad2(minutes / 60) + (colon ? ":" : "") + pad2(minutes % 60);
}

void setExif(Image& image, std::string_view key, Value value) { image.exifData()[key].setValue(std::move(value)); }

}  // namespace

// ---- DateTime -----------------------------------------------------------------

std::string DateTime::toIso8601() const {
  char b[40];
  std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d", year, month, day, hour, minute, second);
  std::string s = b;
  if (millisecond) {
    std::snprintf(b, sizeof b, ".%03d", *millisecond);
    s += b;
  }
  if (utcOffsetMinutes) s += offsetText(*utcOffsetMinutes);
  return s;
}

std::optional<DateTime> DateTime::parse(const std::string& input) {
  const std::string_view text = detail::trim(input);
  DateTime dt;
  std::size_t pos = 0;
  auto number = [&](std::size_t digits, int& out) {
    if (pos + digits > text.size()) return false;
    int v = 0;
    for (std::size_t i = 0; i < digits; ++i) {
      if (!detail::isDigit(text[pos + i])) return false;
      v = v * 10 + (text[pos + i] - '0');
    }
    out = v;
    pos += digits;
    return true;
  };
  auto separator = [&](std::string_view allowed) {
    if (pos < text.size() && allowed.find(text[pos]) != std::string_view::npos) {
      ++pos;
      return true;
    }
    return false;
  };
  if (!number(4, dt.year)) return std::nullopt;
  // IPTC's compact YYYYMMDD.
  if (text.size() == 8 && number(2, dt.month) && number(2, dt.day)) {
  } else {
    if (separator(":-")) {
      if (!number(2, dt.month)) return std::nullopt;
      if (separator(":-") && !number(2, dt.day)) return std::nullopt;
    }
    if (separator("T ")) {
      if (!number(2, dt.hour) || !separator(":") || !number(2, dt.minute)) return std::nullopt;
      if (separator(":")) {
        if (!number(2, dt.second)) return std::nullopt;
        if (separator(".,")) {
          int ms = 0, digits = 0;
          while (pos < text.size() && detail::isDigit(text[pos])) {
            if (digits < 3) ms = ms * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
          }
          if (digits == 0) return std::nullopt;
          for (; digits < 3; ++digits) ms *= 10;
          dt.millisecond = ms;
        }
      }
      if (separator("Z")) {
        dt.utcOffsetMinutes = 0;
      } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        const int sign = text[pos++] == '-' ? -1 : 1;
        int h = 0, m = 0;
        if (!number(2, h)) return std::nullopt;
        separator(":");
        number(2, m);
        dt.utcOffsetMinutes = sign * (h * 60 + m);
      }
    }
  }
  if (pos != text.size()) return std::nullopt;
  if (dt.year == 0) return std::nullopt;
  if (dt.month > 12 || dt.day > 31 || dt.hour > 23 || dt.minute > 59 || dt.second > 60) return std::nullopt;
  if (dt.month == 0) dt.month = 1;
  if (dt.day == 0) dt.day = 1;
  return dt;
}

// ---- Reading ------------------------------------------------------------------

PhotoInfo readPhotoInfo(const Image& image) {
  PhotoInfo info;

  info.dateTaken =
      exifDate(image, "Exif.Photo.DateTimeOriginal", "Exif.Photo.SubSecTimeOriginal", "Exif.Photo.OffsetTimeOriginal");
  if (!info.dateTaken) info.dateTaken = DateTime::parse(xmpText(image, "Xmp.exif.DateTimeOriginal"));
  if (!info.dateTaken) info.dateTaken = DateTime::parse(xmpText(image, "Xmp.photoshop.DateCreated"));
  if (!info.dateTaken) info.dateTaken = iptcDate(image);
  if (!info.dateTaken) info.dateTaken = DateTime::parse(xmpText(image, "Xmp.xmp.CreateDate"));
  info.dateDigitized = exifDate(image, "Exif.Photo.DateTimeDigitized", "Exif.Photo.SubSecTimeDigitized",
                                "Exif.Photo.OffsetTimeDigitized");
  if (!info.dateDigitized) info.dateDigitized = DateTime::parse(xmpText(image, "Xmp.exif.DateTimeDigitized"));
  if (!info.dateDigitized) info.dateDigitized = DateTime::parse(xmpText(image, "Xmp.xmp.CreateDate"));

  auto orientation = exifInt(image, "Exif.Image.Orientation");
  if (!orientation) {
    if (auto x = detail::parseInt(xmpText(image, "Xmp.tiff.Orientation"))) orientation = *x;
  }
  if (orientation && *orientation >= 1 && *orientation <= 8) info.orientation = static_cast<int>(*orientation);

  info.width = image.pixelWidth();
  info.height = image.pixelHeight();
  if (info.width == 0 || info.height == 0) {
    const auto w = exifInt(image, "Exif.Photo.PixelXDimension");
    const auto h = exifInt(image, "Exif.Photo.PixelYDimension");
    if (w && h && *w > 0 && *h > 0 && *w <= 0xffffffffLL && *h <= 0xffffffffLL) {
      info.width = static_cast<std::uint32_t>(*w);
      info.height = static_cast<std::uint32_t>(*h);
    }
  }

  info.cameraMake = firstOf({exifText(image, "Exif.Image.Make"), xmpText(image, "Xmp.tiff.Make")});
  info.cameraModel = firstOf({exifText(image, "Exif.Image.Model"), xmpText(image, "Xmp.tiff.Model")});
  info.lensModel = firstOf({exifText(image, "Exif.Photo.LensModel"), xmpText(image, "Xmp.exifEX.LensModel"),
                            xmpText(image, "Xmp.aux.Lens")});

  info.exposureTime = exifReal(image, "Exif.Photo.ExposureTime");
  if (!info.exposureTime) info.exposureTime = xmpReal(image, "Xmp.exif.ExposureTime");
  info.fNumber = exifReal(image, "Exif.Photo.FNumber");
  if (!info.fNumber) info.fNumber = xmpReal(image, "Xmp.exif.FNumber");
  info.focalLength = exifReal(image, "Exif.Photo.FocalLength");
  if (!info.focalLength) info.focalLength = xmpReal(image, "Xmp.exif.FocalLength");
  if (auto f35 = exifInt(image, "Exif.Photo.FocalLengthIn35mmFilm")) info.focalLength35mm = static_cast<int>(*f35);
  if (auto iso = exifInt(image, "Exif.Photo.ISOSpeedRatings")) {
    info.iso = static_cast<int>(*iso);
  } else if (auto x = detail::parseInt(xmpText(image, "Xmp.exif.ISOSpeedRatings"))) {
    info.iso = static_cast<int>(*x);
  }
  if (auto flash = exifInt(image, "Exif.Photo.Flash")) info.flashFired = (*flash & 1) != 0;

  const auto lat = gpsCoordinate(image, "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef");
  const auto lon = gpsCoordinate(image, "Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSLongitudeRef");
  if (lat && lon) {
    GpsPosition gps{*lat, *lon, std::nullopt};
    if (auto alt = exifReal(image, "Exif.GPSInfo.GPSAltitude")) {
      const auto ref = exifInt(image, "Exif.GPSInfo.GPSAltitudeRef");
      gps.altitude = (ref && *ref == 1) ? -*alt : *alt;
    }
    info.gps = gps;
  } else {
    const auto xlat = xmpGpsCoordinate(xmpText(image, "Xmp.exif.GPSLatitude"));
    const auto xlon = xmpGpsCoordinate(xmpText(image, "Xmp.exif.GPSLongitude"));
    if (xlat && xlon) {
      GpsPosition gps{*xlat, *xlon, std::nullopt};
      if (auto alt = xmpReal(image, "Xmp.exif.GPSAltitude")) {
        gps.altitude = xmpText(image, "Xmp.exif.GPSAltitudeRef") == "1" ? -*alt : *alt;
      }
      info.gps = gps;
    }
  }

  info.title = firstOf({xmpText(image, "Xmp.dc.title"), iptcText(image, "Iptc.Application2.ObjectName"),
                        exifXpText(image, "Exif.Image.XPTitle"), exifText(image, "Exif.Photo.ImageTitle")});
  info.description =
      firstOf({xmpText(image, "Xmp.dc.description"), iptcText(image, "Iptc.Application2.Caption"),
               exifText(image, "Exif.Image.ImageDescription"), exifXpText(image, "Exif.Image.XPComment")});
  if (const auto* subject = xmp(image, "Xmp.dc.subject")) {
    info.keywords =
        subject->value().isArray() ? subject->value().items() : std::vector<std::string>{subject->value().text()};
  }
  if (info.keywords.empty()) info.keywords = image.iptcData().values("Iptc.Application2.Keywords");
  if (info.keywords.empty()) {
    const std::string xp = exifXpText(image, "Exif.Image.XPKeywords");
    std::size_t start = 0;
    while (start < xp.size()) {
      const auto semi = xp.find(';', start);
      const std::string k = trimmed(xp.substr(start, semi - start));
      if (!k.empty()) info.keywords.push_back(k);
      if (semi == std::string::npos) break;
      start = semi + 1;
    }
  }

  if (const auto* creator = xmp(image, "Xmp.dc.creator"); creator && creator->value().isArray()) {
    for (const auto& c : creator->value().items()) info.creator += (info.creator.empty() ? "" : "; ") + c;
  }
  if (info.creator.empty()) {
    for (const auto& c : image.iptcData().values("Iptc.Application2.Byline")) {
      info.creator += (info.creator.empty() ? "" : "; ") + c;
    }
  }
  if (info.creator.empty())
    info.creator = firstOf({xmpText(image, "Xmp.dc.creator"), exifText(image, "Exif.Image.Artist")});
  info.copyright = firstOf({xmpText(image, "Xmp.dc.rights"), iptcText(image, "Iptc.Application2.Copyright"),
                            exifText(image, "Exif.Image.Copyright")});

  if (auto r = xmpReal(image, "Xmp.xmp.Rating")) {
    info.rating = static_cast<int>(std::lround(*r));
  } else if (auto er = exifInt(image, "Exif.Image.Rating")) {
    info.rating = static_cast<int>(*er);
  } else if (auto pct = xmpReal(image, "Xmp.MicrosoftPhoto.Rating")) {
    info.rating = *pct <= 0 ? 0 : *pct < 13 ? 1 : *pct < 38 ? 2 : *pct < 63 ? 3 : *pct < 88 ? 4 : 5;
  }
  if (info.rating && (*info.rating < -1 || *info.rating > 5)) info.rating.reset();
  return info;
}

// ---- Writing ------------------------------------------------------------------

void setOrientation(Image& image, int orientation) {
  if (orientation < 1 || orientation > 8) throw Error(ErrorCode::invalidArgument, "orientation must be 1..8");
  const bool exif = image.canWrite(MetadataKind::exif);
  if (exif) setExif(image, "Exif.Image.Orientation", Value::integers(TypeId::unsignedShort, {orientation}));
  if (!exif || image.xmpData().find("Xmp.tiff.Orientation")) {
    image.xmpData()["Xmp.tiff.Orientation"] = std::to_string(orientation);
  }
}

void setRating(Image& image, int rating) {
  if (rating < -1 || rating > 5) throw Error(ErrorCode::invalidArgument, "rating must be -1..5");
  image.xmpData()["Xmp.xmp.Rating"] = std::to_string(rating);
  static const int percent[] = {0, 1, 25, 50, 75, 99};
  const int pct = rating < 0 ? 0 : percent[rating];
  if (image.xmpData().find("Xmp.MicrosoftPhoto.Rating"))
    image.xmpData()["Xmp.MicrosoftPhoto.Rating"] = std::to_string(pct);
  if (image.exifData().find("Exif.Image.Rating")) {
    setExif(image, "Exif.Image.Rating", Value::integers(TypeId::unsignedShort, {std::max(rating, 0)}));
  }
  if (image.exifData().find("Exif.Image.RatingPercent")) {
    setExif(image, "Exif.Image.RatingPercent", Value::integers(TypeId::unsignedShort, {pct}));
  }
}

void setKeywords(Image& image, const std::vector<std::string>& keywords) {
  if (keywords.empty()) {
    image.xmpData().erase("Xmp.dc.subject");
  } else {
    image.xmpData()["Xmp.dc.subject"] = XmpValue::array(XmpValue::Kind::bag, keywords);
  }
  if (!image.iptcData().empty() && image.canWrite(MetadataKind::iptc)) {
    image.iptcData().setValues("Iptc.Application2.Keywords", keywords);
  }
}

void setTitle(Image& image, const std::string& title) {
  if (title.empty()) {
    image.xmpData().erase("Xmp.dc.title");
  } else {
    auto& d = image.xmpData()["Xmp.dc.title"];
    if (d.kind() != XmpValue::Kind::langAlt) d = XmpValue(XmpValue::Kind::langAlt);
    d.value().setLangText("x-default", title);
  }
  if (!image.iptcData().empty() && image.canWrite(MetadataKind::iptc)) {
    image.iptcData().setValues("Iptc.Application2.ObjectName",
                               title.empty() ? std::vector<std::string>{} : std::vector<std::string>{title});
  }
}

void setDescription(Image& image, const std::string& description) {
  if (description.empty()) {
    image.xmpData().erase("Xmp.dc.description");
  } else {
    auto& d = image.xmpData()["Xmp.dc.description"];
    if (d.kind() != XmpValue::Kind::langAlt) d = XmpValue(XmpValue::Kind::langAlt);
    d.value().setLangText("x-default", description);
  }
  if (image.canWrite(MetadataKind::exif)) {
    if (description.empty()) {
      image.exifData().erase("Exif.Image.ImageDescription");
    } else {
      setExif(image, "Exif.Image.ImageDescription", Value::ascii(description));
    }
  }
  if (!image.iptcData().empty() && image.canWrite(MetadataKind::iptc)) {
    image.iptcData().setValues("Iptc.Application2.Caption", description.empty()
                                                                ? std::vector<std::string>{}
                                                                : std::vector<std::string>{description});
  }
}

void setDateTaken(Image& image, const DateTime& date) {
  char exifDate[32];
  std::snprintf(exifDate, sizeof exifDate, "%04d:%02d:%02d %02d:%02d:%02d", date.year, date.month, date.day, date.hour,
                date.minute, date.second);
  const bool exif = image.canWrite(MetadataKind::exif);
  if (exif) {
    setExif(image, "Exif.Photo.DateTimeOriginal", Value::ascii(exifDate));
    if (date.millisecond) {
      char ms[8];
      std::snprintf(ms, sizeof ms, "%03d", *date.millisecond);
      setExif(image, "Exif.Photo.SubSecTimeOriginal", Value::ascii(ms));
    } else {
      image.exifData().erase("Exif.Photo.SubSecTimeOriginal");
    }
    if (date.utcOffsetMinutes) {
      setExif(image, "Exif.Photo.OffsetTimeOriginal", Value::ascii(offsetText(*date.utcOffsetMinutes)));
    } else {
      image.exifData().erase("Exif.Photo.OffsetTimeOriginal");
    }
  }
  const std::string iso = date.toIso8601();
  image.xmpData()["Xmp.photoshop.DateCreated"] = iso;
  if (!exif || image.xmpData().find("Xmp.exif.DateTimeOriginal")) image.xmpData()["Xmp.exif.DateTimeOriginal"] = iso;
  if (!image.iptcData().empty() && image.canWrite(MetadataKind::iptc)) {
    char d[16], t[16];
    std::snprintf(d, sizeof d, "%04d%02d%02d", date.year, date.month, date.day);
    std::snprintf(t, sizeof t, "%02d%02d%02d", date.hour, date.minute, date.second);
    image.iptcData().setValues("Iptc.Application2.DateCreated", {d});
    image.iptcData().setValues("Iptc.Application2.TimeCreated",
                               {std::string(t) + offsetText(date.utcOffsetMinutes.value_or(0), false)});
  }
}

void eraseGpsPosition(Image& image) {
  auto& exif = image.exifData();
  for (auto it = exif.begin(); it != exif.end();) {
    it = it->ifd() == IfdId::gps ? exif.erase(it) : it + 1;
  }
  auto& xmpData = image.xmpData();
  for (auto it = xmpData.begin(); it != xmpData.end();) {
    it = it->key().compare(0, 12, "Xmp.exif.GPS") == 0 ? xmpData.erase(it) : it + 1;
  }
}

void setGpsPosition(Image& image, const GpsPosition& position) {
  if (!std::isfinite(position.latitude) || !std::isfinite(position.longitude) || std::fabs(position.latitude) > 90 ||
      std::fabs(position.longitude) > 180) {
    throw Error(ErrorCode::invalidArgument, "GPS position out of range");
  }
  eraseGpsPosition(image);
  const auto dms = [](double v) {
    v = std::fabs(v);
    const auto deg = static_cast<std::int64_t>(v);
    const double minutes = (v - static_cast<double>(deg)) * 60;
    const auto min = static_cast<std::int64_t>(minutes);
    const auto sec = static_cast<std::int64_t>(std::llround((minutes - static_cast<double>(min)) * 60 * 10000));
    return Value::rationals(TypeId::unsignedRational, {{deg, 1}, {min, 1}, {sec, 10000}});
  };
  if (image.canWrite(MetadataKind::exif)) {
    setExif(image, "Exif.GPSInfo.GPSVersionID", Value::integers(TypeId::unsignedByte, {2, 3, 0, 0}));
    setExif(image, "Exif.GPSInfo.GPSLatitudeRef", Value::ascii(position.latitude < 0 ? "S" : "N"));
    setExif(image, "Exif.GPSInfo.GPSLatitude", dms(position.latitude));
    setExif(image, "Exif.GPSInfo.GPSLongitudeRef", Value::ascii(position.longitude < 0 ? "W" : "E"));
    setExif(image, "Exif.GPSInfo.GPSLongitude", dms(position.longitude));
    if (position.altitude && std::isfinite(*position.altitude)) {
      setExif(image, "Exif.GPSInfo.GPSAltitudeRef",
              Value::integers(TypeId::unsignedByte, {*position.altitude < 0 ? 1 : 0}));
      setExif(
          image, "Exif.GPSInfo.GPSAltitude",
          Value::rationals(TypeId::unsignedRational,
                           {{static_cast<std::int64_t>(std::llround(std::fabs(*position.altitude) * 1000)), 1000}}));
    }
    return;
  }
  const auto xmpCoord = [](double v, char pos, char neg) {
    const double a = std::fabs(v);
    const auto deg = static_cast<int>(a);
    char b[40];
    std::snprintf(b, sizeof b, "%d,%.6f%c", deg, (a - deg) * 60, v < 0 ? neg : pos);
    return std::string(b);
  };
  auto& x = image.xmpData();
  x["Xmp.exif.GPSVersionID"] = "2.3.0.0";
  x["Xmp.exif.GPSLatitude"] = xmpCoord(position.latitude, 'N', 'S');
  x["Xmp.exif.GPSLongitude"] = xmpCoord(position.longitude, 'E', 'W');
  if (position.altitude && std::isfinite(*position.altitude)) {
    x["Xmp.exif.GPSAltitudeRef"] = *position.altitude < 0 ? "1" : "0";
    x["Xmp.exif.GPSAltitude"] = std::to_string(std::llround(std::fabs(*position.altitude) * 1000)) + "/1000";
  }
}

}  // namespace photos

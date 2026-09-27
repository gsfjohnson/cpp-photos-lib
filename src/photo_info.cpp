#include <lumenlib/error.hpp>
#include <lumenlib/makernote.hpp>
#include <lumenlib/photo_info.hpp>

#include "strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lumenlib {
namespace {

std::string trimmed(std::string_view s) {
  // Exif strings are often padded with spaces or NULs.
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return std::string(s);
}

std::string exifText(const ImageFile& file, std::string_view tag) {
  const auto* e = file.exif().find(tag);
  return e ? trimmed(e->text()) : std::string();
}

std::optional<double> exifReal(const ImageFile& file, std::string_view tag) {
  const auto* e = file.exif().find(tag);
  if (!e || e->count() == 0) return std::nullopt;
  const Rational r = e->asRational();
  if ((e->type() == FieldType::urational || e->type() == FieldType::srational) && r.denominator == 0) {
    return std::nullopt;
  }
  return e->asDouble();
}

std::optional<std::int64_t> exifInt(const ImageFile& file, std::string_view tag) {
  const auto* e = file.exif().find(tag);
  if (!e || e->count() == 0) return std::nullopt;
  return e->asInt();
}

std::string xmpText(const ImageFile& file, std::string_view path) { return file.xmp().text(path).value_or(""); }

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

std::optional<double> xmpReal(const ImageFile& file, std::string_view path) {
  const std::string t = xmpText(file, path);
  if (t.empty()) return std::nullopt;
  return parseNumber(t);
}

std::string iptcText(const ImageFile& file, std::string_view dataset) {
  return file.iptc().value(dataset).value_or("");
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

std::string exifXpText(const ImageFile& file, std::string_view tag) {
  const auto* e = file.exif().find(tag);
  return e ? trimmed(utf16leToUtf8(e->value().encode(ByteOrder::little))) : std::string();
}

std::string firstOf(std::initializer_list<std::string> candidates) {
  for (const auto& c : candidates) {
    if (!c.empty()) return c;
  }
  return {};
}

// Exif's date, with its sub-second and offset companions.
std::optional<DateTime> exifDate(const ImageFile& file, std::string_view date, std::string_view subsec,
                                 std::string_view offset) {
  auto dt = DateTime::parse(exifText(file, date));
  if (!dt) return std::nullopt;
  const std::string ss = exifText(file, subsec);
  if (!ss.empty() && detail::isDigit(ss[0])) {
    std::string digits;
    for (char c : ss) {
      if (!detail::isDigit(c)) break;
      digits += c;
    }
    digits.resize(3, '0');
    dt->millisecond = std::stoi(digits);
  }
  const std::string off = exifText(file, offset);
  if (off.size() >= 6 && (off[0] == '+' || off[0] == '-')) {
    const auto h = detail::parseInt(off.substr(1, 2));
    const auto m = detail::parseInt(off.substr(4, 2));
    if (h && m && *h <= 23 && *m <= 59)
      dt->utcOffsetMinutes = static_cast<int>((off[0] == '-' ? -1 : 1) * (*h * 60 + *m));
  }
  return dt;
}

std::optional<DateTime> iptcDate(const ImageFile& file) {
  const std::string date = iptcText(file, "DateCreated");
  if (date.size() < 8) return std::nullopt;
  std::string text = date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2);
  const std::string time = iptcText(file, "TimeCreated");
  if (time.size() >= 6) {
    text += "T" + time.substr(0, 2) + ":" + time.substr(2, 2) + ":" + time.substr(4, 2);
    if (time.size() >= 11) text += time.substr(6, 3) + ":" + time.substr(9, 2);
  }
  return DateTime::parse(text);
}

std::optional<double> gpsCoordinate(const ImageFile& file, std::string_view valueTag, std::string_view refTag) {
  const auto* v = file.exif().find(valueTag);
  if (!v || v->count() == 0) return std::nullopt;
  double degrees = 0;
  const double scale[] = {1, 60, 3600};
  for (std::size_t i = 0; i < v->count() && i < 3; ++i) {
    const Rational r = v->asRational(i);
    if (r.denominator == 0) {
      if (i == 0) return std::nullopt;
      continue;
    }
    degrees += r.toDouble() / scale[i];
  }
  const std::string ref = exifText(file, refTag);
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

std::string DateTime::toExif() const {
  char b[40];
  std::snprintf(b, sizeof b, "%04d:%02d:%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
  return b;
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

PhotoInfo readPhotoInfo(const ImageFile& file) {
  PhotoInfo info;

  info.dateTaken = exifDate(file, "exif.DateTimeOriginal", "exif.SubSecTimeOriginal", "exif.OffsetTimeOriginal");
  if (!info.dateTaken) info.dateTaken = DateTime::parse(xmpText(file, "exif:DateTimeOriginal"));
  if (!info.dateTaken) info.dateTaken = DateTime::parse(xmpText(file, "photoshop:DateCreated"));
  if (!info.dateTaken) info.dateTaken = iptcDate(file);
  if (!info.dateTaken) info.dateTaken = DateTime::parse(xmpText(file, "xmp:CreateDate"));
  info.dateDigitized = exifDate(file, "exif.DateTimeDigitized", "exif.SubSecTimeDigitized", "exif.OffsetTimeDigitized");
  if (!info.dateDigitized) info.dateDigitized = DateTime::parse(xmpText(file, "exif:DateTimeDigitized"));
  if (!info.dateDigitized) info.dateDigitized = DateTime::parse(xmpText(file, "xmp:CreateDate"));

  auto orientation = exifInt(file, "ifd0.Orientation");
  if (!orientation) {
    if (auto x = detail::parseInt(xmpText(file, "tiff:Orientation"))) orientation = *x;
  }
  if (orientation && *orientation >= 1 && *orientation <= 8) info.orientation = static_cast<int>(*orientation);

  info.width = file.width();
  info.height = file.height();
  if (info.width == 0 || info.height == 0) {
    const auto w = exifInt(file, "exif.PixelXDimension");
    const auto h = exifInt(file, "exif.PixelYDimension");
    if (w && h && *w > 0 && *h > 0 && *w <= 0xffffffffLL && *h <= 0xffffffffLL) {
      info.width = static_cast<std::uint32_t>(*w);
      info.height = static_cast<std::uint32_t>(*h);
    }
  }

  info.cameraMake = firstOf({exifText(file, "ifd0.Make"), xmpText(file, "tiff:Make")});
  info.cameraModel = firstOf({exifText(file, "ifd0.Model"), xmpText(file, "tiff:Model")});
  info.lensModel = firstOf({exifText(file, "exif.LensModel"), xmpText(file, "exifEX:LensModel"),
                            xmpText(file, "aux:Lens"), lensDescription(file.exif()).value_or("")});

  info.exposureTime = exifReal(file, "exif.ExposureTime");
  if (!info.exposureTime) info.exposureTime = xmpReal(file, "exif:ExposureTime");
  info.fNumber = exifReal(file, "exif.FNumber");
  if (!info.fNumber) info.fNumber = xmpReal(file, "exif:FNumber");
  info.focalLength = exifReal(file, "exif.FocalLength");
  if (!info.focalLength) info.focalLength = xmpReal(file, "exif:FocalLength");
  if (auto f35 = exifInt(file, "exif.FocalLengthIn35mmFilm")) info.focalLength35mm = static_cast<int>(*f35);
  if (auto iso = exifInt(file, "exif.PhotographicSensitivity")) {
    info.iso = static_cast<int>(*iso);
  } else if (auto x = detail::parseInt(xmpText(file, "exifEX:PhotographicSensitivity"))) {
    info.iso = static_cast<int>(*x);
  } else if (auto y = detail::parseInt(xmpText(file, "exif:ISOSpeedRatings"))) {
    info.iso = static_cast<int>(*y);
  } else if (file.exif().contains("ifd0.0x002e")) {
    // A Panasonic RW2 (which carries its JPEG in 0x002E) has the ISO in 0x0017.
    if (auto z = exifInt(file, "ifd0.0x0017"); z && *z > 0) info.iso = static_cast<int>(*z);
  }
  if (auto flash = exifInt(file, "exif.Flash")) info.flashFired = (*flash & 1) != 0;

  const auto lat = gpsCoordinate(file, "gps.GPSLatitude", "gps.GPSLatitudeRef");
  const auto lon = gpsCoordinate(file, "gps.GPSLongitude", "gps.GPSLongitudeRef");
  if (lat && lon) {
    GpsPosition gps{*lat, *lon, std::nullopt};
    if (auto alt = exifReal(file, "gps.GPSAltitude")) {
      const auto ref = exifInt(file, "gps.GPSAltitudeRef");
      gps.altitude = (ref && *ref == 1) ? -*alt : *alt;
    }
    info.gps = gps;
  } else {
    const auto xlat = xmpGpsCoordinate(xmpText(file, "exif:GPSLatitude"));
    const auto xlon = xmpGpsCoordinate(xmpText(file, "exif:GPSLongitude"));
    if (xlat && xlon) {
      GpsPosition gps{*xlat, *xlon, std::nullopt};
      if (auto alt = xmpReal(file, "exif:GPSAltitude")) {
        gps.altitude = xmpText(file, "exif:GPSAltitudeRef") == "1" ? -*alt : *alt;
      }
      info.gps = gps;
    }
  }

  info.title = firstOf({xmpText(file, "dc:title"), iptcText(file, "ObjectName"), exifXpText(file, "ifd0.XPTitle"),
                        exifText(file, "exif.ImageTitle")});
  info.description = firstOf({xmpText(file, "dc:description"), iptcText(file, "CaptionAbstract"),
                              exifText(file, "ifd0.ImageDescription"), exifXpText(file, "ifd0.XPComment")});
  if (const auto* subject = file.xmp().find("dc:subject")) {
    info.keywords =
        subject->value().isArray() ? subject->value().items() : std::vector<std::string>{subject->value().text()};
  }
  if (info.keywords.empty()) info.keywords = file.iptc().values("Keywords");
  if (info.keywords.empty()) {
    const std::string xp = exifXpText(file, "ifd0.XPKeywords");
    std::size_t start = 0;
    while (start < xp.size()) {
      const auto semi = xp.find(';', start);
      const std::string k = trimmed(xp.substr(start, semi - start));
      if (!k.empty()) info.keywords.push_back(k);
      if (semi == std::string::npos) break;
      start = semi + 1;
    }
  }

  if (const auto* creator = file.xmp().find("dc:creator"); creator && creator->value().isArray()) {
    for (const auto& c : creator->value().items()) info.creator += (info.creator.empty() ? "" : "; ") + c;
  }
  if (info.creator.empty()) {
    for (const auto& c : file.iptc().values("ByLine")) info.creator += (info.creator.empty() ? "" : "; ") + c;
  }
  if (info.creator.empty()) info.creator = firstOf({xmpText(file, "dc:creator"), exifText(file, "ifd0.Artist")});
  info.copyright =
      firstOf({xmpText(file, "dc:rights"), iptcText(file, "CopyrightNotice"), exifText(file, "ifd0.Copyright")});

  if (auto r = xmpReal(file, "xmp:Rating")) {
    info.rating = static_cast<int>(std::lround(*r));
  } else if (auto er = exifInt(file, "ifd0.Rating")) {
    info.rating = static_cast<int>(*er);
  } else if (auto pct = xmpReal(file, "MicrosoftPhoto:Rating")) {
    info.rating = *pct <= 0 ? 0 : *pct < 13 ? 1 : *pct < 38 ? 2 : *pct < 63 ? 3 : *pct < 88 ? 4 : 5;
  }
  if (info.rating && (*info.rating < -1 || *info.rating > 5)) info.rating.reset();
  return info;
}

// ---- Writing ------------------------------------------------------------------

void setOrientation(ImageFile& file, int orientation) {
  if (orientation < 1 || orientation > 8) throw Error(ErrorCode::invalidArgument, "orientation must be 1..8");
  const bool exif = file.canWrite(MetadataKind::exif);
  if (exif) file.exif().set("ifd0.Orientation", FieldValue::integers(FieldType::u16, {orientation}));
  if (!exif || file.xmp().contains("tiff:Orientation")) {
    file.xmp().setText("tiff:Orientation", std::to_string(orientation));
  }
}

void setRating(ImageFile& file, int rating) {
  if (rating < -1 || rating > 5) throw Error(ErrorCode::invalidArgument, "rating must be -1..5");
  file.xmp().setText("xmp:Rating", std::to_string(rating));
  static const int percent[] = {0, 1, 25, 50, 75, 99};
  const int pct = rating < 0 ? 0 : percent[rating];
  if (file.xmp().contains("MicrosoftPhoto:Rating")) file.xmp().setText("MicrosoftPhoto:Rating", std::to_string(pct));
  if (file.exif().contains("ifd0.Rating")) {
    file.exif().set("ifd0.Rating", FieldValue::integers(FieldType::u16, {std::max(rating, 0)}));
  }
  if (file.exif().contains("ifd0.RatingPercent")) {
    file.exif().set("ifd0.RatingPercent", FieldValue::integers(FieldType::u16, {pct}));
  }
}

void setKeywords(ImageFile& file, const std::vector<std::string>& keywords) {
  if (keywords.empty()) {
    file.xmp().remove("dc:subject");
  } else {
    file.xmp().set("dc:subject", XmpValue::array(XmpValue::Kind::bag, keywords));
  }
  if (!file.iptc().empty() && file.canWrite(MetadataKind::iptc)) file.iptc().setValues("Keywords", keywords);
}

void setTitle(ImageFile& file, const std::string& title) {
  if (title.empty()) {
    file.xmp().remove("dc:title");
  } else {
    file.xmp().setLangText("dc:title", "x-default", title);
  }
  if (!file.iptc().empty() && file.canWrite(MetadataKind::iptc)) {
    file.iptc().setValues("ObjectName", title.empty() ? std::vector<std::string>{} : std::vector<std::string>{title});
  }
}

void setDescription(ImageFile& file, const std::string& description) {
  if (description.empty()) {
    file.xmp().remove("dc:description");
  } else {
    file.xmp().setLangText("dc:description", "x-default", description);
  }
  if (file.canWrite(MetadataKind::exif)) {
    if (description.empty()) {
      file.exif().remove("ifd0.ImageDescription");
    } else {
      file.exif().set("ifd0.ImageDescription", FieldValue::ascii(description));
    }
  }
  if (!file.iptc().empty() && file.canWrite(MetadataKind::iptc)) {
    file.iptc().setValues("CaptionAbstract",
                          description.empty() ? std::vector<std::string>{} : std::vector<std::string>{description});
  }
}

void setDateTaken(ImageFile& file, const DateTime& date) {
  const bool exif = file.canWrite(MetadataKind::exif);
  if (exif) {
    file.exif().set("exif.DateTimeOriginal", FieldValue::ascii(date.toExif()));
    if (date.millisecond) {
      char ms[8];
      std::snprintf(ms, sizeof ms, "%03d", *date.millisecond);
      file.exif().set("exif.SubSecTimeOriginal", FieldValue::ascii(ms));
    } else {
      file.exif().remove("exif.SubSecTimeOriginal");
    }
    if (date.utcOffsetMinutes) {
      file.exif().set("exif.OffsetTimeOriginal", FieldValue::ascii(offsetText(*date.utcOffsetMinutes)));
    } else {
      file.exif().remove("exif.OffsetTimeOriginal");
    }
  }
  const std::string iso = date.toIso8601();
  file.xmp().setText("photoshop:DateCreated", iso);
  if (!exif || file.xmp().contains("exif:DateTimeOriginal")) file.xmp().setText("exif:DateTimeOriginal", iso);
  if (!file.iptc().empty() && file.canWrite(MetadataKind::iptc)) {
    char d[16], t[16];
    std::snprintf(d, sizeof d, "%04d%02d%02d", date.year, date.month, date.day);
    std::snprintf(t, sizeof t, "%02d%02d%02d", date.hour, date.minute, date.second);
    file.iptc().setValues("DateCreated", {d});
    file.iptc().setValues("TimeCreated", {std::string(t) + offsetText(date.utcOffsetMinutes.value_or(0), false)});
  }
}

void eraseGpsPosition(ImageFile& file) {
  file.exif().removeIf([](const ExifEntry& e) { return e.ifd() == Ifd::gps; });
  file.xmp().removeIf([](const XmpEntry& e) { return e.path().compare(0, 8, "exif:GPS") == 0; });
}

void setGpsPosition(ImageFile& file, const GpsPosition& position) {
  if (!std::isfinite(position.latitude) || !std::isfinite(position.longitude) || std::fabs(position.latitude) > 90 ||
      std::fabs(position.longitude) > 180) {
    throw Error(ErrorCode::invalidArgument, "GPS position out of range");
  }
  eraseGpsPosition(file);
  const auto dms = [](double v) {
    v = std::fabs(v);
    auto deg = static_cast<std::int64_t>(v);
    const double minutes = (v - static_cast<double>(deg)) * 60;
    auto min = static_cast<std::int64_t>(minutes);
    auto sec = static_cast<std::int64_t>(std::llround((minutes - static_cast<double>(min)) * 60 * 10000));
    if (sec >= 600000) {
      sec -= 600000;
      if (++min == 60) {
        min = 0;
        ++deg;
      }
    }
    return FieldValue::rationals(FieldType::urational, {{deg, 1}, {min, 1}, {sec, 10000}});
  };
  if (file.canWrite(MetadataKind::exif)) {
    auto& exif = file.exif();
    exif.set("gps.GPSVersionID", FieldValue::integers(FieldType::u8, {2, 3, 0, 0}));
    exif.set("gps.GPSLatitudeRef", FieldValue::ascii(position.latitude < 0 ? "S" : "N"));
    exif.set("gps.GPSLatitude", dms(position.latitude));
    exif.set("gps.GPSLongitudeRef", FieldValue::ascii(position.longitude < 0 ? "W" : "E"));
    exif.set("gps.GPSLongitude", dms(position.longitude));
    if (position.altitude && std::isfinite(*position.altitude)) {
      exif.set("gps.GPSAltitudeRef", FieldValue::integers(FieldType::u8, {*position.altitude < 0 ? 1 : 0}));
      exif.set("gps.GPSAltitude",
               FieldValue::rationals(
                   FieldType::urational,
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
  auto& x = file.xmp();
  x.setText("exif:GPSVersionID", "2.3.0.0");
  x.setText("exif:GPSLatitude", xmpCoord(position.latitude, 'N', 'S'));
  x.setText("exif:GPSLongitude", xmpCoord(position.longitude, 'E', 'W'));
  if (position.altitude && std::isfinite(*position.altitude)) {
    x.setText("exif:GPSAltitudeRef", *position.altitude < 0 ? "1" : "0");
    x.setText("exif:GPSAltitude", std::to_string(std::llround(std::fabs(*position.altitude) * 1000)) + "/1000");
  }
}

}  // namespace lumenlib

// ExifEntry::describe: values as a person reads them, following the meanings
// the TIFF and Exif specifications give each tag's numbers.
#include <lumenlib/exif.hpp>

#include "bytes.hpp"
#include "strings.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace lumenlib {
namespace {

struct Meaning {
  std::int64_t value;
  const char* text;
};

template <std::size_t N>
std::string choose(const Meaning (&meanings)[N], std::int64_t v) {
  for (const auto& m : meanings) {
    if (m.value == v) return m.text;
  }
  return "(" + std::to_string(v) + ")";
}

std::string fixed(double v, int decimals) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", decimals, v);
  return b;
}

// The shortest of 1-3 decimals that shows the value.
std::string shortNumber(double v) {
  for (int d = 0; d < 3; ++d) {
    if (std::fabs(v * std::pow(10.0, d) - std::round(v * std::pow(10.0, d))) < 1e-6) return fixed(v, d);
  }
  return fixed(v, 3);
}

std::string exposureTime(const Rational& r) {
  if (r.denominator == 0 || r.numerator <= 0)
    return "(" + std::to_string(r.numerator) + "/" + std::to_string(r.denominator) + ")";
  const double seconds = r.toDouble();
  if (seconds >= 1) return shortNumber(seconds) + " s";
  return "1/" + shortNumber(1 / seconds) + " s";
}

std::string fNumber(double f) {
  return f > 0 ? "F" + shortNumber(std::round(f * 10) / 10) : "(" + shortNumber(f) + ")";
}

std::string version(const Bytes& b) {
  // Four ASCII digits: "0232" is 2.32.
  if (b.size() != 4) return {};
  for (auto c : b) {
    if (c < '0' || c > '9') return {};
  }
  std::string s;
  s += static_cast<char>(b[0] == '0' ? b[1] : b[0]);
  if (b[0] != '0') s += static_cast<char>(b[1]);
  s += '.';
  s += static_cast<char>(b[2]);
  if (b[3] != '0') s += static_cast<char>(b[3]);
  return s;
}

std::string utf16leText(const Bytes& b, std::size_t from) {
  std::string out;
  for (std::size_t i = from; i + 1 < b.size(); i += 2) {
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

// UserComment and the GPS text tags: an 8-byte character code, then text.
std::string comment(const Bytes& b) {
  if (b.size() < 8) return {};
  const std::string code(b.begin(), b.begin() + 8);
  if (code.compare(0, 7, "UNICODE") == 0) {
    std::size_t from = 8;
    // A byte-order mark says the order; without one, UCS-2 is taken as
    // little-endian, as most writers use.
    if (b.size() >= 10 && b[8] == 0xfe && b[9] == 0xff) {
      Bytes swapped(b.begin() + 10, b.end());
      for (std::size_t i = 0; i + 1 < swapped.size(); i += 2) std::swap(swapped[i], swapped[i + 1]);
      return utf16leText(swapped, 0);
    }
    if (b.size() >= 10 && b[8] == 0xff && b[9] == 0xfe) from = 10;
    return utf16leText(b, from);
  }
  std::string text(b.begin() + 8, b.end());
  while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.pop_back();
  if (!detail::isValidUtf8(text)) text = detail::latin1ToUtf8(text);
  return text;
}

std::string degrees(const FieldValue& v) {
  if (v.count() < 3) return v.text();
  const double d = v.asDouble(0), m = v.asDouble(1), s = v.asDouble(2);
  return shortNumber(d) + "\xc2\xb0 " + shortNumber(m) + "' " + fixed(s, 2) + "\"";
}

const Meaning kOrientation[] = {{1, "Upright"},
                                {2, "Mirrored horizontally"},
                                {3, "Rotated 180\xc2\xb0"},
                                {4, "Mirrored vertically"},
                                {5, "Mirrored horizontally, rotated 90\xc2\xb0 counter-clockwise"},
                                {6, "Rotated 90\xc2\xb0 clockwise"},
                                {7, "Mirrored horizontally, rotated 90\xc2\xb0 clockwise"},
                                {8, "Rotated 90\xc2\xb0 counter-clockwise"}};
const Meaning kResolutionUnit[] = {{1, "None"}, {2, "Inches"}, {3, "Centimetres"}};
const Meaning kCompression[] = {{1, "Uncompressed"},   {2, "CCITT RLE"},  {3, "CCITT Group 3"},
                                {4, "CCITT Group 4"},  {5, "LZW"},        {6, "JPEG (old-style)"},
                                {7, "JPEG"},           {8, "Deflate"},    {32773, "PackBits"},
                                {34892, "Lossy JPEG"}, {52546, "JPEG XL"}};
const Meaning kPhotometric[] = {{0, "White is zero"},           {1, "Black is zero"}, {2, "RGB"},   {3, "Palette"},
                                {4, "Transparency mask"},       {5, "CMYK"},          {6, "YCbCr"}, {8, "CIELab"},
                                {32803, "Colour filter array"}, {34892, "Linear raw"}};
const Meaning kPlanar[] = {{1, "Chunky"}, {2, "Planar"}};
const Meaning kYCbCrPositioning[] = {{1, "Centred"}, {2, "Co-sited"}};
const Meaning kExposureProgram[] = {{0, "Not defined"},       {1, "Manual"},           {2, "Normal program"},
                                    {3, "Aperture priority"}, {4, "Shutter priority"}, {5, "Creative program"},
                                    {6, "Action program"},    {7, "Portrait mode"},    {8, "Landscape mode"}};
const Meaning kMeteringMode[] = {{0, "Unknown"}, {1, "Average"},    {2, "Centre-weighted average"},
                                 {3, "Spot"},    {4, "Multi-spot"}, {5, "Pattern"},
                                 {6, "Partial"}, {255, "Other"}};
const Meaning kLightSource[] = {{0, "Unknown"},
                                {1, "Daylight"},
                                {2, "Fluorescent"},
                                {3, "Tungsten (incandescent)"},
                                {4, "Flash"},
                                {9, "Fine weather"},
                                {10, "Cloudy weather"},
                                {11, "Shade"},
                                {12, "Daylight fluorescent (D 5700-7100K)"},
                                {13, "Day white fluorescent (N 4600-5500K)"},
                                {14, "Cool white fluorescent (W 3800-4500K)"},
                                {15, "White fluorescent (WW 3250-3800K)"},
                                {16, "Warm white fluorescent (L 2600-3250K)"},
                                {17, "Standard light A"},
                                {18, "Standard light B"},
                                {19, "Standard light C"},
                                {20, "D55"},
                                {21, "D65"},
                                {22, "D75"},
                                {23, "D50"},
                                {24, "ISO studio tungsten"},
                                {255, "Other light source"}};
const Meaning kColorSpace[] = {{1, "sRGB"}, {2, "Adobe RGB"}, {0xffff, "Uncalibrated"}};
const Meaning kSensingMethod[] = {{1, "Not defined"},
                                  {2, "One-chip colour area sensor"},
                                  {3, "Two-chip colour area sensor"},
                                  {4, "Three-chip colour area sensor"},
                                  {5, "Colour sequential area sensor"},
                                  {7, "Trilinear sensor"},
                                  {8, "Colour sequential linear sensor"}};
const Meaning kCustomRendered[] = {{0, "Normal process"},
                                   {1, "Custom process"},
                                   {2, "HDR (no original saved)"},
                                   {3, "HDR (original saved)"},
                                   {4, "Original (for HDR)"},
                                   {6, "Panorama"},
                                   {7, "Portrait HDR"},
                                   {8, "Portrait"}};
const Meaning kExposureMode[] = {{0, "Auto exposure"}, {1, "Manual exposure"}, {2, "Auto bracket"}};
const Meaning kWhiteBalance[] = {{0, "Auto"}, {1, "Manual"}};
const Meaning kSceneCaptureType[] = {{0, "Standard"}, {1, "Landscape"}, {2, "Portrait"}, {3, "Night scene"}};
const Meaning kGainControl[] = {
    {0, "None"}, {1, "Low gain up"}, {2, "High gain up"}, {3, "Low gain down"}, {4, "High gain down"}};
const Meaning kContrast[] = {{0, "Normal"}, {1, "Soft"}, {2, "Hard"}};
const Meaning kSaturation[] = {{0, "Normal"}, {1, "Low"}, {2, "High"}};
const Meaning kSubjectDistanceRange[] = {{0, "Unknown"}, {1, "Macro"}, {2, "Close view"}, {3, "Distant view"}};
const Meaning kSensitivityType[] = {{0, "Unknown"},
                                    {1, "Standard output sensitivity"},
                                    {2, "Recommended exposure index"},
                                    {3, "ISO speed"},
                                    {4, "Standard output sensitivity and recommended exposure index"},
                                    {5, "Standard output sensitivity and ISO speed"},
                                    {6, "Recommended exposure index and ISO speed"},
                                    {7, "Standard output sensitivity, recommended exposure index and ISO speed"}};
const Meaning kCompositeImage[] = {{0, "Unknown"},
                                   {1, "Not a composite image"},
                                   {2, "General composite image"},
                                   {3, "Composite image captured while shooting"}};
const Meaning kAltitudeRef[] = {{0, "Above sea level"}, {1, "Below sea level"}};
const Meaning kGpsDifferential[] = {{0, "Without correction"}, {1, "Correction applied"}};

std::string flash(std::int64_t v) {
  std::string s = (v & 1) ? "Fired" : "Did not fire";
  switch ((v >> 1) & 3) {
    case 2:
      s += ", return not detected";
      break;
    case 3:
      s += ", return detected";
      break;
    default:
      break;
  }
  switch ((v >> 3) & 3) {
    case 1:
      s += ", compulsory";
      break;
    case 2:
      s += ", suppressed";
      break;
    case 3:
      s += ", auto mode";
      break;
    default:
      break;
  }
  if (v & 0x20) s = "No flash function";
  if (v & 0x40) s += ", red-eye reduction";
  return s;
}

std::string gpsRef(const std::string& t, std::uint16_t tag) {
  if (t.empty()) return t;
  switch (tag) {
    case 0x0001:
    case 0x0013:
      return t[0] == 'N' ? "North" : t[0] == 'S' ? "South" : t;
    case 0x0003:
    case 0x0015:
      return t[0] == 'E' ? "East" : t[0] == 'W' ? "West" : t;
    case 0x000c:
      return t[0] == 'K' ? "km/h" : t[0] == 'M' ? "mph" : t[0] == 'N' ? "knots" : t;
    case 0x000e:
    case 0x0010:
    case 0x0017:
      return t[0] == 'T' ? "True direction" : t[0] == 'M' ? "Magnetic direction" : t;
    case 0x0019:
      return t[0] == 'K' ? "Kilometres" : t[0] == 'M' ? "Miles" : t[0] == 'N' ? "Nautical miles" : t;
    case 0x0009:
      return t[0] == 'A' ? "Measurement in progress" : t[0] == 'V' ? "Measurement interrupted" : t;
    case 0x000a:
      return t[0] == '2' ? "2-dimensional" : t[0] == '3' ? "3-dimensional" : t;
    default:
      return t;
  }
}

std::string plain(const FieldValue& v) {
  if ((v.type() == FieldType::undefined || v.type() == FieldType::u8) && v.count() > 64) {
    return "(" + std::to_string(v.count()) + " bytes)";
  }
  return v.text();
}

}  // namespace

std::string ExifEntry::describe() const {
  const FieldValue& v = value_;
  if (v.empty()) return {};
  const auto first = [&] { return v.asInt(0); };
  const std::uint16_t n = number();
  try {
    if (ifd() == Ifd::gps) {
      switch (n) {
        case 0x0000:
          if (v.count() == 4) {
            return std::to_string(v.asInt(0)) + "." + std::to_string(v.asInt(1)) + "." + std::to_string(v.asInt(2)) +
                   "." + std::to_string(v.asInt(3));
          }
          break;
        case 0x0002:
        case 0x0004:
        case 0x0014:
        case 0x0016:
          return degrees(v);
        case 0x0005:
          return choose(kAltitudeRef, first());
        case 0x0006:
          return shortNumber(v.asDouble()) + " m";
        case 0x0007:
          if (v.count() == 3) {
            char b[32];
            std::snprintf(b, sizeof b, "%02d:%02d:%05.2f", static_cast<int>(v.asDouble(0)),
                          static_cast<int>(v.asDouble(1)), v.asDouble(2));
            return b;
          }
          break;
        case 0x001b:
        case 0x001c:
          return comment(v.bytes());
        case 0x001e:
          return choose(kGpsDifferential, first());
        case 0x001f:
          return shortNumber(v.asDouble()) + " m";
        default:
          if (v.type() == FieldType::ascii) return gpsRef(v.text(), n);
          break;
      }
      return plain(v);
    }
    if (ifd() == Ifd::interop) {
      if (n == 0x0002 && v.type() == FieldType::undefined) {
        if (auto s = version(v.bytes()); !s.empty()) return s;
      }
      return plain(v);
    }
    // IFD0, IFD1 and the Exif IFD share their numbers (TIFF/EP puts Exif's
    // tags in IFD0).
    switch (n) {
      case 0x0103:
        return choose(kCompression, first());
      case 0x0106:
        return choose(kPhotometric, first());
      case 0x0112:
        return choose(kOrientation, first());
      case 0x011a:
      case 0x011b:
        return shortNumber(v.asDouble());
      case 0x011c:
        return choose(kPlanar, first());
      case 0x0128:
      case 0xa210:
        return choose(kResolutionUnit, first());
      case 0x0213:
        return choose(kYCbCrPositioning, first());
      case 0x829a:
        return exposureTime(v.asRational());
      case 0x829d:
        return fNumber(v.asDouble());
      case 0x8822:
        return choose(kExposureProgram, first());
      case 0x8827:
      case 0x8832:
      case 0x8833:
        return v.text();
      case 0x8830:
        return choose(kSensitivityType, first());
      case 0x9000:
      case 0xa000:
        if (auto s = version(v.bytes()); !s.empty()) return s;
        break;
      case 0x9101: {
        static const char* const parts[] = {"", "Y", "Cb", "Cr", "R", "G", "B"};
        std::string s;
        for (auto c : v.bytes()) {
          if (c > 0 && c < 7) s += parts[c];
        }
        return s.empty() ? plain(v) : s;
      }
      case 0x9102:
        return shortNumber(v.asDouble()) + " bits per pixel";
      case 0x9201: {
        // APEX: the time is 2^-value.
        const double t = std::pow(2.0, -v.asDouble());
        if (!std::isfinite(t) || t <= 0) break;
        return t >= 1 ? shortNumber(t) + " s" : "1/" + shortNumber(std::round(1 / t)) + " s";
      }
      case 0x9202:
      case 0x9205: {
        const double f = std::pow(2.0, v.asDouble() / 2);
        if (!std::isfinite(f)) break;
        return fNumber(f);
      }
      case 0x9203:
        return shortNumber(v.asDouble()) + " EV";
      case 0x9204: {
        const Rational r = v.asRational();
        if (r.denominator == 0) break;
        if (r.numerator == 0) return "0 EV";
        // Thirds and halves of a stop as fractions, as cameras show them.
        std::int64_t num = r.numerator, den = r.denominator;
        if (den < 0) num = -num, den = -den;
        std::int64_t a = num < 0 ? -num : num, b = den;
        while (b != 0) {
          const std::int64_t t = a % b;
          a = b;
          b = t;
        }
        num /= a, den /= a;
        const std::string sign = num > 0 ? "+" : "-";
        const std::int64_t whole = (num < 0 ? -num : num) / den, part = (num < 0 ? -num : num) % den;
        if (den == 1) return sign + std::to_string(whole) + " EV";
        if (den <= 3) {
          return sign + (whole ? std::to_string(whole) + " " : "") + std::to_string(part) + "/" + std::to_string(den) +
                 " EV";
        }
        return sign + shortNumber(std::fabs(r.toDouble())) + " EV";
      }
      case 0x9206:
        return shortNumber(v.asDouble()) + " m";
      case 0x9207:
        return choose(kMeteringMode, first());
      case 0x9208:
        return choose(kLightSource, first());
      case 0x9209:
        return flash(first());
      case 0x920a:
        return shortNumber(v.asDouble()) + " mm";
      case 0x9286:
        return comment(v.bytes());
      case 0x9c9b:
      case 0x9c9c:
      case 0x9c9d:
      case 0x9c9e:
      case 0x9c9f:
        return utf16leText(v.encode(ByteOrder::little), 0);
      case 0xa001:
        return choose(kColorSpace, first());
      case 0xa217:
      case 0x9217:
        return choose(kSensingMethod, first());
      case 0xa300:
        return first() == 3   ? "Digital still camera"
               : first() == 2 ? "Reflection print scanner"
               : first() == 1 ? "Transparency scanner"
                              : "Other";
      case 0xa301:
        return first() == 1 ? "Directly photographed" : plain(v);
      case 0xa401:
        return choose(kCustomRendered, first());
      case 0xa402:
        return choose(kExposureMode, first());
      case 0xa403:
        return choose(kWhiteBalance, first());
      case 0xa404:
        return first() == 0 && v.asRational().numerator == 0 ? "Not used" : shortNumber(v.asDouble()) + "x";
      case 0xa405:
        return v.text() + " mm";
      case 0xa406:
        return choose(kSceneCaptureType, first());
      case 0xa407:
        return choose(kGainControl, first());
      case 0xa408:
        return choose(kContrast, first());
      case 0xa409:
        return choose(kSaturation, first());
      case 0xa40a:
        return choose(kContrast, first());
      case 0xa40c:
        return choose(kSubjectDistanceRange, first());
      case 0xa432:
      case 0xc630:
        if (v.count() == 4) {
          std::string s = shortNumber(v.asDouble(0));
          if (v.asDouble(1) != v.asDouble(0)) s += "-" + shortNumber(v.asDouble(1));
          s += " mm";
          if (v.asDouble(2) > 0) {
            s += " " + fNumber(v.asDouble(2));
            if (v.asDouble(3) > 0 && v.asDouble(3) != v.asDouble(2)) s += "-" + shortNumber(v.asDouble(3));
          }
          return s;
        }
        break;
      case 0xa460:
        return choose(kCompositeImage, first());
      default:
        break;
    }
  } catch (const Error&) {
  }
  return plain(v);
}

}  // namespace lumenlib

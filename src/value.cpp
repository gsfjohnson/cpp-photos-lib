#include <photos/error.hpp>
#include <photos/value.hpp>

#include "bytes.hpp"
#include "strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace photos {

double Rational::toDouble() const noexcept {
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

std::size_t typeSize(TypeId type) noexcept {
  switch (type) {
    case TypeId::unsignedByte:
    case TypeId::asciiString:
    case TypeId::signedByte:
    case TypeId::undefined:
      return 1;
    case TypeId::unsignedShort:
    case TypeId::signedShort:
      return 2;
    case TypeId::unsignedLong:
    case TypeId::signedLong:
    case TypeId::tiffFloat:
    case TypeId::tiffIfd:
      return 4;
    case TypeId::unsignedRational:
    case TypeId::signedRational:
    case TypeId::tiffDouble:
      return 8;
  }
  return 0;
}

const char* typeName(TypeId type) noexcept {
  switch (type) {
    case TypeId::unsignedByte:
      return "Byte";
    case TypeId::asciiString:
      return "Ascii";
    case TypeId::unsignedShort:
      return "Short";
    case TypeId::unsignedLong:
      return "Long";
    case TypeId::unsignedRational:
      return "Rational";
    case TypeId::signedByte:
      return "SByte";
    case TypeId::undefined:
      return "Undefined";
    case TypeId::signedShort:
      return "SShort";
    case TypeId::signedLong:
      return "SLong";
    case TypeId::signedRational:
      return "SRational";
    case TypeId::tiffFloat:
      return "Float";
    case TypeId::tiffDouble:
      return "Double";
    case TypeId::tiffIfd:
      return "Ifd";
  }
  return "Unknown";
}

bool isValidType(std::uint16_t type) noexcept { return type >= 1 && type <= 13; }

namespace {

enum class Storage { bytes, ints, rationals, reals };

Storage storageOf(TypeId type) {
  switch (type) {
    case TypeId::unsignedByte:
    case TypeId::asciiString:
    case TypeId::signedByte:
    case TypeId::undefined:
      return Storage::bytes;
    case TypeId::unsignedShort:
    case TypeId::signedShort:
    case TypeId::unsignedLong:
    case TypeId::signedLong:
    case TypeId::tiffIfd:
      return Storage::ints;
    case TypeId::unsignedRational:
    case TypeId::signedRational:
      return Storage::rationals;
    case TypeId::tiffFloat:
    case TypeId::tiffDouble:
      return Storage::reals;
  }
  throw Error(ErrorCode::invalidArgument, "unknown TIFF type");
}

void checkRange(TypeId type, std::int64_t v) {
  std::int64_t lo = 0, hi = 0;
  switch (type) {
    case TypeId::unsignedByte:
      hi = 0xff;
      break;
    case TypeId::signedByte:
      lo = -128;
      hi = 127;
      break;
    case TypeId::unsignedShort:
      hi = 0xffff;
      break;
    case TypeId::signedShort:
      lo = -32768;
      hi = 32767;
      break;
    case TypeId::unsignedLong:
    case TypeId::tiffIfd:
      hi = 0xffffffffLL;
      break;
    case TypeId::signedLong:
      lo = std::numeric_limits<std::int32_t>::min();
      hi = std::numeric_limits<std::int32_t>::max();
      break;
    case TypeId::undefined:
      lo = -128;
      hi = 0xff;
      break;
    default:
      return;
  }
  if (v < lo || v > hi) {
    throw Error(ErrorCode::invalidArgument, std::to_string(v) + " is out of range for " + typeName(type));
  }
}

void checkRational(TypeId type, const Rational& r) {
  if (type == TypeId::unsignedRational) {
    if (r.numerator < 0 || r.denominator < 0 || r.numerator > 0xffffffffLL || r.denominator > 0xffffffffLL) {
      throw Error(ErrorCode::invalidArgument, "rational out of range for Rational");
    }
  } else {
    constexpr std::int64_t lo = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t hi = std::numeric_limits<std::int32_t>::max();
    if (r.numerator < lo || r.numerator > hi || r.denominator < lo || r.denominator > hi) {
      throw Error(ErrorCode::invalidArgument, "rational out of range for SRational");
    }
  }
}

// The closest fraction whose terms fit the type (continued fractions).
Rational toRationalApprox(double v, bool isSigned) {
  if (!std::isfinite(v)) return {0, 1};
  if (!isSigned && v < 0) v = 0;
  const double limit = isSigned ? 2147483647.0 : 4294967295.0;
  const bool negative = v < 0;
  double x = std::fabs(v);
  if (x > limit) return {negative ? -static_cast<std::int64_t>(limit) : static_cast<std::int64_t>(limit), 1};
  // Exact for integers and short decimals.
  if (x == std::floor(x)) return {static_cast<std::int64_t>(negative ? -x : x), 1};
  std::int64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
  double f = x;
  for (int i = 0; i < 64; ++i) {
    const double a = std::floor(f);
    const double h2 = a * static_cast<double>(h1) + static_cast<double>(h0);
    const double k2 = a * static_cast<double>(k1) + static_cast<double>(k0);
    if (h2 > limit || k2 > limit) break;
    h0 = h1;
    h1 = static_cast<std::int64_t>(h2);
    k0 = k1;
    k1 = static_cast<std::int64_t>(k2);
    const double approx = static_cast<double>(h1) / static_cast<double>(k1);
    if (std::fabs(approx - x) < 1e-12 * std::max(1.0, x)) break;
    const double rem = f - a;
    if (rem < 1e-15) break;
    f = 1.0 / rem;
  }
  if (k1 == 0) return {0, 1};
  return {negative ? -h1 : h1, k1};
}

std::string formatReal(double v) {
  std::ostringstream os;
  os.imbue(std::locale::classic());
  os << v;
  return os.str();
}

}  // namespace

Value::Value(TypeId type) : type_(type) { storageOf(type); }

Value Value::ascii(std::string_view text) {
  Value v(TypeId::asciiString);
  v.bytes_.assign(text.begin(), text.end());
  v.bytes_.push_back(0);
  return v;
}

Value Value::bytes(TypeId type, Bytes data) {
  if (storageOf(type) != Storage::bytes) {
    throw Error(ErrorCode::invalidArgument, std::string("Value::bytes: not a byte type: ") + typeName(type));
  }
  Value v(type);
  v.bytes_ = std::move(data);
  return v;
}

Value Value::integers(TypeId type, const std::vector<std::int64_t>& components) {
  Value v(type);
  for (auto c : components) checkRange(type, c);
  switch (storageOf(type)) {
    case Storage::bytes:
      if (type == TypeId::asciiString) throw Error(ErrorCode::invalidArgument, "Value::integers: Ascii");
      for (auto c : components) v.bytes_.push_back(static_cast<std::uint8_t>(c));
      break;
    case Storage::ints:
      v.ints_ = components;
      break;
    case Storage::rationals:
      for (auto c : components) v.rationals_.push_back({c, 1});
      for (auto& r : v.rationals_) checkRational(type, r);
      break;
    case Storage::reals:
      for (auto c : components) v.reals_.push_back(static_cast<double>(c));
      break;
  }
  return v;
}

Value Value::rationals(TypeId type, const std::vector<Rational>& components) {
  if (storageOf(type) != Storage::rationals) {
    throw Error(ErrorCode::invalidArgument, std::string("Value::rationals: not a rational type: ") + typeName(type));
  }
  for (const auto& r : components) checkRational(type, r);
  Value v(type);
  v.rationals_ = components;
  return v;
}

Value Value::reals(TypeId type, const std::vector<double>& components) {
  Value v(type);
  switch (storageOf(type)) {
    case Storage::reals:
      v.reals_ = components;
      break;
    case Storage::rationals:
      for (double c : components) v.rationals_.push_back(toRationalApprox(c, type == TypeId::signedRational));
      break;
    default:
      throw Error(ErrorCode::invalidArgument, std::string("Value::reals: not a real type: ") + typeName(type));
  }
  return v;
}

Value Value::fromString(TypeId type, std::string_view text) {
  if (type == TypeId::asciiString) return ascii(text);
  const auto tokens = detail::splitWhitespace(text);
  switch (storageOf(type)) {
    case Storage::bytes:
    case Storage::ints: {
      std::vector<std::int64_t> ints;
      for (const auto& t : tokens) {
        auto n = detail::parseInt(t);
        if (!n) throw Error(ErrorCode::invalidArgument, "not an integer: '" + t + "'");
        ints.push_back(*n);
      }
      if (type == TypeId::undefined) {
        for (auto& n : ints) {
          checkRange(type, n);
          n &= 0xff;
        }
      }
      return integers(type, ints);
    }
    case Storage::rationals: {
      std::vector<Rational> rs;
      for (const auto& t : tokens) {
        const auto slash = t.find('/');
        if (slash != std::string::npos) {
          auto n = detail::parseInt(t.substr(0, slash));
          auto d = detail::parseInt(t.substr(slash + 1));
          if (!n || !d) throw Error(ErrorCode::invalidArgument, "not a rational: '" + t + "'");
          rs.push_back({*n, *d});
        } else {
          auto d = detail::parseDouble(t);
          if (!d) throw Error(ErrorCode::invalidArgument, "not a number: '" + t + "'");
          rs.push_back(toRationalApprox(*d, type == TypeId::signedRational));
        }
      }
      return rationals(type, rs);
    }
    case Storage::reals: {
      std::vector<double> ds;
      for (const auto& t : tokens) {
        auto d = detail::parseDouble(t);
        if (!d) throw Error(ErrorCode::invalidArgument, "not a number: '" + t + "'");
        ds.push_back(*d);
      }
      return reals(type, ds);
    }
  }
  return Value(type);
}

Value Value::fromBytes(TypeId type, const std::uint8_t* data, std::size_t size, std::size_t count, ByteOrder order) {
  const std::size_t unit = typeSize(type);
  if (unit == 0 || count > size / unit) {
    throw Error(ErrorCode::corruptData, "value data shorter than its count");
  }
  Value v(type);
  switch (type) {
    case TypeId::unsignedByte:
    case TypeId::asciiString:
    case TypeId::signedByte:
    case TypeId::undefined:
      v.bytes_.assign(data, data + count);
      break;
    case TypeId::unsignedShort:
      for (std::size_t i = 0; i < count; ++i) v.ints_.push_back(detail::get16(data + 2 * i, order));
      break;
    case TypeId::signedShort:
      for (std::size_t i = 0; i < count; ++i) {
        v.ints_.push_back(static_cast<std::int16_t>(detail::get16(data + 2 * i, order)));
      }
      break;
    case TypeId::unsignedLong:
    case TypeId::tiffIfd:
      for (std::size_t i = 0; i < count; ++i) v.ints_.push_back(detail::get32(data + 4 * i, order));
      break;
    case TypeId::signedLong:
      for (std::size_t i = 0; i < count; ++i) {
        v.ints_.push_back(static_cast<std::int32_t>(detail::get32(data + 4 * i, order)));
      }
      break;
    case TypeId::unsignedRational:
      for (std::size_t i = 0; i < count; ++i) {
        v.rationals_.push_back({detail::get32(data + 8 * i, order), detail::get32(data + 8 * i + 4, order)});
      }
      break;
    case TypeId::signedRational:
      for (std::size_t i = 0; i < count; ++i) {
        v.rationals_.push_back({static_cast<std::int32_t>(detail::get32(data + 8 * i, order)),
                                static_cast<std::int32_t>(detail::get32(data + 8 * i + 4, order))});
      }
      break;
    case TypeId::tiffFloat:
      for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t bits = detail::get32(data + 4 * i, order);
        float f;
        std::memcpy(&f, &bits, 4);
        v.reals_.push_back(f);
      }
      break;
    case TypeId::tiffDouble:
      for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t bits = detail::get64(data + 8 * i, order);
        double d;
        std::memcpy(&d, &bits, 8);
        v.reals_.push_back(d);
      }
      break;
  }
  return v;
}

std::size_t Value::count() const noexcept {
  switch (type_) {
    case TypeId::unsignedByte:
    case TypeId::asciiString:
    case TypeId::signedByte:
    case TypeId::undefined:
      return bytes_.size();
    case TypeId::unsignedRational:
    case TypeId::signedRational:
      return rationals_.size();
    case TypeId::tiffFloat:
    case TypeId::tiffDouble:
      return reals_.size();
    default:
      return ints_.size();
  }
}

std::int64_t Value::toInt64(std::size_t i) const {
  if (i >= count()) throw Error(ErrorCode::invalidArgument, "value component out of range");
  switch (storageOf(type_)) {
    case Storage::bytes:
      return type_ == TypeId::signedByte ? static_cast<std::int8_t>(bytes_[i]) : bytes_[i];
    case Storage::ints:
      return ints_[i];
    case Storage::rationals: {
      const auto& r = rationals_[i];
      return r.denominator == 0 ? 0 : r.numerator / r.denominator;
    }
    case Storage::reals: {
      const double d = reals_[i];
      if (!std::isfinite(d) || std::fabs(d) > 9.2e18) return 0;
      return static_cast<std::int64_t>(d);
    }
  }
  return 0;
}

double Value::toDouble(std::size_t i) const {
  if (i >= count()) throw Error(ErrorCode::invalidArgument, "value component out of range");
  switch (storageOf(type_)) {
    case Storage::rationals:
      return rationals_[i].toDouble();
    case Storage::reals:
      return reals_[i];
    default:
      return static_cast<double>(toInt64(i));
  }
}

Rational Value::toRational(std::size_t i) const {
  if (i >= count()) throw Error(ErrorCode::invalidArgument, "value component out of range");
  switch (storageOf(type_)) {
    case Storage::rationals:
      return rationals_[i];
    case Storage::reals:
      return toRationalApprox(reals_[i], true);
    default:
      return {toInt64(i), 1};
  }
}

std::string Value::toString(std::size_t i) const {
  if (i >= count()) throw Error(ErrorCode::invalidArgument, "value component out of range");
  switch (storageOf(type_)) {
    case Storage::bytes:
      if (type_ == TypeId::asciiString) return std::string(1, static_cast<char>(bytes_[i]));
      return std::to_string(toInt64(i));
    case Storage::ints:
      return std::to_string(ints_[i]);
    case Storage::rationals:
      return std::to_string(rationals_[i].numerator) + "/" + std::to_string(rationals_[i].denominator);
    case Storage::reals:
      return formatReal(reals_[i]);
  }
  return {};
}

std::string Value::toString() const {
  if (type_ == TypeId::asciiString) {
    std::size_t n = 0;
    while (n < bytes_.size() && bytes_[n] != 0) ++n;
    return std::string(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(n));
  }
  std::string out;
  const std::size_t n = count();
  for (std::size_t i = 0; i < n; ++i) {
    if (i) out += ' ';
    out += toString(i);
  }
  return out;
}

Bytes Value::toBytes(ByteOrder order) const {
  Bytes out;
  switch (type_) {
    case TypeId::unsignedByte:
    case TypeId::asciiString:
    case TypeId::signedByte:
    case TypeId::undefined:
      return bytes_;
    case TypeId::unsignedShort:
    case TypeId::signedShort:
      for (auto v : ints_) detail::append16(out, static_cast<std::uint16_t>(v), order);
      break;
    case TypeId::unsignedLong:
    case TypeId::signedLong:
    case TypeId::tiffIfd:
      for (auto v : ints_) detail::append32(out, static_cast<std::uint32_t>(v), order);
      break;
    case TypeId::unsignedRational:
    case TypeId::signedRational:
      for (const auto& r : rationals_) {
        detail::append32(out, static_cast<std::uint32_t>(r.numerator), order);
        detail::append32(out, static_cast<std::uint32_t>(r.denominator), order);
      }
      break;
    case TypeId::tiffFloat:
      for (double d : reals_) {
        const float f = static_cast<float>(d);
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        detail::append32(out, bits, order);
      }
      break;
    case TypeId::tiffDouble:
      for (double d : reals_) {
        std::uint64_t bits;
        std::memcpy(&bits, &d, 8);
        const auto hi = static_cast<std::uint32_t>(bits >> 32);
        const auto lo = static_cast<std::uint32_t>(bits);
        if (order == ByteOrder::littleEndian) {
          detail::append32(out, lo, order);
          detail::append32(out, hi, order);
        } else {
          detail::append32(out, hi, order);
          detail::append32(out, lo, order);
        }
      }
      break;
  }
  return out;
}

bool operator==(const Value& a, const Value& b) noexcept {
  return a.type_ == b.type_ && a.bytes_ == b.bytes_ && a.ints_ == b.ints_ && a.rationals_ == b.rationals_ &&
         a.reals_ == b.reals_;
}

}  // namespace photos

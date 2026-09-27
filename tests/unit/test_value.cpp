#include "testing.hpp"

using photos::ByteOrder;
using photos::ErrorCode;
using photos::Rational;
using photos::TypeId;
using photos::Value;

TEST(value_ascii) {
  const Value v = Value::ascii("Canon");
  CHECK_EQ(v.count(), 6u);  // with the NUL
  CHECK_EQ(v.toString(), "Canon");
  CHECK_EQ(v.toBytes(ByteOrder::littleEndian).back(), 0);
  CHECK_EQ(Value::fromString(TypeId::asciiString, "x y").toString(), "x y");
}

TEST(value_integers_round_trip_both_byte_orders) {
  for (auto order : {ByteOrder::littleEndian, ByteOrder::bigEndian}) {
    const Value v = Value::integers(TypeId::unsignedShort, {1, 65535, 300});
    const auto bytes = v.toBytes(order);
    CHECK_EQ(bytes.size(), 6u);
    const Value back = Value::fromBytes(TypeId::unsignedShort, bytes.data(), bytes.size(), 3, order);
    CHECK(back == v);
    CHECK_EQ(back.toString(), "1 65535 300");

    const Value s = Value::integers(TypeId::signedLong, {-5, 2147483647});
    const auto sb = s.toBytes(order);
    CHECK(Value::fromBytes(TypeId::signedLong, sb.data(), sb.size(), 2, order) == s);
  }
}

TEST(value_range_is_checked) {
  CHECK_THROWS(Value::integers(TypeId::unsignedShort, {65536}), ErrorCode::invalidArgument);
  CHECK_THROWS(Value::integers(TypeId::unsignedByte, {-1}), ErrorCode::invalidArgument);
  CHECK_THROWS(Value::integers(TypeId::signedShort, {40000}), ErrorCode::invalidArgument);
  CHECK_THROWS(Value::fromString(TypeId::unsignedShort, "12 abc"), ErrorCode::invalidArgument);
}

TEST(value_rationals) {
  const Value v = Value::fromString(TypeId::unsignedRational, "1/250 28/10");
  CHECK_EQ(v.count(), 2u);
  CHECK_EQ(v.toString(), "1/250 28/10");
  CHECK_NEAR(v.toDouble(0), 0.004, 1e-12);
  CHECK_NEAR(v.toDouble(1), 2.8, 1e-12);
  CHECK_EQ(v.toInt64(1), 2);

  // Decimals become the closest fraction.
  const Value d = Value::fromString(TypeId::unsignedRational, "2.8");
  CHECK_EQ(d.toRational().numerator, 14);
  CHECK_EQ(d.toRational().denominator, 5);
  const Value s = Value::fromString(TypeId::signedRational, "-0.333333333333");
  CHECK_NEAR(s.toDouble(), -1.0 / 3.0, 1e-9);

  // A zero denominator reads as 0, not a crash.
  const Value z = Value::rationals(TypeId::unsignedRational, {Rational{5, 0}});
  CHECK_EQ(z.toDouble(), 0.0);
  CHECK_EQ(z.toInt64(), 0);
  CHECK_THROWS(Value::rationals(TypeId::unsignedRational, {Rational{-1, 2}}), ErrorCode::invalidArgument);
}

TEST(value_reals) {
  for (auto order : {ByteOrder::littleEndian, ByteOrder::bigEndian}) {
    const Value d = Value::reals(TypeId::tiffDouble, {1.5, -2.25});
    const auto b = d.toBytes(order);
    CHECK(Value::fromBytes(TypeId::tiffDouble, b.data(), b.size(), 2, order) == d);
    const Value f = Value::reals(TypeId::tiffFloat, {0.5f});
    const auto fb = f.toBytes(order);
    CHECK_NEAR(Value::fromBytes(TypeId::tiffFloat, fb.data(), fb.size(), 1, order).toDouble(), 0.5, 1e-9);
  }
}

TEST(value_undefined_prints_decimal_bytes) {
  const Value v = Value::fromString(TypeId::undefined, "48 50 51 50");
  CHECK_EQ(v.count(), 4u);
  CHECK_EQ(v.toString(), "48 50 51 50");
  CHECK_EQ(v.rawBytes()[0], '0');
}

TEST(value_short_data_is_rejected) {
  const std::uint8_t data[3] = {1, 2, 3};
  CHECK_THROWS(Value::fromBytes(TypeId::unsignedLong, data, sizeof data, 1, ByteOrder::littleEndian),
               ErrorCode::corruptData);
  CHECK_THROWS(Value().toInt64(0), ErrorCode::invalidArgument);
}

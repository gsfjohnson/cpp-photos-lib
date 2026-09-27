#include "testing.hpp"

using lumenlib::ByteOrder;
using lumenlib::ErrorCode;
using lumenlib::FieldType;
using lumenlib::FieldValue;
using lumenlib::Rational;

TEST(value_ascii) {
  const FieldValue v = FieldValue::ascii("Canon");
  CHECK_EQ(v.count(), 6u);  // with the NUL
  CHECK_EQ(v.text(), "Canon");
  CHECK_EQ(v.encode(ByteOrder::little).back(), 0);
  CHECK_EQ(FieldValue::parse(FieldType::ascii, "x y").text(), "x y");
}

TEST(value_type_names_are_the_specification_s) {
  CHECK_EQ(std::string(lumenlib::fieldTypeName(FieldType::u16)), "SHORT");
  CHECK_EQ(std::string(lumenlib::fieldTypeName(FieldType::srational)), "SRATIONAL");
  CHECK_EQ(std::string(lumenlib::fieldTypeName(FieldType::undefined)), "UNDEFINED");
  CHECK_EQ(lumenlib::fieldTypeSize(FieldType::f64), 8u);
  CHECK(lumenlib::isFieldType(13));
  CHECK(!lumenlib::isFieldType(14));
}

TEST(value_integers_round_trip_both_byte_orders) {
  for (auto order : {ByteOrder::little, ByteOrder::big}) {
    const FieldValue v = FieldValue::integers(FieldType::u16, {1, 65535, 300});
    const auto bytes = v.encode(order);
    CHECK_EQ(bytes.size(), 6u);
    const FieldValue back = FieldValue::decode(FieldType::u16, bytes.data(), bytes.size(), 3, order);
    CHECK(back == v);
    CHECK_EQ(back.text(), "1 65535 300");

    const FieldValue s = FieldValue::integers(FieldType::i32, {-5, 2147483647});
    const auto sb = s.encode(order);
    CHECK(FieldValue::decode(FieldType::i32, sb.data(), sb.size(), 2, order) == s);
  }
}

TEST(value_range_is_checked) {
  CHECK_THROWS(FieldValue::integers(FieldType::u16, {65536}), ErrorCode::invalidArgument);
  CHECK_THROWS(FieldValue::integers(FieldType::u8, {-1}), ErrorCode::invalidArgument);
  CHECK_THROWS(FieldValue::integers(FieldType::i16, {40000}), ErrorCode::invalidArgument);
  CHECK_THROWS(FieldValue::parse(FieldType::u16, "12 abc"), ErrorCode::invalidArgument);
}

TEST(value_rationals) {
  const FieldValue v = FieldValue::parse(FieldType::urational, "1/250 28/10");
  CHECK_EQ(v.count(), 2u);
  CHECK_EQ(v.text(), "1/250 28/10");
  CHECK_NEAR(v.asDouble(0), 0.004, 1e-12);
  CHECK_NEAR(v.asDouble(1), 2.8, 1e-12);
  CHECK_EQ(v.asInt(1), 2);

  // Decimals become the closest fraction.
  const FieldValue d = FieldValue::parse(FieldType::urational, "2.8");
  CHECK_EQ(d.asRational().numerator, 14);
  CHECK_EQ(d.asRational().denominator, 5);
  const FieldValue s = FieldValue::parse(FieldType::srational, "-0.333333333333");
  CHECK_NEAR(s.asDouble(), -1.0 / 3.0, 1e-9);

  // A zero denominator reads as 0, not a crash.
  const FieldValue z = FieldValue::rationals(FieldType::urational, {Rational{5, 0}});
  CHECK_EQ(z.asDouble(), 0.0);
  CHECK_EQ(z.asInt(), 0);
  CHECK_THROWS(FieldValue::rationals(FieldType::urational, {Rational{-1, 2}}), ErrorCode::invalidArgument);
}

TEST(value_reals) {
  for (auto order : {ByteOrder::little, ByteOrder::big}) {
    const FieldValue d = FieldValue::reals(FieldType::f64, {1.5, -2.25});
    const auto b = d.encode(order);
    CHECK(FieldValue::decode(FieldType::f64, b.data(), b.size(), 2, order) == d);
    const FieldValue f = FieldValue::reals(FieldType::f32, {0.5f});
    const auto fb = f.encode(order);
    CHECK_NEAR(FieldValue::decode(FieldType::f32, fb.data(), fb.size(), 1, order).asDouble(), 0.5, 1e-9);
  }
}

TEST(value_undefined_prints_decimal_bytes) {
  const FieldValue v = FieldValue::parse(FieldType::undefined, "48 50 51 50");
  CHECK_EQ(v.count(), 4u);
  CHECK_EQ(v.text(), "48 50 51 50");
  CHECK_EQ(v.bytes()[0], '0');
}

TEST(value_short_data_is_rejected) {
  const std::uint8_t data[3] = {1, 2, 3};
  CHECK_THROWS(FieldValue::decode(FieldType::u32, data, sizeof data, 1, ByteOrder::little), ErrorCode::corruptData);
  CHECK_THROWS(FieldValue().asInt(0), ErrorCode::invalidArgument);
}

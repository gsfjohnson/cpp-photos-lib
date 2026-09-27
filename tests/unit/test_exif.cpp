#include "testing.hpp"

#include <algorithm>

using namespace lumenlib;

namespace {

ExifMetadata decode(const Bytes& tiff) { return ExifMetadata::decode(tiff); }

Bytes encode(const ExifMetadata& exif) { return exif.encode(); }

void put16(Bytes& b, std::size_t at, std::uint16_t v) {
  b[at] = static_cast<std::uint8_t>(v);
  b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(Bytes& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// A little-endian TIFF block: IFD0 with Make and Orientation and a pointer to
// an Exif IFD holding a maker note whose contents reference an absolute
// offset (as many cameras' do). The maker note is 16 bytes at offset 100.
Bytes blockWithMakerNote() {
  Bytes b(140, 0);
  b[0] = 'I';
  b[1] = 'I';
  put16(b, 2, 42);
  put32(b, 4, 8);
  // IFD0 at 8: 3 entries.
  put16(b, 8, 3);
  put16(b, 10, 0x010f);  // Make, ASCII, 6 bytes at 80
  put16(b, 12, 2);
  put32(b, 14, 6);
  put32(b, 18, 80);
  put16(b, 22, 0x0112);  // Orientation, SHORT, 1
  put16(b, 24, 3);
  put32(b, 26, 1);
  put16(b, 30, 1);
  put16(b, 34, 0x8769);  // ExifIFDPointer -> 50
  put16(b, 36, 4);
  put32(b, 38, 1);
  put32(b, 42, 50);
  put32(b, 46, 0);  // no IFD1
  // Exif IFD at 50: 1 entry.
  put16(b, 50, 1);
  put16(b, 52, 0x927c);  // MakerNote, UNDEFINED, 16 bytes at 100
  put16(b, 54, 7);
  put32(b, 56, 16);
  put32(b, 60, 100);
  put32(b, 64, 0);
  const char make[] = "Maker";
  std::copy(make, make + 6, b.begin() + 80);
  // Maker note: a magic and an absolute offset back into the block.
  const char magic[] = "MKNOTE";
  std::copy(magic, magic + 6, b.begin() + 100);
  put32(b, 108, 80);
  return b;
}

}  // namespace

TEST(exif_tags_by_ifd_and_specification_name) {
  const ExifTag t("exif.DateTimeOriginal");
  CHECK(t.ifd() == Ifd::exif);
  CHECK_EQ(t.number(), 0x9003);
  CHECK_EQ(t.str(), "exif.DateTimeOriginal");
  CHECK_EQ(ExifTag(Ifd::gps, 0x0002).str(), "gps.GPSLatitude");
  CHECK_EQ(ExifTag(Ifd::ifd1, 0x0103).str(), "ifd1.Compression");
  CHECK_EQ(ExifTag(Ifd::exif, 0x8827).str(), "exif.PhotographicSensitivity");
  CHECK_EQ(ExifTag(Ifd::ifd0, 0x8769).str(), "ifd0.ExifIFDPointer");
  CHECK_EQ(ExifTag("ifd0.0xabcd").number(), 0xabcd);
  CHECK_EQ(ExifTag(Ifd::ifd0, 0xabcd).str(), "ifd0.0xabcd");
  CHECK_THROWS(ExifTag("nope.Make"), ErrorCode::invalidArgument);
  CHECK_THROWS(ExifTag("ifd0.NoSuchTag"), ErrorCode::invalidArgument);
  CHECK_THROWS(ExifTag("NoSuchTag"), ErrorCode::invalidArgument);
  CHECK(!ExifTag::parse("ifd0."));
}

TEST(exif_bare_names_find_their_ifd) {
  CHECK(ExifTag("Make") == ExifTag(Ifd::ifd0, 0x010f));
  CHECK(ExifTag("DateTimeOriginal") == ExifTag(Ifd::exif, 0x9003));  // not TIFF/EP's IFD0 copy
  CHECK(ExifTag("GPSLatitude") == ExifTag(Ifd::gps, 0x0002));
  CHECK(ExifTag("InteroperabilityIndex") == ExifTag(Ifd::interop, 0x0001));
  CHECK(ExifTag("CFAPattern") == ExifTag(Ifd::exif, 0xa302));
}

TEST(exif_setters_use_the_tag_type) {
  ExifMetadata exif;
  exif.setText("ifd0.Make", "PhotoCo");
  exif.setInt("ifd0.Orientation", 6);
  exif.setRational("exif.ExposureTime", Rational{1, 250});
  exif.setText("exif.FNumber", "2.8");
  exif.setText("exif.PhotographicSensitivity", "100");
  CHECK(exif.find("ifd0.Make")->type() == FieldType::ascii);
  CHECK(exif.find("Orientation")->type() == FieldType::u16);
  CHECK(exif.find("exif.ExposureTime")->type() == FieldType::urational);
  CHECK_EQ(exif.find("exif.FNumber")->text(), "14/5");
  CHECK_EQ(exif.find("exif.PhotographicSensitivity")->asInt(), 100);
  CHECK_EQ(exif.size(), 5u);
  CHECK(exif.find("ifd0.Model") == nullptr);
  CHECK(exif.find("not a tag") == nullptr);
  CHECK_EQ(exif.remove("ifd0.Make"), 1u);
  CHECK(!exif.contains("ifd0.Make"));
  // Bad text changes nothing.
  CHECK_THROWS(exif.setText("ifd0.Orientation", "sideways"), ErrorCode::invalidArgument);
  CHECK_EQ(exif.find("ifd0.Orientation")->asInt(), 6);
}

TEST(exif_set_replaces_duplicates) {
  ExifMetadata exif;
  exif.append(ExifTag(Ifd::ifd0, 0x0110), FieldValue::ascii("one"));
  exif.append(ExifTag(Ifd::ifd0, 0x0110), FieldValue::ascii("two"));
  exif.set("ifd0.Model", FieldValue::ascii("three"));
  CHECK_EQ(exif.size(), 1u);
  CHECK_EQ(exif.find("ifd0.Model")->text(), "three");
  exif.entry("ifd0.Artist").setText("Ada");
  CHECK_EQ(exif.find("ifd0.Artist")->text(), "Ada");
  CHECK_EQ(exif.removeIf([](const ExifEntry& e) { return e.ifd() == Ifd::ifd0; }), 2u);
}

TEST(exif_user_comment_is_encoded) {
  ExifMetadata exif;
  exif.setText("exif.UserComment", "hi");
  const Bytes& raw = exif.find("exif.UserComment")->value().bytes();
  CHECK_EQ(raw.size(), 10u);
  CHECK(std::equal(raw.begin(), raw.begin() + 5, "ASCII"));
  CHECK_EQ(exif.find("exif.UserComment")->describe(), "hi");
  exif.setText("exif.UserComment", testing::kKobenhavn);
  CHECK_EQ(exif.find("exif.UserComment")->describe(), testing::kKobenhavn);
}

TEST(exif_values_described) {
  ExifMetadata exif;
  const auto describe = [&](const char* tag, const char* text) {
    exif.setText(tag, text);
    return exif.find(tag)->describe();
  };
  CHECK_EQ(describe("exif.ExposureTime", "1/250"), "1/250 s");
  CHECK_EQ(describe("exif.ExposureTime", "2/1"), "2 s");
  CHECK_EQ(describe("exif.FNumber", "28/10"), "F2.8");
  CHECK_EQ(describe("exif.FocalLength", "50/1"), "50 mm");
  CHECK_EQ(describe("exif.ExposureBiasValue", "-1/3"), "-1/3 EV");
  CHECK_EQ(describe("exif.ExposureBiasValue", "-4/3"), "-1 1/3 EV");
  CHECK_EQ(describe("exif.ExposureBiasValue", "6/6"), "+1 EV");
  CHECK_EQ(describe("exif.ExposureBiasValue", "2/3"), "+2/3 EV");
  CHECK_EQ(describe("ifd0.Orientation", "6"), "Rotated 90\xc2\xb0 clockwise");
  CHECK_EQ(describe("exif.Flash", "25"), "Fired, auto mode");
  CHECK_EQ(describe("exif.Flash", "16"), "Did not fire, suppressed");
  CHECK_EQ(describe("exif.MeteringMode", "5"), "Pattern");
  CHECK_EQ(describe("exif.ExposureProgram", "3"), "Aperture priority");
  CHECK_EQ(describe("exif.ColorSpace", "65535"), "Uncalibrated");
  CHECK_EQ(describe("exif.WhiteBalance", "0"), "Auto");
  CHECK_EQ(describe("exif.MeteringMode", "77"), "(77)");
  CHECK_EQ(describe("gps.GPSLatitude", "55/1 40/1 3399/100"), "55\xc2\xb0 40' 33.99\"");
  CHECK_EQ(describe("gps.GPSLatitudeRef", "N"), "North");
  CHECK_EQ(describe("gps.GPSAltitudeRef", "1"), "Below sea level");
  CHECK_EQ(describe("gps.GPSVersionID", "2 3 0 0"), "2.3.0.0");
  CHECK_EQ(describe("exif.LensSpecification", "18/1 55/1 35/10 56/10"), "18-55 mm F3.5-5.6");
  exif.set("exif.ExifVersion", FieldValue::fromBytes(FieldType::undefined, testing::bytesOf("0232")));
  CHECK_EQ(exif.find("exif.ExifVersion")->describe(), "2.32");
  exif.set("exif.MakerNote", FieldValue::fromBytes(FieldType::undefined, Bytes(100, 1)));
  CHECK_EQ(exif.find("exif.MakerNote")->describe(), "(100 bytes)");
}

TEST(exif_encode_decode_round_trip) {
  for (auto order : {ByteOrder::little, ByteOrder::big}) {
    ExifMetadata exif;
    exif.setByteOrder(order);
    exif.setText("ifd0.Make", "PhotoCo");
    exif.setText("ifd0.Model", "Model X");
    exif.setInt("ifd0.Orientation", 8);
    exif.setText("exif.DateTimeOriginal", "2024:05:17 18:42:07");
    exif.setRational("exif.ExposureTime", Rational{1, 60});
    exif.setText("exif.ExposureBiasValue", "-1/3");
    exif.setText("gps.GPSLatitude", "55/1 40/1 3399/100");
    exif.setText("gps.GPSLatitudeRef", "N");
    exif.setText("interop.InteroperabilityIndex", "R98");
    exif.setThumbnail(Bytes{0xff, 0xd8, 0xff, 0xd9});

    const Bytes tiff = encode(exif);
    ExifMetadata back = decode(tiff);
    CHECK(back.byteOrder() == order);
    CHECK_EQ(back.size(), exif.size());
    for (const auto& e : exif) {
      const auto* b = back.find(e.tag());
      CHECK(b != nullptr);
      CHECK(b->value() == e.value());
    }
    CHECK(back.thumbnail() == exif.thumbnail());
    CHECK_EQ(back.find("ifd1.Compression")->asInt(), 6);
    // With the prefix JPEG and WebP writers use, too.
    CHECK_EQ(ExifMetadata::decode(testing::concat({testing::bytesOf(std::string("Exif\0\0", 6)), tiff})).size(),
             exif.size());
  }
}

TEST(exif_unchanged_block_is_written_back_as_is) {
  const Bytes block = blockWithMakerNote();
  ExifMetadata exif = decode(block);
  CHECK_EQ(exif.size(), 3u);  // Make, Orientation, MakerNote
  CHECK(encode(exif) == block);
}

TEST(exif_same_size_edit_patches_in_place) {
  const Bytes block = blockWithMakerNote();
  ExifMetadata exif = decode(block);
  exif.setInt("ifd0.Orientation", 6);
  exif.setText("ifd0.Make", "Mkr");  // shorter fits too
  const Bytes out = encode(exif);
  CHECK_EQ(out.size(), block.size());
  // The maker note, and its internal offset, are where they were.
  CHECK(std::equal(block.begin() + 100, block.begin() + 116, out.begin() + 100));
  ExifMetadata back = decode(out);
  CHECK_EQ(back.find("ifd0.Orientation")->asInt(), 6);
  CHECK_EQ(back.find("ifd0.Make")->text(), "Mkr");
}

TEST(exif_structural_edit_rewrites) {
  const Bytes block = blockWithMakerNote();
  ExifMetadata exif = decode(block);
  exif.setText("ifd0.Model", "A new tag");
  const Bytes out = encode(exif);
  CHECK(out != block);
  ExifMetadata back = decode(out);
  CHECK_EQ(back.size(), 4u);
  CHECK_EQ(back.find("ifd0.Model")->text(), "A new tag");
  CHECK_EQ(back.find("exif.MakerNote")->count(), 16u);
}

TEST(exif_empty_encodes_to_nothing) {
  ExifMetadata exif;
  CHECK(encode(exif).empty());
  exif.entry("ifd0.Make");  // created, never set
  CHECK(encode(exif).empty());
}

TEST(exif_loops_and_bad_offsets_are_survived) {
  std::vector<std::string> warnings;
  setWarningHandler([&](const std::string& w) { warnings.push_back(w); });

  Bytes b = blockWithMakerNote();
  put32(b, 42, 8);  // the Exif IFD pointer points back at IFD0
  ExifMetadata exif = decode(b);
  CHECK_EQ(exif.size(), 2u);

  Bytes c = blockWithMakerNote();
  put32(c, 18, 0xfffffff0);  // Make's value far past the end
  ExifMetadata exif2 = decode(c);
  CHECK(exif2.find("ifd0.Make") == nullptr);
  CHECK(exif2.find("ifd0.Orientation") != nullptr);
  CHECK(!warnings.empty());

  Bytes d = blockWithMakerNote();
  put32(d, 14, 0x40000000);  // a count whose size overflows 32 bits
  ExifMetadata exif3 = decode(d);
  CHECK(exif3.find("ifd0.Make") == nullptr);

  // Not TIFF at all.
  CHECK_THROWS(decode(Bytes{'X', 'X', 0, 0, 0, 0, 0, 0}), ErrorCode::corruptData);
  setWarningHandler(nullptr);
}

TEST(exif_structural_tags_come_from_the_layout) {
  ExifMetadata exif;
  exif.setText("ifd0.Make", "PhotoCo");
  exif.setInt("ifd0.ExifIFDPointer", 12345);  // a bogus pointer
  exif.setInt("exif.PhotographicSensitivity", 100);
  exif.setInt("ifd1.JPEGInterchangeFormat", 999);
  const ExifMetadata back = decode(encode(exif));
  CHECK_EQ(back.find("exif.PhotographicSensitivity")->asInt(), 100);
  CHECK(back.find("ifd0.ExifIFDPointer") == nullptr);
  CHECK(back.find("ifd1.JPEGInterchangeFormat") == nullptr);
}

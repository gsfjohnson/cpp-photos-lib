#include "testing.hpp"

#include <algorithm>

using namespace photos;

namespace {

// The Exif codec is reached through a minimal JPEG (SOI, APP1, SOS, EOI), so
// these tests use only the public API.
Bytes jpegWith(const Bytes& tiff) {
  Bytes j = {0xff, 0xd8};
  if (!tiff.empty()) {
    const std::size_t n = tiff.size() + 8;
    j.insert(j.end(), {0xff, 0xe1, static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)});
    const char id[] = "Exif\0";
    j.insert(j.end(), id, id + 6);
    j.insert(j.end(), tiff.begin(), tiff.end());
  }
  j.insert(j.end(), {0xff, 0xda, 0x00, 0x02, 0x00, 0xff, 0xd9});
  return j;
}

ExifData decode(const Bytes& tiff) {
  auto image = Image::open(jpegWith(tiff));
  image->readMetadata();
  return image->exifData();
}

Bytes encode(const ExifData& exif) { return testing::tiffOf(exif); }

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
  put16(b, 10, 0x010f);  // Make, Ascii, 6 bytes at 80
  put16(b, 12, 2);
  put32(b, 14, 6);
  put32(b, 18, 80);
  put16(b, 22, 0x0112);  // Orientation, Short, 1
  put16(b, 24, 3);
  put32(b, 26, 1);
  put16(b, 30, 1);
  put16(b, 34, 0x8769);  // ExifTag -> 50
  put16(b, 36, 4);
  put32(b, 38, 1);
  put32(b, 42, 50);
  put32(b, 46, 0);  // no IFD1
  // Exif IFD at 50: 1 entry.
  put16(b, 50, 1);
  put16(b, 52, 0x927c);  // MakerNote, Undefined, 16 bytes at 100
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

TEST(exif_keys) {
  const ExifKey k("Exif.Photo.DateTimeOriginal");
  CHECK(k.ifd() == IfdId::exif);
  CHECK_EQ(k.tag(), 0x9003);
  CHECK_EQ(k.str(), "Exif.Photo.DateTimeOriginal");
  CHECK_EQ(ExifKey(IfdId::gps, 0x0002).str(), "Exif.GPSInfo.GPSLatitude");
  CHECK_EQ(ExifKey(IfdId::ifd1, 0x0103).str(), "Exif.Thumbnail.Compression");
  CHECK_EQ(ExifKey("Exif.Image.0xabcd").tag(), 0xabcd);
  CHECK_EQ(ExifKey(IfdId::ifd0, 0xabcd).str(), "Exif.Image.0xabcd");
  CHECK_THROWS(ExifKey("Exif.Nope.Make"), ErrorCode::invalidArgument);
  CHECK_THROWS(ExifKey("Exif.Image.NoSuchTag"), ErrorCode::invalidArgument);
  CHECK_THROWS(ExifKey("Iptc.Image.Make"), ErrorCode::invalidArgument);
}

TEST(exif_assignment_uses_the_tag_type) {
  ExifData exif;
  exif["Exif.Image.Make"] = "PhotoCo";
  exif["Exif.Image.Orientation"] = std::uint16_t{6};
  exif["Exif.Photo.ExposureTime"] = Rational{1, 250};
  exif["Exif.Photo.FNumber"] = "2.8";
  exif["Exif.Photo.ISOSpeedRatings"] = "100";
  CHECK(exif["Exif.Image.Make"].typeId() == TypeId::asciiString);
  CHECK(exif["Exif.Image.Orientation"].typeId() == TypeId::unsignedShort);
  CHECK(exif["Exif.Photo.ExposureTime"].typeId() == TypeId::unsignedRational);
  CHECK_EQ(exif["Exif.Photo.FNumber"].toString(), "14/5");
  CHECK_EQ(exif["Exif.Photo.ISOSpeedRatings"].toInt64(), 100);
  CHECK_EQ(exif.size(), 5u);
  CHECK(exif.find("Exif.Image.Model") == nullptr);
  CHECK_EQ(exif.erase("Exif.Image.Make"), 1u);
  CHECK(exif.find("Exif.Image.Make") == nullptr);
}

TEST(exif_user_comment_is_encoded) {
  ExifData exif;
  exif["Exif.Photo.UserComment"] = "hi";
  const Bytes& raw = exif["Exif.Photo.UserComment"].value().rawBytes();
  CHECK_EQ(raw.size(), 10u);
  CHECK(std::equal(raw.begin(), raw.begin() + 5, "ASCII"));
}

TEST(exif_encode_decode_round_trip) {
  for (auto order : {ByteOrder::littleEndian, ByteOrder::bigEndian}) {
    ExifData exif;
    exif.setByteOrder(order);
    exif["Exif.Image.Make"] = "PhotoCo";
    exif["Exif.Image.Model"] = "Model X";
    exif["Exif.Image.Orientation"] = std::uint16_t{8};
    exif["Exif.Photo.DateTimeOriginal"] = "2024:05:17 18:42:07";
    exif["Exif.Photo.ExposureTime"] = Rational{1, 60};
    exif["Exif.Photo.ExposureBiasValue"] = "-1/3";
    exif["Exif.GPSInfo.GPSLatitude"] = "55/1 40/1 3399/100";
    exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
    exif["Exif.Iop.InteroperabilityIndex"] = "R98";
    exif.setThumbnail(Bytes{0xff, 0xd8, 0xff, 0xd9});

    const Bytes tiff = encode(exif);
    ExifData back = decode(tiff);
    CHECK(back.byteOrder() == order);
    CHECK_EQ(back.size(), exif.size());
    for (const auto& d : exif) {
      const auto* b = back.find(d.key());
      CHECK(b != nullptr);
      CHECK(b->value() == d.value());
    }
    CHECK(back.thumbnail() == exif.thumbnail());
    CHECK_EQ(back["Exif.Thumbnail.Compression"].toInt64(), 6);
  }
}

TEST(exif_unchanged_block_is_written_back_as_is) {
  const Bytes block = blockWithMakerNote();
  ExifData exif = decode(block);
  CHECK_EQ(exif.size(), 3u);  // Make, Orientation, MakerNote
  CHECK(encode(exif) == block);
}

TEST(exif_same_size_edit_patches_in_place) {
  const Bytes block = blockWithMakerNote();
  ExifData exif = decode(block);
  exif["Exif.Image.Orientation"] = std::uint16_t{6};
  exif["Exif.Image.Make"] = "Mkr";  // shorter fits too
  const Bytes out = encode(exif);
  CHECK_EQ(out.size(), block.size());
  // The maker note, and its internal offset, are where they were.
  CHECK(std::equal(block.begin() + 100, block.begin() + 116, out.begin() + 100));
  ExifData back = decode(out);
  CHECK_EQ(back["Exif.Image.Orientation"].toInt64(), 6);
  CHECK_EQ(back["Exif.Image.Make"].toString(), "Mkr");
}

TEST(exif_structural_edit_rewrites) {
  const Bytes block = blockWithMakerNote();
  ExifData exif = decode(block);
  exif["Exif.Image.Model"] = "A new tag";
  const Bytes out = encode(exif);
  CHECK(out != block);
  ExifData back = decode(out);
  CHECK_EQ(back.size(), 4u);
  CHECK_EQ(back["Exif.Image.Model"].toString(), "A new tag");
  CHECK_EQ(back["Exif.Photo.MakerNote"].count(), 16u);
}

TEST(exif_empty_encodes_to_nothing) {
  ExifData exif;
  CHECK(encode(exif).empty());
  exif["Exif.Image.Make"];  // created, never set
  CHECK(encode(exif).empty());
}

TEST(exif_loops_and_bad_offsets_are_survived) {
  Bytes b = blockWithMakerNote();
  put32(b, 42, 8);  // the Exif IFD pointer points back at IFD0
  ExifData exif = decode(b);
  CHECK_EQ(exif.size(), 2u);

  Bytes c = blockWithMakerNote();
  put32(c, 18, 0xfffffff0);  // Make's value far past the end
  ExifData exif2 = decode(c);
  CHECK(exif2.find("Exif.Image.Make") == nullptr);
  CHECK(exif2.find("Exif.Image.Orientation") != nullptr);

  Bytes d = blockWithMakerNote();
  put32(d, 14, 0x40000000);  // a count whose size overflows 32 bits
  ExifData exif3 = decode(d);
  CHECK(exif3.find("Exif.Image.Make") == nullptr);

  // Not TIFF at all: no Exif, and no error for the rest of the file.
  CHECK(decode(Bytes{'X', 'X', 0, 0, 0, 0, 0, 0}).empty());
}

TEST(exif_structural_tags_come_from_the_layout) {
  ExifData exif;
  exif["Exif.Image.Make"] = "PhotoCo";
  exif["Exif.Image.ExifTag"] = std::uint32_t{12345};  // a bogus pointer
  exif["Exif.Photo.ISOSpeedRatings"] = std::uint16_t{100};
  exif["Exif.Thumbnail.JPEGInterchangeFormat"] = std::uint32_t{999};
  const ExifData back = decode(encode(exif));
  CHECK_EQ(back.find("Exif.Photo.ISOSpeedRatings")->toInt64(), 100);
  CHECK(back.find("Exif.Image.ExifTag") == nullptr);
  CHECK(back.find("Exif.Thumbnail.JPEGInterchangeFormat") == nullptr);
}

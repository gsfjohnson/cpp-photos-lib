#include "testing.hpp"

#include <algorithm>

using namespace lumenlib;

namespace {

struct Entry {
  std::uint16_t tag;
  std::uint16_t type;
  std::uint32_t count;
  Bytes data;  // the value, serialised
};

void put16(Bytes& b, std::size_t at, std::uint16_t v, bool le) {
  b[at] = static_cast<std::uint8_t>(le ? v : v >> 8);
  b[at + 1] = static_cast<std::uint8_t>(le ? v >> 8 : v);
}

void put32(Bytes& b, std::size_t at, std::uint32_t v, bool le) {
  for (int i = 0; i < 4; ++i)
    b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(le ? v >> (8 * i) : v >> (24 - 8 * i));
}

Bytes ascii(const std::string& s) {
  Bytes b(s.begin(), s.end());
  b.push_back(0);
  return b;
}

Bytes rationals(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> rs, bool le) {
  Bytes b(8 * rs.size());
  std::size_t at = 0;
  for (const auto& [n, d] : rs) {
    put32(b, at, n, le);
    put32(b, at + 4, d, le);
    at += 8;
  }
  return b;
}

// `prefix`, then an IFD of `entries` and their values. An out-of-line value
// at position p in the note gets the offset p + origin (so origin is where
// the offsets count from, relative to the note's start, negated).
Bytes note(const Bytes& prefix, const std::vector<Entry>& entries, bool le, std::int64_t origin) {
  Bytes b = prefix;
  const std::size_t ifd = b.size();
  b.resize(ifd + 2 + 12 * entries.size() + 4, 0);
  put16(b, ifd, static_cast<std::uint16_t>(entries.size()), le);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    const std::size_t at = ifd + 2 + 12 * i;
    put16(b, at, e.tag, le);
    put16(b, at + 2, e.type, le);
    // ASCII counts are the text's bytes, NUL included.
    put32(b, at + 4, e.type == 2 ? static_cast<std::uint32_t>(e.data.size()) : e.count, le);
    if (e.data.size() <= 4) {
      std::copy(e.data.begin(), e.data.end(), b.begin() + static_cast<std::ptrdiff_t>(at + 8));
    } else {
      put32(b, at + 8, static_cast<std::uint32_t>(static_cast<std::int64_t>(b.size()) + origin), le);
      b.insert(b.end(), e.data.begin(), e.data.end());
      if (b.size() & 1) b.push_back(0);
    }
  }
  return b;
}

// A TIFF block with the Make, a few other tags and the maker note. A note
// whose offsets count from the TIFF header needs to know where it will be:
// the block is laid out once with a stand-in of the same size to find out.
Bytes blockWith(const std::string& make, const std::function<Bytes(std::int64_t)>& makeNote) {
  const auto build = [&](const Bytes& n) {
    ExifMetadata exif;
    exif.setText("ifd0.Make", make);
    exif.setText("ifd0.Model", "Test");
    exif.setText("exif.DateTimeOriginal", "2024:01:02 03:04:05");
    exif.set("exif.MakerNote", FieldValue::fromBytes(FieldType::undefined, n));
    return exif.encode();
  };
  const Bytes standIn(makeNote(0).size(), 0xab);
  const Bytes laidOut = build(standIn);
  const auto at = std::search(laidOut.begin(), laidOut.end(), standIn.begin(), standIn.end()) - laidOut.begin();
  return build(makeNote(at));
}

const MakerNote& decoded(const ExifMetadata& exif) {
  const MakerNote* n = exif.makerNote();
  if (!n) throw testing::Failure{"no maker note decoded"};
  return *n;
}

Bytes canonNote(std::int64_t at) {
  Bytes settings(2 * 30, 0);
  put16(settings, 2 * 23, 55, true);  // longest focal length
  put16(settings, 2 * 24, 18, true);  // shortest
  put16(settings, 2 * 25, 1, true);   // focal units per mm
  return note({},
              {{0x0001, 3, 30, settings},
               {0x0006, 2, 20, ascii("Canon EOS 5D Mark V")},
               {0x0095, 2, 21, ascii("EF24-105mm f/4L IS")}},
              true, at);
}

}  // namespace

TEST(makernote_canon_offsets_from_the_tiff_header) {
  const Bytes block = blockWith("Canon", canonNote);
  const ExifMetadata exif = ExifMetadata::decode(block);
  const MakerNote& n = decoded(exif);
  CHECK(n.format() == MakerNoteFormat::canon);
  CHECK_EQ(n.find("LensModel")->value.text(), "EF24-105mm f/4L IS");
  CHECK_EQ(n.find("canon.ImageType")->value.text(), "Canon EOS 5D Mark V");
  CHECK_EQ(n.find("CameraSettings")->value.count(), 30u);
  CHECK_EQ(*lensDescription(exif), "EF24-105mm f/4L IS");
  CHECK_EQ(std::string(makerNoteFormatName(n.format())), "Canon");
}

TEST(makernote_moves_with_its_offsets) {
  // A new tag moves the maker note; its TIFF-relative offsets follow it.
  const Bytes block = blockWith("Canon", canonNote);
  ExifMetadata exif = ExifMetadata::decode(block);
  exif.setText("ifd0.Artist", "Someone with a long enough name to move everything along");
  const Bytes moved = exif.encode();
  CHECK(moved.size() > block.size());
  const ExifMetadata again = ExifMetadata::decode(moved);
  CHECK_EQ(decoded(again).find("LensModel")->value.text(), "EF24-105mm f/4L IS");
  // And moves again, from where it is now.
  ExifMetadata third = again;
  third.remove("ifd0.Artist");
  third.setText("ifd0.Software", "x");
  CHECK_EQ(decoded(ExifMetadata::decode(third.encode())).find("LensModel")->value.text(), "EF24-105mm f/4L IS");
}

TEST(makernote_canon_focal_range_without_a_lens_name) {
  const Bytes block = blockWith("Canon", [](std::int64_t at) {
    Bytes settings(2 * 30, 0);
    put16(settings, 2 * 23, 55, true);
    put16(settings, 2 * 24, 18, true);
    put16(settings, 2 * 25, 1, true);
    return note({}, {{0x0001, 3, 30, settings}}, true, at);
  });
  CHECK_EQ(*lensDescription(ExifMetadata::decode(block)), "18-55mm");
}

TEST(makernote_nikon_has_its_own_tiff_header) {
  for (bool le : {true, false}) {
    Bytes prefix = testing::bytesOf(std::string("Nikon\0\x02\x10\0\0", 10));
    Bytes header(8, 0);
    header[0] = header[1] = le ? 'I' : 'M';
    put16(header, 2, 42, le);
    put32(header, 4, 8, le);
    prefix.insert(prefix.end(), header.begin(), header.end());
    const Bytes n = note(prefix,
                         {{0x0001, 7, 4, testing::bytesOf("0210")},
                          {0x0084, 5, 4, rationals({{18, 1}, {55, 1}, {35, 10}, {56, 10}}, le)},
                          {0x001d, 2, 8, ascii("1234567")}},
                         le, -10);
    const Bytes block = blockWith("NIKON CORPORATION", [&](std::int64_t) { return n; });
    const ExifMetadata exif = ExifMetadata::decode(block);
    const MakerNote& d = decoded(exif);
    CHECK(d.format() == MakerNoteFormat::nikon);
    CHECK_EQ(d.find("nikon.SerialNumber")->value.text(), "1234567");
    CHECK_EQ(*lensDescription(exif), "18-55mm F3.5-5.6");
  }
}

TEST(makernote_olympus_equipment_ifd) {
  const Bytes equipment = note({}, {{0x0203, 2, 24, ascii("OLYMPUS M.12-40mm F2.8")}}, true, 0);
  // The equipment IFD follows the main IFD; its offsets count from the note.
  const Bytes prefix = testing::bytesOf(std::string("OLYMPUS\0II\x03\0", 12));
  const std::size_t mainSize = 2 + 12 * 1 + 4;
  const std::size_t equipmentAt = prefix.size() + mainSize;
  Bytes subOffset(4);
  put32(subOffset, 0, static_cast<std::uint32_t>(equipmentAt), true);
  Bytes n = note(prefix, {{0x2010, 13, 1, subOffset}}, true, 0);
  // Re-base the equipment IFD's own value offsets onto the note.
  const Bytes rebased =
      note({}, {{0x0203, 2, 24, ascii("OLYMPUS M.12-40mm F2.8")}}, true, static_cast<std::int64_t>(equipmentAt));
  CHECK_EQ(rebased.size(), equipment.size());
  n.insert(n.end(), rebased.begin(), rebased.end());
  const Bytes block = blockWith("OLYMPUS CORPORATION", [&](std::int64_t) { return n; });
  const ExifMetadata exif = ExifMetadata::decode(block);
  const MakerNote& d = decoded(exif);
  CHECK(d.format() == MakerNoteFormat::olympus);
  CHECK_EQ(d.find("olympus.equipment.LensModel")->value.text(), "OLYMPUS M.12-40mm F2.8");
  CHECK_EQ(*lensDescription(exif), "OLYMPUS M.12-40mm F2.8");
}

TEST(makernote_sony_lens_spec) {
  const Bytes block = blockWith("SONY", [](std::int64_t at) {
    const Bytes spec = {0x00, 0x00, 0x18, 0x00, 0x55, 0x35, 0x56, 0x00};
    return note(testing::bytesOf(std::string("SONY DSC \0\0\0", 12)), {{0xb02a, 7, 8, spec}}, true, at);
  });
  const ExifMetadata exif = ExifMetadata::decode(block);
  CHECK(decoded(exif).format() == MakerNoteFormat::sony);
  CHECK_EQ(*lensDescription(exif), "18-55mm F3.5-5.6");
}

TEST(makernote_panasonic_lens_type) {
  const Bytes block = blockWith("Panasonic", [](std::int64_t at) {
    return note(testing::bytesOf(std::string("Panasonic\0\0\0", 12)),
                {{0x0051, 2, 26, ascii("LUMIX G VARIO 12-32/F3.5")}}, true, at);
  });
  CHECK_EQ(*lensDescription(ExifMetadata::decode(block)), "LUMIX G VARIO 12-32/F3.5");
}

TEST(makernote_fujifilm_focal_range) {
  Bytes prefix = testing::bytesOf("FUJIFILM");
  prefix.insert(prefix.end(), {12, 0, 0, 0});
  const Bytes n = note(prefix,
                       {{0x1404, 5, 1, rationals({{18, 1}}, true)},
                        {0x1405, 5, 1, rationals({{55, 1}}, true)},
                        {0x1406, 5, 1, rationals({{28, 10}}, true)},
                        {0x1407, 5, 1, rationals({{40, 10}}, true)}},
                       true, 0);
  const Bytes block = blockWith("FUJIFILM", [&](std::int64_t) { return n; });
  const ExifMetadata exif = ExifMetadata::decode(block);
  CHECK(decoded(exif).format() == MakerNoteFormat::fujifilm);
  CHECK_EQ(*lensDescription(exif), "18-55mm F2.8-4");
}

TEST(makernote_lens_model_tag_comes_first) {
  ExifMetadata exif = ExifMetadata::decode(blockWith("Canon", canonNote));
  exif.setText("exif.LensModel", "RF50mm F1.8 STM");
  CHECK_EQ(*lensDescription(exif), "RF50mm F1.8 STM");
  exif.setText("exif.LensModel", "----");  // placeholders do not count
  CHECK_EQ(*lensDescription(exif), "EF24-105mm f/4L IS");
  ExifMetadata plain;
  CHECK(!lensDescription(plain));
  plain.setText("exif.LensSpecification", "24/1 70/1 28/10 28/10");
  CHECK_EQ(*lensDescription(plain), "24-70mm F2.8");
}

TEST(makernote_unknown_or_damaged_is_left_alone) {
  ExifMetadata exif;
  exif.setText("ifd0.Make", "Nobody");
  exif.set("exif.MakerNote", FieldValue::fromBytes(FieldType::undefined, Bytes(40, 7)));
  const ExifMetadata back = ExifMetadata::decode(exif.encode());
  CHECK(back.makerNote() == nullptr);
  CHECK(back.find("exif.MakerNote")->value().bytes() == Bytes(40, 7));

  // A Canon note whose IFD claims more entries than there are.
  Bytes bad = canonNote(0);
  bad[0] = 0xff;
  bad[1] = 0x7f;
  ExifMetadata canon;
  canon.setText("ifd0.Make", "Canon");
  canon.set("exif.MakerNote", FieldValue::fromBytes(FieldType::undefined, bad));
  const ExifMetadata badBack = ExifMetadata::decode(canon.encode());
  CHECK(badBack.makerNote() == nullptr);
  CHECK(badBack.find("exif.MakerNote") != nullptr);
}

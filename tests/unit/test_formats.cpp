#include "testing.hpp"

#include <algorithm>
#include <cstring>

using namespace lumenlib;
using testing::load;
using testing::roundTrip;

namespace {

// Offset of the first SOS marker of a JPEG.
std::size_t scanStart(const Bytes& jpeg) {
  std::size_t pos = 2;
  while (pos + 4 <= jpeg.size() && jpeg[pos] == 0xff && jpeg[pos + 1] != 0xda) {
    pos += 2 + (jpeg[pos + 2] << 8 | jpeg[pos + 3]);
  }
  return pos;
}

void checkFixtureExif(const ImageFile& file) {
  const auto& exif = file.exif();
  CHECK_EQ(exif.find("ifd0.Make")->text(), "PhotoCo");
  CHECK_EQ(exif.find("ifd0.Model")->text(), "Model X");
  CHECK_EQ(exif.find("ifd0.Orientation")->asInt(), 6);
  CHECK_EQ(exif.find("exif.DateTimeOriginal")->text(), "2024:05:17 18:42:07");
  CHECK_EQ(exif.find("exif.ExposureTime")->text(), "1/250");
  CHECK_EQ(exif.find("gps.GPSLatitudeRef")->text(), "N");
}

void checkFixtureXmp(const ImageFile& file) {
  const auto& xmp = file.xmp();
  CHECK_EQ(xmp.find("xmp:Rating")->summary(), "4");
  CHECK_EQ(xmp.find("dc:subject")->summary(), std::string("harbour, boats, ") + testing::kKobenhavn);
  CHECK_EQ(xmp.find("mwg-rs:Regions/mwg-rs:RegionList[1]/mwg-rs:Name")->summary(), "Ada");
}

std::vector<std::pair<std::string, std::string>> listing(const ExifMetadata& m) {
  std::vector<std::pair<std::string, std::string>> v;
  for (const auto& e : m) v.emplace_back(e.tag().str(), e.text());
  return v;
}
std::vector<std::pair<std::string, std::string>> listing(const IptcMetadata& m) {
  std::vector<std::pair<std::string, std::string>> v;
  for (const auto& e : m) v.emplace_back(e.name(), e.value());
  return v;
}
std::vector<std::pair<std::string, std::string>> listing(const XmpMetadata& m) {
  std::vector<std::pair<std::string, std::string>> v;
  for (const auto& e : m) v.emplace_back(e.path(), e.summary());
  return v;
}

// Every entry of a survives in b (in any order: IPTC is written sorted).
template <typename Data>
void checkSame(const Data& a, const Data& b) {
  auto va = listing(a), vb = listing(b);
  std::stable_sort(va.begin(), va.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  std::stable_sort(vb.begin(), vb.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  CHECK_EQ(va.size(), vb.size());
  for (std::size_t i = 0; i < va.size() && i < vb.size(); ++i) {
    CHECK_EQ(va[i].first, vb[i].first);
    CHECK_EQ(va[i].second, vb[i].second);
  }
}

bool contains(const Bytes& haystack, const std::string& needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// The bytes of every strip of a TIFF's IFD0.
Bytes stripData(const ImageFile& file) {
  const auto* offsets = file.exif().find("ifd0.StripOffsets");
  const auto* counts = file.exif().find("ifd0.StripByteCounts");
  Bytes out;
  if (!offsets || !counts) return out;
  for (std::size_t i = 0; i < offsets->count(); ++i) {
    const auto b = file.source().readBytes(static_cast<std::uint64_t>(offsets->asInt(i)),
                                           static_cast<std::size_t>(counts->asInt(i)));
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}

// A test ICC profile: the bytes only need to survive.
Bytes iccProfile(std::size_t size) {
  Bytes b(size);
  for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(i * 7 + 3);
  return b;
}

}  // namespace

TEST(detect_formats) {
  const std::pair<const char*, FileFormat> cases[] = {
      {"photo.jpg", FileFormat::jpeg},  {"multi.mpo", FileFormat::jpeg}, {"photo.png", FileFormat::png},
      {"photo.webp", FileFormat::webp}, {"photo.tif", FileFormat::tiff}, {"photo.heic", FileFormat::heif},
      {"photo.avif", FileFormat::avif}, {"photo.xmp", FileFormat::xmp},
  };
  for (const auto& [name, format] : cases) {
    CHECK_EQ(formatName(ImageFile::open(testing::readData(name))->format()), std::string(formatName(format)));
    CHECK(detectFormat(testing::dataPath(name)) == format);
  }
  CHECK_THROWS(ImageFile::open(Bytes{'n', 'o', 'p', 'e'}), ErrorCode::unsupportedFormat);
  CHECK_THROWS(ImageFile::open(Bytes{}), ErrorCode::unsupportedFormat);
  CHECK_THROWS(detectFormat(testing::dataPath("missing.jpg")), ErrorCode::io);
}

TEST(format_capabilities) {
  CHECK(formatCanWrite(FileFormat::jpeg, MetadataKind::iptc));
  CHECK(formatCanWrite(FileFormat::tiff, MetadataKind::exif));
  CHECK(formatCanWrite(FileFormat::tiff, MetadataKind::iccProfile));
  CHECK(!formatCanWrite(FileFormat::webp, MetadataKind::iptc));
  CHECK(!formatCanWrite(FileFormat::heif, MetadataKind::exif));
  CHECK(formatCanRead(FileFormat::heif, MetadataKind::exif));
  CHECK(!formatCanWrite(FileFormat::raf, MetadataKind::xmp));
  CHECK(!formatCanWrite(FileFormat::rw2, MetadataKind::exif));
  CHECK(formatCanRead(FileFormat::rw2, MetadataKind::exif));
  // An RW2 is told apart from other TIFF-based raws by its magic number.
  Bytes rw2 = testing::readData("photo.tif");
  rw2[0] = rw2[1] = 'I';
  rw2[2] = 0x55;
  rw2[3] = 0;
  CHECK(ImageFile::open(std::move(rw2))->format() == FileFormat::rw2);
  CHECK(!formatCanRead(FileFormat::unknown, MetadataKind::exif));
  CHECK(load("photo.png")->canWrite(MetadataKind::iptc));
}

TEST(jpeg_read) {
  const auto file = load("photo.jpg");
  checkFixtureExif(*file);
  checkFixtureXmp(*file);
  CHECK_EQ(file->width(), 64u);
  CHECK_EQ(file->height(), 48u);
  CHECK_EQ(file->comment(), "hello comment");
  CHECK((file->iptc().values("Keywords") == std::vector<std::string>{"harbour", testing::kKobenhavn}));
  CHECK_EQ(file->exif().thumbnail().size(), 647u);
  CHECK_EQ(file->exif().thumbnail()[0], 0xff);
  CHECK(file->canWrite(MetadataKind::iptc));
  CHECK(file->mimeType() == std::string("image/jpeg"));
}

TEST(jpeg_big_endian) {
  const auto file = load("motorola.jpg");
  CHECK(file->exif().byteOrder() == ByteOrder::big);
  checkFixtureExif(*file);
  const auto again = roundTrip(*file);
  CHECK(again->exif().byteOrder() == ByteOrder::big);
  checkSame(file->exif(), again->exif());
}

TEST(jpeg_unchanged_write_keeps_everything) {
  const Bytes original = testing::readData("photo.jpg");
  const auto file = load("photo.jpg");
  const auto again = roundTrip(*file);
  checkSame(file->exif(), again->exif());
  checkSame(file->iptc(), again->iptc());
  checkSame(file->xmp(), again->xmp());
  CHECK_EQ(again->comment(), "hello comment");
  CHECK(again->exif().thumbnail() == file->exif().thumbnail());

  // The image data is copied byte for byte.
  MemorySink sink;
  file->saveTo(sink);
  const Bytes& out = sink.data();
  const auto a = scanStart(original), b = scanStart(out);
  CHECK(original.size() - a == out.size() - b);
  CHECK(std::equal(original.begin() + static_cast<std::ptrdiff_t>(a), original.end(),
                   out.begin() + static_cast<std::ptrdiff_t>(b)));
}

TEST(jpeg_edit_round_trip) {
  auto file = load("photo.jpg");
  file->exif().setText("ifd0.Artist", "Grace Hopper");
  file->exif().remove("gps.GPSAltitude");
  file->xmp().setText("xmp:Rating", "2");
  file->xmp().setItems("dc:subject", {"one"});
  file->iptc().setValues("Keywords", {testing::kAlesund});
  file->comment() = "";
  const auto again = roundTrip(*file);
  CHECK_EQ(again->exif().find("ifd0.Artist")->text(), "Grace Hopper");
  CHECK(again->exif().find("gps.GPSAltitude") == nullptr);
  CHECK_EQ(again->xmp().find("xmp:Rating")->summary(), "2");
  CHECK_EQ(again->xmp().find("dc:subject")->summary(), "one");
  CHECK((again->iptc().values("Keywords") == std::vector<std::string>{testing::kAlesund}));
  CHECK(again->comment().empty());
  CHECK_EQ(again->exif().thumbnail().size(), 647u);
}

TEST(jpeg_metadata_added_to_bare_file_and_removed_again) {
  auto file = load("bare.jpg");
  CHECK(file->exif().empty());
  file->exif().setText("ifd0.Make", "PhotoCo");
  file->xmp().setText("xmp:Rating", "3");
  file->iptc().set("City", "Oslo");
  auto again = roundTrip(*file);
  CHECK_EQ(again->exif().find("ifd0.Make")->text(), "PhotoCo");
  CHECK_EQ(again->xmp().find("xmp:Rating")->summary(), "3");
  CHECK_EQ(*again->iptc().value("City"), "Oslo");

  again->clearMetadata();
  MemorySink sink;
  again->saveTo(sink);
  CHECK(sink.data() == testing::readData("bare.jpg"));
}

TEST(jpeg_clear_metadata_keeps_the_icc_profile) {
  auto file = load("photo.jpg");
  file->setIccProfile(iccProfile(300));
  auto withIcc = roundTrip(*file);
  withIcc->clearMetadata();
  const auto again = roundTrip(*withIcc);
  CHECK(again->exif().empty());
  CHECK(again->xmp().empty());
  CHECK(again->iptc().empty());
  CHECK(again->comment().empty());
  CHECK(again->iccProfile() == iccProfile(300));
}

TEST(jpeg_icc_profile_written_in_chunks) {
  auto file = load("photo.jpg");
  const Bytes big = iccProfile(150000);  // three APP2 segments
  file->setIccProfile(big);
  auto again = roundTrip(*file);
  CHECK(again->iccProfile() == big);
  checkFixtureExif(*again);
  // Replaced, not added to.
  again->setIccProfile(iccProfile(10));
  CHECK(roundTrip(*again)->iccProfile() == iccProfile(10));
  // And removed.
  again->setIccProfile({});
  CHECK(roundTrip(*again)->iccProfile().empty());
}

TEST(jpeg_multi_picture_offsets_follow_the_edit) {
  auto file = load("multi.mpo");
  file->xmp().setText("dc:description", std::string(3000, 'x'));  // grows the header a lot
  MemorySink sink;
  file->saveTo(sink);
  const Bytes& out = sink.data();
  // Find the MPF segment and its second entry's offset, relative to the MPF
  // header, and check a JPEG starts there.
  const char mpf[] = "MPF";
  const auto it = std::search(out.begin(), out.end(), mpf, mpf + 4);
  CHECK(it != out.end());
  const std::size_t header = static_cast<std::size_t>(it - out.begin()) + 4;
  const bool le = out[header] == 'I';
  const auto get32 = [&](std::size_t p) {
    return le ? static_cast<std::uint32_t>(out[p] | out[p + 1] << 8 | out[p + 2] << 16 | out[p + 3] << 24)
              : static_cast<std::uint32_t>(out[p] << 24 | out[p + 1] << 16 | out[p + 2] << 8 | out[p + 3]);
  };
  const auto get16 = [&](std::size_t p) { return le ? out[p] | out[p + 1] << 8 : out[p] << 8 | out[p + 1]; };
  const std::size_t ifd = header + get32(header + 4);
  std::uint32_t entries = 0;
  for (int i = 0; i < get16(ifd); ++i) {
    const std::size_t e = ifd + 2 + 12 * static_cast<std::size_t>(i);
    if (get16(e) == 0xb002) entries = get32(e + 8);
  }
  CHECK(entries != 0);
  const std::size_t second = header + get32(header + entries + 16 + 8);
  CHECK(second + 3 < out.size());
  CHECK_EQ(out[second], 0xff);
  CHECK_EQ(out[second + 1], 0xd8);
}

TEST(png_read_write) {
  auto file = load("photo.png");
  checkFixtureExif(*file);
  checkFixtureXmp(*file);
  CHECK_EQ(file->width(), 64u);
  file->xmp().setText("xmp:Rating", "1");
  file->iptc().set("City", "Oslo");
  const auto again = roundTrip(*file);
  checkFixtureExif(*again);
  CHECK_EQ(again->xmp().find("xmp:Rating")->summary(), "1");
  CHECK_EQ(*again->iptc().value("City"), "Oslo");
}

TEST(png_chunks_are_valid) {
  auto file = load("bare.png");
  file->exif().setText("ifd0.Make", "PhotoCo");
  file->xmp().setText("xmp:Rating", "5");
  file->setIccProfile(iccProfile(500));
  MemorySink sink;
  file->saveTo(sink);
  const Bytes& out = sink.data();
  // Walk the chunks; IHDR first, IEND last, eXIf and iCCP before IDAT.
  std::size_t pos = 8;
  std::vector<std::string> types;
  while (pos + 12 <= out.size()) {
    const std::size_t n =
        static_cast<std::size_t>(out[pos] << 24 | out[pos + 1] << 16 | out[pos + 2] << 8 | out[pos + 3]);
    types.emplace_back(out.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                       out.begin() + static_cast<std::ptrdiff_t>(pos + 8));
    pos += 12 + n;
  }
  CHECK_EQ(pos, out.size());
  CHECK_EQ(types.front(), "IHDR");
  CHECK_EQ(types.back(), "IEND");
  const auto idat = std::find(types.begin(), types.end(), "IDAT");
  CHECK(std::find(types.begin(), types.end(), "eXIf") < idat);
  CHECK(std::find(types.begin(), types.end(), "iCCP") < idat);
  CHECK_EQ(std::count(types.begin(), types.end(), "iCCP"), 1);
  auto again = ImageFile::open(Bytes(out));
  again->load();
  CHECK(again->iccProfile() == iccProfile(500));
}

TEST(webp_read_write) {
  auto file = load("photo.webp");
  checkFixtureExif(*file);
  checkFixtureXmp(*file);
  CHECK_EQ(file->width(), 64u);
  CHECK_EQ(file->height(), 48u);
  CHECK(!file->canWrite(MetadataKind::iptc));
  file->exif().setText("ifd0.Artist", "Me");
  const auto again = roundTrip(*file);
  CHECK_EQ(again->exif().find("ifd0.Artist")->text(), "Me");
  checkFixtureXmp(*again);
}

TEST(webp_icc_profile_and_flag) {
  for (const char* name : {"lossy.webp", "photo.webp"}) {
    auto file = load(name);
    file->setIccProfile(iccProfile(301));  // odd, so the chunk is padded
    MemorySink sink;
    file->saveTo(sink);
    const Bytes& out = sink.data();
    CHECK(std::memcmp(out.data() + 12, "VP8X", 4) == 0);
    CHECK_EQ(out[20] & 0x20, 0x20);                       // the ICC flag
    CHECK(std::memcmp(out.data() + 30, "ICCP", 4) == 0);  // right after VP8X
    auto again = ImageFile::open(Bytes(out));
    again->load();
    CHECK(again->iccProfile() == iccProfile(301));
    again->setIccProfile({});
    MemorySink none;
    again->saveTo(none);
    CHECK_EQ(none.data()[20] & 0x20, 0);
  }
}

TEST(webp_simple_files_become_extended) {
  for (const char* name : {"lossy.webp", "lossless.webp"}) {
    auto file = load(name);
    CHECK(file->exif().empty());
    file->xmp().setText("xmp:Rating", "5");
    file->exif().setInt("ifd0.Orientation", 3);
    MemorySink sink;
    file->saveTo(sink);
    const Bytes& out = sink.data();
    CHECK(std::memcmp(out.data() + 12, "VP8X", 4) == 0);
    CHECK_EQ(out[20] & 0x0c, 0x0c);  // EXIF and XMP flags
    const std::uint32_t riff = out[4] | out[5] << 8 | out[6] << 16 | static_cast<std::uint32_t>(out[7]) << 24;
    CHECK_EQ(riff + 8, out.size());
    auto again = ImageFile::open(Bytes(out));
    again->load();
    CHECK_EQ(again->width(), 64u);
    CHECK_EQ(again->height(), 48u);
    CHECK_EQ(again->exif().find("ifd0.Orientation")->asInt(), 3);
  }
}

TEST(tiff_read) {
  const auto file = load("photo.tif");
  checkFixtureExif(*file);
  checkFixtureXmp(*file);
  CHECK_EQ(*file->iptc().value("City"), "Copenhagen");
  CHECK_EQ(file->width(), 64u);
  // The XMP and IPTC blocks are not listed as Exif tags.
  CHECK(file->exif().find("ifd0.XMP") == nullptr);
  CHECK(file->exif().find("ifd0.IPTC") == nullptr);
  CHECK(file->canWrite(MetadataKind::exif));
}

TEST(tiff_write_round_trip) {
  auto file = load("photo.tif");
  const Bytes pixels = stripData(*file);
  CHECK(!pixels.empty());
  file->exif().setText("ifd0.Artist", "Grace Hopper");
  file->exif().setText("exif.LensModel", "A much longer lens name than any the file had before");
  file->xmp().setText("xmp:Rating", "1");
  file->iptc().set("City", "Oslo");
  const auto again = roundTrip(*file);
  CHECK_EQ(again->exif().find("ifd0.Artist")->text(), "Grace Hopper");
  CHECK_EQ(again->exif().find("exif.LensModel")->text(), "A much longer lens name than any the file had before");
  CHECK_EQ(again->exif().find("ifd0.Make")->text(), "PhotoCo");
  CHECK_EQ(again->exif().find("gps.GPSLatitudeRef")->text(), "N");
  CHECK_EQ(again->xmp().find("xmp:Rating")->summary(), "1");
  CHECK_EQ(*again->iptc().value("City"), "Oslo");
  CHECK_EQ(again->width(), 64u);
  CHECK(stripData(*again) == pixels);
}

TEST(tiff_image_structure_is_not_editable) {
  auto file = load("photo.tif");
  const Bytes pixels = stripData(*file);
  file->exif().setInt("ifd0.ImageWidth", 12);
  file->exif().remove("ifd0.StripOffsets");
  file->exif().setText("ifd0.Model", "Edited");
  const auto again = roundTrip(*file);
  CHECK_EQ(again->exif().find("ifd0.ImageWidth")->asInt(), 64);
  CHECK(stripData(*again) == pixels);
  CHECK_EQ(again->exif().find("ifd0.Model")->text(), "Edited");
}

TEST(tiff_removed_metadata_cannot_be_read_back) {
  const Bytes original = testing::readData("photo.tif");
  CHECK(contains(original, "Copenhagen"));
  CHECK(contains(original, "Harbour at dusk"));
  CHECK(contains(original, "Model X"));
  auto file = load("photo.tif");
  const Bytes pixels = stripData(*file);
  file->clearMetadata();
  MemorySink sink;
  file->saveTo(sink);
  const Bytes& out = sink.data();
  CHECK(!contains(out, "Copenhagen"));
  CHECK(!contains(out, "Harbour at dusk"));
  CHECK(!contains(out, "Model X"));
  auto again = ImageFile::open(Bytes(out));
  again->load();
  CHECK(again->xmp().empty());
  CHECK(again->iptc().empty());
  CHECK(again->exif().find("gps.GPSLatitude") == nullptr);
  CHECK(again->exif().find("ifd0.Make") == nullptr);
  CHECK_EQ(again->width(), 64u);  // the image's own tags stay
  CHECK(stripData(*again) == pixels);
}

TEST(tiff_repeated_saves_do_not_grow_the_file) {
  auto file = ImageFile::open(testing::readData("photo.tif"));
  file->load();
  file->xmp().setText("xmp:Rating", "2");
  file->save();
  const std::size_t first = file->buffer()->size();
  for (int i = 0; i < 3; ++i) {
    file->load();
    file->xmp().setText("xmp:Rating", std::to_string(3 + i));
    file->save();
  }
  CHECK_EQ(file->buffer()->size(), first);
  file->load();
  CHECK_EQ(*file->xmp().text("xmp:Rating"), "5");
  checkFixtureExif(*file);
}

TEST(tiff_icc_profile) {
  auto file = load("photo.tif");
  file->setIccProfile(iccProfile(1000));
  auto again = roundTrip(*file);
  CHECK(again->iccProfile() == iccProfile(1000));
  CHECK(again->exif().find("ifd0.ICCProfile") == nullptr);
  // Kept through an edit of something else.
  again->xmp().setText("xmp:Rating", "3");
  CHECK(roundTrip(*again)->iccProfile() == iccProfile(1000));
}

TEST(raf_read) {
  // A RAF: the signature, a version, the camera's name, and a directory
  // whose first entry is the JPEG's offset and length.
  const Bytes jpeg = testing::readData("photo.jpg");
  Bytes raf = testing::bytesOf("FUJIFILMCCD-RAW 0201FF383501");
  raf.resize(100, 0);
  const std::uint32_t offset = 100, length = static_cast<std::uint32_t>(jpeg.size());
  for (int i = 0; i < 4; ++i) {
    raf[84 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(offset >> (24 - 8 * i));
    raf[88 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(length >> (24 - 8 * i));
  }
  raf.insert(raf.end(), jpeg.begin(), jpeg.end());
  auto file = ImageFile::open(Bytes(raf));
  CHECK(file->format() == FileFormat::raf);
  file->load();
  checkFixtureExif(*file);
  checkFixtureXmp(*file);
  CHECK(!file->canWrite(MetadataKind::exif));
  MemorySink sink;
  CHECK_THROWS(file->saveTo(sink), ErrorCode::unsupportedOperation);
  raf[91] = 0xff;  // the JPEG runs past the end
  auto damaged = ImageFile::open(Bytes(raf));
  CHECK_THROWS(damaged->load(), ErrorCode::corruptData);
}

TEST(heif_and_avif_read) {
  for (const char* name : {"photo.heic", "photo.avif"}) {
    const auto file = load(name);
    CHECK_EQ(file->exif().find("ifd0.Model")->text(), "Model H");
    CHECK_EQ(file->exif().find("exif.DateTimeOriginal")->text(), "2024:05:17 18:42:07");
    checkFixtureXmp(*file);
    CHECK_EQ(file->width(), 64u);
    CHECK_EQ(file->height(), 48u);
  }
}

TEST(xmp_sidecar) {
  auto file = load("photo.xmp");
  checkFixtureXmp(*file);
  CHECK(!file->canWrite(MetadataKind::exif));
  file->xmp().setText("xmp:Rating", "5");
  const auto again = roundTrip(*file);
  CHECK(again->format() == FileFormat::xmp);
  CHECK_EQ(again->xmp().find("xmp:Rating")->summary(), "5");

  auto fresh = ImageFile::newXmpSidecar();
  fresh->xmp().setText("dc:title", "New");
  fresh->save();
  const Bytes* buffer = fresh->buffer();
  CHECK(buffer != nullptr);
  CHECK(!contains(*buffer, "xpacket"));  // a sidecar is the XMP alone
  auto reopened = ImageFile::open(Bytes(*buffer));
  reopened->load();
  CHECK_EQ(*reopened->xmp().find("dc:title")->value().langText(), "New");
}

TEST(file_write_replaces_the_file) {
  const testing::TempDir dir("lumenlib-tests");
  const auto path = dir.path / "edit.jpg";
  std::filesystem::copy_file(testing::dataPath("photo.jpg"), path, std::filesystem::copy_options::overwrite_existing);
  {
    auto file = ImageFile::open(path);
    file->load();
    file->xmp().setText("xmp:Rating", "1");
    file->save();
    // Still usable after the write.
    file->load();
    CHECK_EQ(file->xmp().find("xmp:Rating")->summary(), "1");
  }
  {
    auto file = ImageFile::open(path);
    file->load();
    CHECK_EQ(file->xmp().find("xmp:Rating")->summary(), "1");
    checkFixtureExif(*file);
  }
  // No temporary files are left behind.
  std::size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir.path)) files += e.is_regular_file() ? 1 : 0;
  CHECK_EQ(files, 1u);
  CHECK_THROWS(ImageFile::open(dir.path / "missing.jpg"), ErrorCode::io);
}

TEST(file_names_outside_ascii) {
  const testing::TempDir dir("lumenlib-tests-names");
  const auto path = dir.path / std::filesystem::u8path(testing::kKobenhavn + ".tif");
  std::filesystem::copy_file(testing::dataPath("photo.tif"), path);
  CHECK(detectFormat(path) == FileFormat::tiff);
  {
    auto file = ImageFile::open(path);
    file->load();
    file->exif().setText("ifd0.Artist", testing::kAlesund);
    file->save();
  }
  auto file = ImageFile::open(path);
  file->load();
  CHECK_EQ(file->exif().find("ifd0.Artist")->text(), testing::kAlesund);
}

TEST(jpeg_xl_container_read) {
  using testing::box;
  using testing::bytesOf;
  ExifMetadata exif;
  exif.setText("ifd0.Make", "PhotoCo");
  const Bytes tiff = testing::tiffOf(exif);
  const Bytes signature = {0, 0, 0, 12, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a};
  const Bytes jxl = testing::concat({
      signature,
      box("ftyp", bytesOf(std::string("jxl \0\0\0\0jxl ", 12))),
      box("Exif", testing::concat({Bytes{0, 0, 0, 0}, tiff})),
      box("xml ", bytesOf("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                          "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
                          "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"2\"/></rdf:RDF></x:xmpmeta>")),
      box("jxlc", Bytes{0xff, 0x0a}),
  });
  auto file = ImageFile::open(Bytes(jxl));
  CHECK(file->format() == FileFormat::jxl);
  file->load();
  CHECK_EQ(file->exif().find("ifd0.Make")->text(), "PhotoCo");
  CHECK_EQ(file->xmp().find("xmp:Rating")->summary(), "2");
}

TEST(cr3_read) {
  using testing::box;
  using testing::bytesOf;
  // CMT1 is IFD0, CMT2 the Exif IFD and CMT4 the GPS IFD, each a TIFF
  // structure whose first IFD holds the tags.
  ExifMetadata ifd0, exifIfd, gpsIfd;
  ifd0.setText("ifd0.Model", "Canon EOS R5");
  exifIfd.append(ExifTag(Ifd::ifd0, 0x829a), FieldValue::rationals(FieldType::urational, {{1, 500}}));
  gpsIfd.append(ExifTag(Ifd::ifd0, 0x0001), FieldValue::ascii("S"));
  const std::uint8_t canon[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                  0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
  const std::uint8_t xmpUuid[16] = {0xbe, 0x7a, 0xcf, 0xcb, 0x97, 0xa9, 0x42, 0xe8,
                                    0x9c, 0x71, 0x99, 0x94, 0x91, 0xe3, 0xaf, 0xac};
  const Bytes cmt = testing::concat({Bytes(canon, canon + 16), box("CMT1", testing::tiffOf(ifd0)),
                                     box("CMT2", testing::tiffOf(exifIfd)), box("CMT4", testing::tiffOf(gpsIfd))});
  const Bytes packet = bytesOf(
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
      "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
      "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"5\"/></rdf:RDF></x:xmpmeta>");
  const Bytes cr3 = testing::concat({
      box("ftyp", bytesOf(std::string("crx \0\0\0\x01crx isom", 16))),
      box("moov", box("uuid", cmt)),
      box("uuid", testing::concat({Bytes(xmpUuid, xmpUuid + 16), packet})),
  });
  auto file = ImageFile::open(Bytes(cr3));
  CHECK(file->format() == FileFormat::cr3);
  file->load();
  CHECK_EQ(file->exif().find("ifd0.Model")->text(), "Canon EOS R5");
  CHECK_EQ(file->exif().find("exif.ExposureTime")->text(), "1/500");
  CHECK_EQ(file->exif().find("gps.GPSLatitudeRef")->text(), "S");
  CHECK_EQ(file->xmp().find("xmp:Rating")->summary(), "5");
  CHECK(!file->canWrite(MetadataKind::exif));
}

TEST(damaged_xmp_packet_warns_and_is_skipped) {
  std::vector<std::string> warnings;
  setWarningHandler([&](const std::string& w) { warnings.push_back(w); });
  auto file = load("bare.jpg");
  file->xmp().setText("xmp:Rating", "1");
  MemorySink sink;
  file->saveTo(sink);
  Bytes out = sink.release();
  const std::string tag = "xmp:Rating";
  auto it = std::search(out.begin(), out.end(), tag.begin(), tag.end());
  CHECK(it != out.end());
  *(it + 3) = '<';  // breaks the XML
  auto damaged = ImageFile::open(std::move(out));
  damaged->load();
  CHECK(damaged->xmp().empty());
  CHECK(!damaged->xmpPacket().empty());
  CHECK_EQ(warnings.size(), 1u);
  setWarningHandler(nullptr);
}

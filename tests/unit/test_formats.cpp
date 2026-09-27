#include "testing.hpp"

#include <algorithm>
#include <cstring>

using namespace photos;
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

void checkFixtureExif(const Image& image) {
  const auto& exif = image.exifData();
  CHECK_EQ(exif.find("Exif.Image.Make")->toString(), "PhotoCo");
  CHECK_EQ(exif.find("Exif.Image.Model")->toString(), "Model X");
  CHECK_EQ(exif.find("Exif.Image.Orientation")->toInt64(), 6);
  CHECK_EQ(exif.find("Exif.Photo.DateTimeOriginal")->toString(), "2024:05:17 18:42:07");
  CHECK_EQ(exif.find("Exif.Photo.ExposureTime")->toString(), "1/250");
  CHECK_EQ(exif.find("Exif.GPSInfo.GPSLatitudeRef")->toString(), "N");
}

void checkFixtureXmp(const Image& image) {
  const auto& xmp = image.xmpData();
  CHECK_EQ(xmp.find("Xmp.xmp.Rating")->toString(), "4");
  CHECK_EQ(xmp.find("Xmp.dc.subject")->toString(), std::string("harbour, boats, ") + testing::kKobenhavn);
  CHECK_EQ(xmp.find("Xmp.mwg-rs.Regions/mwg-rs:RegionList[1]/mwg-rs:Name")->toString(), "Ada");
}

// Every datum of a survives in b (in any order: IPTC is written sorted).
template <typename Data>
void checkSame(const Data& a, const Data& b) {
  std::vector<std::pair<std::string, std::string>> va, vb;
  for (const auto& d : a) va.emplace_back(d.key(), d.toString());
  for (const auto& d : b) vb.emplace_back(d.key(), d.toString());
  std::stable_sort(va.begin(), va.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  std::stable_sort(vb.begin(), vb.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  CHECK_EQ(va.size(), vb.size());
  for (std::size_t i = 0; i < va.size() && i < vb.size(); ++i) {
    CHECK_EQ(va[i].first, vb[i].first);
    CHECK_EQ(va[i].second, vb[i].second);
  }
}

}  // namespace

TEST(detect_types) {
  const std::pair<const char*, ImageType> cases[] = {
      {"photo.jpg", ImageType::jpeg},  {"multi.mpo", ImageType::jpeg},       {"photo.png", ImageType::png},
      {"photo.webp", ImageType::webp}, {"photo.tif", ImageType::tiff},       {"photo.heic", ImageType::heif},
      {"photo.avif", ImageType::avif}, {"photo.xmp", ImageType::xmpSidecar},
  };
  for (const auto& [name, type] : cases) {
    CHECK_EQ(toString(Image::open(testing::readData(name))->type()), std::string(toString(type)));
  }
  CHECK_THROWS(Image::open(Bytes{'n', 'o', 'p', 'e'}), ErrorCode::unsupportedFormat);
  CHECK_THROWS(Image::open(Bytes{}), ErrorCode::unsupportedFormat);
}

TEST(jpeg_read) {
  const auto image = load("photo.jpg");
  checkFixtureExif(*image);
  checkFixtureXmp(*image);
  CHECK_EQ(image->pixelWidth(), 64u);
  CHECK_EQ(image->pixelHeight(), 48u);
  CHECK_EQ(image->comment(), "hello comment");
  CHECK((image->iptcData().values("Iptc.Application2.Keywords") ==
         std::vector<std::string>{"harbour", testing::kKobenhavn}));
  CHECK_EQ(image->exifData().thumbnail().size(), 647u);
  CHECK_EQ(image->exifData().thumbnail()[0], 0xff);
  CHECK(image->canWrite(MetadataKind::iptc));
}

TEST(jpeg_big_endian) {
  const auto image = load("motorola.jpg");
  CHECK(image->exifData().byteOrder() == ByteOrder::bigEndian);
  checkFixtureExif(*image);
  const auto again = roundTrip(*image);
  CHECK(again->exifData().byteOrder() == ByteOrder::bigEndian);
  checkSame(image->exifData(), again->exifData());
}

TEST(jpeg_unchanged_write_keeps_everything) {
  const Bytes original = testing::readData("photo.jpg");
  const auto image = load("photo.jpg");
  const auto again = roundTrip(*image);
  checkSame(image->exifData(), again->exifData());
  checkSame(image->iptcData(), again->iptcData());
  checkSame(image->xmpData(), again->xmpData());
  CHECK_EQ(again->comment(), "hello comment");
  CHECK(again->exifData().thumbnail() == image->exifData().thumbnail());

  // The image data is copied byte for byte.
  MemorySink sink;
  image->writeMetadata(sink);
  const Bytes& out = sink.data();
  const auto a = scanStart(original), b = scanStart(out);
  CHECK(original.size() - a == out.size() - b);
  CHECK(std::equal(original.begin() + static_cast<std::ptrdiff_t>(a), original.end(),
                   out.begin() + static_cast<std::ptrdiff_t>(b)));
}

TEST(jpeg_edit_round_trip) {
  auto image = load("photo.jpg");
  image->exifData()["Exif.Image.Artist"] = "Grace Hopper";
  image->exifData().erase("Exif.GPSInfo.GPSAltitude");
  image->xmpData()["Xmp.xmp.Rating"] = "2";
  image->xmpData()["Xmp.dc.subject"] = std::vector<std::string>{"one"};
  image->iptcData().setValues("Iptc.Application2.Keywords", {testing::kAlesund});
  image->comment() = "";
  const auto again = roundTrip(*image);
  CHECK_EQ(again->exifData().find("Exif.Image.Artist")->toString(), "Grace Hopper");
  CHECK(again->exifData().find("Exif.GPSInfo.GPSAltitude") == nullptr);
  CHECK_EQ(again->xmpData().find("Xmp.xmp.Rating")->toString(), "2");
  CHECK_EQ(again->xmpData().find("Xmp.dc.subject")->toString(), "one");
  CHECK((again->iptcData().values("Iptc.Application2.Keywords") == std::vector<std::string>{testing::kAlesund}));
  CHECK(again->comment().empty());
  CHECK_EQ(again->exifData().thumbnail().size(), 647u);
}

TEST(jpeg_metadata_added_to_bare_file_and_removed_again) {
  auto image = load("bare.jpg");
  CHECK(image->exifData().empty());
  image->exifData()["Exif.Image.Make"] = "PhotoCo";
  image->xmpData()["Xmp.xmp.Rating"] = "3";
  image->iptcData()["Iptc.Application2.City"] = "Oslo";
  auto again = roundTrip(*image);
  CHECK_EQ(again->exifData().find("Exif.Image.Make")->toString(), "PhotoCo");
  CHECK_EQ(again->xmpData().find("Xmp.xmp.Rating")->toString(), "3");
  CHECK_EQ(again->iptcData().find("Iptc.Application2.City")->toString(), "Oslo");

  again->exifData().clear();
  again->xmpData().clear();
  again->iptcData().clear();
  MemorySink sink;
  again->writeMetadata(sink);
  CHECK(sink.data() == testing::readData("bare.jpg"));
}

TEST(jpeg_multi_picture_offsets_follow_the_edit) {
  auto image = load("multi.mpo");
  image->xmpData()["Xmp.dc.description"] = std::string(3000, 'x');  // grows the header a lot
  MemorySink sink;
  image->writeMetadata(sink);
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
  auto image = load("photo.png");
  checkFixtureExif(*image);
  checkFixtureXmp(*image);
  CHECK_EQ(image->pixelWidth(), 64u);
  image->xmpData()["Xmp.xmp.Rating"] = "1";
  image->iptcData()["Iptc.Application2.City"] = "Oslo";
  const auto again = roundTrip(*image);
  checkFixtureExif(*again);
  CHECK_EQ(again->xmpData().find("Xmp.xmp.Rating")->toString(), "1");
  CHECK_EQ(again->iptcData().find("Iptc.Application2.City")->toString(), "Oslo");
}

TEST(png_chunks_are_valid) {
  auto image = load("bare.png");
  image->exifData()["Exif.Image.Make"] = "PhotoCo";
  image->xmpData()["Xmp.xmp.Rating"] = "5";
  MemorySink sink;
  image->writeMetadata(sink);
  const Bytes& out = sink.data();
  // Walk the chunks; IHDR first, IEND last, eXIf before IDAT.
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
  const auto exif = std::find(types.begin(), types.end(), "eXIf");
  CHECK(exif != types.end());
  CHECK(exif < std::find(types.begin(), types.end(), "IDAT"));
}

TEST(webp_read_write) {
  auto image = load("photo.webp");
  checkFixtureExif(*image);
  checkFixtureXmp(*image);
  CHECK_EQ(image->pixelWidth(), 64u);
  CHECK_EQ(image->pixelHeight(), 48u);
  CHECK(!image->canWrite(MetadataKind::iptc));
  image->exifData()["Exif.Image.Artist"] = "Me";
  const auto again = roundTrip(*image);
  CHECK_EQ(again->exifData().find("Exif.Image.Artist")->toString(), "Me");
  checkFixtureXmp(*again);
}

TEST(webp_simple_files_become_extended) {
  for (const char* name : {"lossy.webp", "lossless.webp"}) {
    auto image = load(name);
    CHECK(image->exifData().empty());
    image->xmpData()["Xmp.xmp.Rating"] = "5";
    image->exifData()["Exif.Image.Orientation"] = std::uint16_t{3};
    MemorySink sink;
    image->writeMetadata(sink);
    const Bytes& out = sink.data();
    CHECK(std::memcmp(out.data() + 12, "VP8X", 4) == 0);
    CHECK_EQ(out[20] & 0x0c, 0x0c);  // EXIF and XMP flags
    const std::uint32_t riff = out[4] | out[5] << 8 | out[6] << 16 | static_cast<std::uint32_t>(out[7]) << 24;
    CHECK_EQ(riff + 8, out.size());
    auto again = Image::open(Bytes(out));
    again->readMetadata();
    CHECK_EQ(again->pixelWidth(), 64u);
    CHECK_EQ(again->pixelHeight(), 48u);
    CHECK_EQ(again->exifData().find("Exif.Image.Orientation")->toInt64(), 3);
  }
}

TEST(tiff_read_only) {
  const auto image = load("photo.tif");
  checkFixtureExif(*image);
  checkFixtureXmp(*image);
  CHECK_EQ(image->iptcData().find("Iptc.Application2.City")->toString(), "Copenhagen");
  CHECK_EQ(image->pixelWidth(), 64u);
  // The XMP and IPTC blocks are not listed as Exif tags.
  CHECK(image->exifData().find("Exif.Image.XMLPacket") == nullptr);
  CHECK(!image->canWrite(MetadataKind::exif));
  MemorySink sink;
  CHECK_THROWS(image->writeMetadata(sink), ErrorCode::unsupportedOperation);
}

TEST(heif_and_avif_read) {
  for (const char* name : {"photo.heic", "photo.avif"}) {
    const auto image = load(name);
    CHECK_EQ(image->exifData().find("Exif.Image.Model")->toString(), "Model H");
    CHECK_EQ(image->exifData().find("Exif.Photo.DateTimeOriginal")->toString(), "2024:05:17 18:42:07");
    checkFixtureXmp(*image);
    CHECK_EQ(image->pixelWidth(), 64u);
    CHECK_EQ(image->pixelHeight(), 48u);
  }
}

TEST(xmp_sidecar) {
  auto image = load("photo.xmp");
  checkFixtureXmp(*image);
  CHECK(!image->canWrite(MetadataKind::exif));
  image->xmpData()["Xmp.xmp.Rating"] = "5";
  const auto again = roundTrip(*image);
  CHECK(again->type() == ImageType::xmpSidecar);
  CHECK_EQ(again->xmpData().find("Xmp.xmp.Rating")->toString(), "5");

  auto fresh = Image::createXmpSidecar();
  fresh->xmpData()["Xmp.dc.title"] = "New";
  fresh->writeMetadata();
  const Bytes* buffer = fresh->buffer();
  CHECK(buffer != nullptr);
  auto reopened = Image::open(Bytes(*buffer));
  reopened->readMetadata();
  CHECK_EQ(*reopened->xmpData().find("Xmp.dc.title")->value().langText(), "New");
}

TEST(file_write_replaces_the_file) {
  const auto dir = std::filesystem::temp_directory_path() / "photos-tests";
  std::filesystem::create_directories(dir);
  const auto file = dir / "edit.jpg";
  std::filesystem::copy_file(testing::dataPath("photo.jpg"), file, std::filesystem::copy_options::overwrite_existing);
  {
    auto image = Image::open(file);
    image->readMetadata();
    image->xmpData()["Xmp.xmp.Rating"] = "1";
    image->writeMetadata();
    // Still usable after the write.
    image->readMetadata();
    CHECK_EQ(image->xmpData().find("Xmp.xmp.Rating")->toString(), "1");
  }
  auto image = Image::open(file);
  image->readMetadata();
  CHECK_EQ(image->xmpData().find("Xmp.xmp.Rating")->toString(), "1");
  checkFixtureExif(*image);
  // No temporary files are left behind.
  std::size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) files += e.is_regular_file() ? 1 : 0;
  CHECK_EQ(files, 1u);
  std::filesystem::remove_all(dir);
  CHECK_THROWS(Image::open(dir / "missing.jpg"), ErrorCode::io);
}

TEST(jpeg_xl_container_read) {
  using testing::box;
  using testing::bytesOf;
  ExifData exif;
  exif["Exif.Image.Make"] = "PhotoCo";
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
  auto image = Image::open(Bytes(jxl));
  CHECK(image->type() == ImageType::jxl);
  image->readMetadata();
  CHECK_EQ(image->exifData().find("Exif.Image.Make")->toString(), "PhotoCo");
  CHECK_EQ(image->xmpData().find("Xmp.xmp.Rating")->toString(), "2");
}

TEST(cr3_read) {
  using testing::box;
  using testing::bytesOf;
  // CMT1 is IFD0, CMT2 the Exif IFD and CMT4 the GPS IFD, each a TIFF
  // structure whose first IFD holds the tags.
  ExifData ifd0, exifIfd, gpsIfd;
  ifd0["Exif.Image.Model"] = "Canon EOS R5";
  exifIfd.add(ExifKey(IfdId::ifd0, 0x829a), Value::rationals(TypeId::unsignedRational, {{1, 500}}));
  gpsIfd.add(ExifKey(IfdId::ifd0, 0x0001), Value::ascii("S"));
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
  auto image = Image::open(Bytes(cr3));
  CHECK(image->type() == ImageType::cr3);
  image->readMetadata();
  CHECK_EQ(image->exifData().find("Exif.Image.Model")->toString(), "Canon EOS R5");
  CHECK_EQ(image->exifData().find("Exif.Photo.ExposureTime")->toString(), "1/500");
  CHECK_EQ(image->exifData().find("Exif.GPSInfo.GPSLatitudeRef")->toString(), "S");
  CHECK_EQ(image->xmpData().find("Xmp.xmp.Rating")->toString(), "5");
  CHECK(!image->canWrite(MetadataKind::exif));
}

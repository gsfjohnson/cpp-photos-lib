// A HEIF's primary image as coded, and the orientation its irot and imir give.
#include "testing.hpp"

using namespace lumenlib;

namespace {

HeifImage read(const char* name) {
  const MemorySource source(testing::readData(name));
  return readHeifImage(source);
}

}  // namespace

TEST(heif_image_plain) {
  const HeifImage heic = read("photo.heic");
  CHECK_EQ(heic.brand, std::string("heic"));
  CHECK_EQ(heic.itemType, std::string("hvc1"));
  // Coded 64x64, with a clean aperture of 64x48 that is not an orientation.
  CHECK_EQ(heic.width, 64u);
  CHECK_EQ(heic.height, 64u);
  CHECK_EQ(heic.orientation, 1);

  const HeifImage avif = read("photo.avif");
  CHECK_EQ(avif.brand, std::string("avif"));
  CHECK_EQ(avif.itemType, std::string("av01"));
  CHECK_EQ(avif.width, 64u);
  CHECK_EQ(avif.height, 48u);
  CHECK_EQ(avif.orientation, 1);
}

TEST(heif_image_turned) {
  // Coded 64x48 and shown a quarter turn clockwise: irot 270.
  const HeifImage turned = read("turned.avif");
  CHECK_EQ(turned.width, 64u);
  CHECK_EQ(turned.height, 48u);
  CHECK_EQ(turned.orientation, 6);
  // ImageFile's size is the image as shown.
  auto file = ImageFile::open(testing::readData("turned.avif"));
  file->load();
  CHECK_EQ(file->width(), 48u);
  CHECK_EQ(file->height(), 64u);

  // irot 270, then imir with axis 0 (top and bottom swap): transverse.
  const HeifImage transverse = read("transverse.avif");
  CHECK_EQ(transverse.width, 64u);
  CHECK_EQ(transverse.height, 48u);
  CHECK_EQ(transverse.orientation, 7);
  CHECK_EQ(readHeifImage(testing::dataPath("transverse.avif")).orientation, 7);
}

TEST(heif_image_refuses_others) {
  const MemorySource jpeg(testing::readData("photo.jpg"));
  CHECK_THROWS(readHeifImage(jpeg), ErrorCode::unsupportedFormat);
  const MemorySource movie(testing::readData("iphone.mov"));
  CHECK_THROWS(readHeifImage(movie), ErrorCode::unsupportedFormat);
  const MemorySource empty(Bytes{});
  CHECK_THROWS(readHeifImage(empty), ErrorCode::unsupportedFormat);
  // A HEIF brand with no meta box after it.
  Bytes ftyp = testing::readData("photo.heic");
  ftyp.resize((std::size_t{ftyp[0]} << 24) | (std::size_t{ftyp[1]} << 16) | (std::size_t{ftyp[2]} << 8) | ftyp[3]);
  const MemorySource bare(std::move(ftyp));
  CHECK_THROWS(readHeifImage(bare), ErrorCode::corruptData);
}

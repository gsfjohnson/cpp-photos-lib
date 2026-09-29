// What a HEIF, HEIC or AVIF file's primary image is, read from its meta box's
// item properties: the size it is coded at and how it is turned to be shown.
// No pixel is decoded, so a program can size and orient a photo whose codec it
// does not have, and hand the decoding to a platform decoder that turns the
// picture itself.
#pragma once

#include <lumenlib/export.hpp>
#include <lumenlib/io.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace lumenlib {

struct LUMENLIB_EXPORT HeifImage {
  std::string brand;       // ftyp's major brand: "heic", "mif1", "avif"...
  std::string itemType;    // the primary item's type: "hvc1", "av01", "grid", "iden"...
  std::uint32_t width = 0; // its ispe: the image as coded, before any transformation
  std::uint32_t height = 0;
  // Its irot and imir, in the order the file associates them, as the Exif
  // orientation (1..8) that turns the coded image the way it is shown. imir's
  // axis 0 swaps top and bottom and 1 swaps left and right, as libheif and
  // Apple read it. A clap crop is not an orientation and is left out.
  int orientation = 1;
};

// Reads the primary image. Throws Error(unsupportedFormat) when the data is
// not an ISO media file with a HEIF brand, and Error(corruptData) when its
// meta box is damaged, names no primary item, or gives it no size.
LUMENLIB_EXPORT HeifImage readHeifImage(const InputSource& source);
LUMENLIB_EXPORT HeifImage readHeifImage(const std::filesystem::path& path);

}  // namespace lumenlib

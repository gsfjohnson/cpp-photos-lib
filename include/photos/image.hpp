// An image file and its metadata.
//
//   auto image = photos::Image::open("IMG_0001.jpg");
//   image->readMetadata();
//   std::string model = image->exifData()["Exif.Image.Model"].toString();
//   image->xmpData()["Xmp.xmp.Rating"] = "4";
//   image->writeMetadata();
//
// Formats and what they support:
//
//   format                         read              write
//   JPEG                           Exif IPTC XMP     Exif IPTC XMP
//                                  comment ICC       comment
//   PNG                            Exif IPTC XMP ICC Exif IPTC XMP
//   WebP                           Exif XMP ICC      Exif XMP
//   TIFF and TIFF-based raw        Exif IPTC XMP     -
//     (DNG, CR2, NEF, ARW, ORF, RW2, PEF, SRW, ...)
//   HEIF/HEIC, AVIF                Exif XMP          -
//   Canon CR3                      Exif XMP          -
//   JPEG XL (container)            Exif XMP          -
//   XMP sidecar (.xmp)             XMP               XMP
//
// Metadata a format cannot hold is left out when writing (canWrite() says
// what is written). Writing rewrites only the metadata: image data, and
// anything after it (such as the extra images of a multi-picture JPEG), is
// copied unchanged.
#pragma once

#include <photos/exif.hpp>
#include <photos/export.hpp>
#include <photos/io.hpp>
#include <photos/iptc.hpp>
#include <photos/types.hpp>
#include <photos/xmp.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace photos {

enum class ImageType {
  unknown,
  jpeg,
  png,
  webp,
  tiff,  // TIFF and TIFF-based raw
  heif,  // HEIF/HEIC
  avif,
  cr3,
  jxl,
  xmpSidecar,
};

PHOTOS_EXPORT const char* toString(ImageType type) noexcept;
PHOTOS_EXPORT const char* mimeType(ImageType type) noexcept;
// Identifies the format from the first bytes of the source.
PHOTOS_EXPORT ImageType detectImageType(const InputSource& source);

enum class MetadataKind : unsigned {
  exif = 1,
  iptc = 2,
  xmp = 4,
  comment = 8,
  iccProfile = 16,
};

class PHOTOS_EXPORT Image {
 public:
  virtual ~Image();
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  // Opens a file, detecting its format from its content. Nothing is read
  // until readMetadata(). Throws Error(io) or Error(unsupportedFormat).
  static std::unique_ptr<Image> open(const std::filesystem::path& path);
  // Opens an image held in memory; writeMetadata() replaces the buffer.
  static std::unique_ptr<Image> open(Bytes data);
  // Opens a custom source. writeMetadata() without a sink is not possible;
  // write with writeMetadata(OutputSink&).
  static std::unique_ptr<Image> open(std::unique_ptr<InputSource> source);
  // A new, empty XMP sidecar in memory.
  static std::unique_ptr<Image> createXmpSidecar();

  ImageType type() const noexcept { return type_; }
  const char* mimeType() const noexcept { return photos::mimeType(type_); }

  // Reads the metadata, replacing what the containers hold.
  // Throws Error(corruptData) when the file is damaged.
  void readMetadata();

  // Writes the metadata back to the file (through a temporary file that
  // replaces it) or the memory buffer the image was opened from.
  void writeMetadata();
  // Writes the whole image, with the current metadata, to the sink.
  // Throws Error(unsupportedOperation) for a read-only format.
  void writeMetadata(OutputSink& sink) const;

  // Whether the format can hold / this library writes the kind.
  bool canRead(MetadataKind kind) const noexcept;
  bool canWrite(MetadataKind kind) const noexcept;

  ExifData& exifData() noexcept { return exif_; }
  const ExifData& exifData() const noexcept { return exif_; }
  IptcData& iptcData() noexcept { return iptc_; }
  const IptcData& iptcData() const noexcept { return iptc_; }
  XmpData& xmpData() noexcept { return xmp_; }
  const XmpData& xmpData() const noexcept { return xmp_; }
  // The JPEG comment (COM segment).
  std::string& comment() noexcept { return comment_; }
  const std::string& comment() const noexcept { return comment_; }
  // The embedded ICC profile, if any.
  const Bytes& iccProfile() const noexcept { return icc_; }

  // The XMP packet as read, before parsing; empty if there was none.
  const std::string& xmpPacket() const noexcept { return xmpPacket_; }

  // The pixel size from the image's own headers (not Exif); 0 when unknown.
  std::uint32_t pixelWidth() const noexcept { return width_; }
  std::uint32_t pixelHeight() const noexcept { return height_; }

  // The file's path, when opened from one.
  const std::optional<std::filesystem::path>& path() const noexcept { return path_; }
  // The buffer of an image opened from memory, or nullptr.
  const Bytes* buffer() const noexcept;
  const InputSource& source() const noexcept { return *source_; }

 protected:
  Image(ImageType type, std::unique_ptr<InputSource> source, unsigned readable, unsigned writable);

  virtual void doReadMetadata() = 0;
  virtual void doWriteMetadata(OutputSink& sink) const;

  // For the format readers: parse an XMP packet into xmp_ (keeping it in
  // xmpPacket_), tolerating a damaged one.
  void setXmpPacket(std::string packet);

  ExifData exif_;
  IptcData iptc_;
  XmpData xmp_;
  std::string comment_;
  Bytes icc_;
  std::string xmpPacket_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;

 private:
  static std::unique_ptr<Image> create(ImageType type, std::unique_ptr<InputSource> source);

  ImageType type_;
  std::unique_ptr<InputSource> source_;
  std::optional<std::filesystem::path> path_;
  unsigned readable_;
  unsigned writable_;
};

}  // namespace photos

// An image file and its metadata.
//
//   auto file = lumenlib::ImageFile::open("IMG_0001.jpg");
//   file->load();
//   std::string model = file->exif().find("ifd0.Model")->text();
//   file->xmp().setText("xmp:Rating", "4");
//   file->save();
//
// Formats and what they support:
//
//   format                         read              write
//   JPEG                           Exif IPTC XMP     Exif IPTC XMP
//                                  comment ICC       comment ICC
//   PNG                            Exif IPTC XMP ICC Exif IPTC XMP ICC
//   WebP                           Exif XMP ICC      Exif XMP ICC
//   TIFF and TIFF-based raw        Exif IPTC XMP ICC Exif IPTC XMP ICC
//     (DNG, CR2, NEF, ARW, ORF, PEF, SRW, ...)
//   Panasonic RW2                  Exif IPTC XMP ICC -
//   Fujifilm RAF                   Exif IPTC XMP ICC -
//   HEIF/HEIC, AVIF                Exif XMP          -
//   Canon CR3                      Exif XMP          -
//   JPEG XL (container)            Exif XMP          -
//   XMP sidecar (.xmp)             XMP               XMP
//
// Metadata a format cannot hold is left out when writing (canWrite() says
// what is written). Writing rewrites only the metadata: image data, and
// anything after it (such as the extra images of a multi-picture JPEG), is
// copied unchanged.
//
// In a TIFF or raw file only IFD0's descriptive tags (Make, Model,
// Orientation, Artist, Copyright, ImageDescription, DateTime, Software,
// resolution, ratings, ...) and the Exif, GPS and interoperability IFDs are
// written from exif(); the tags that describe the image data (its size,
// strips, tiles, compression, a DNG's colour data) and every other IFD stay
// as they were, whatever exif() says. The maker note stays where it is. The
// new IFDs go at the end of the file, and the old ones are blanked, so
// nothing removed can be read back.
#pragma once

#include <lumenlib/exif.hpp>
#include <lumenlib/export.hpp>
#include <lumenlib/io.hpp>
#include <lumenlib/iptc.hpp>
#include <lumenlib/types.hpp>
#include <lumenlib/xmp.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace lumenlib {

enum class FileFormat {
  unknown,
  jpeg,
  png,
  webp,
  tiff,  // TIFF and TIFF-based raw
  rw2,   // Panasonic raw: a TIFF whose metadata a JPEG inside it repeats
  raf,   // Fujifilm raw
  heif,  // HEIF/HEIC
  avif,
  cr3,
  jxl,
  xmp,  // an XMP sidecar
};

// "JPEG", "PNG", "TIFF", ...
LUMENLIB_EXPORT const char* formatName(FileFormat format) noexcept;
LUMENLIB_EXPORT const char* mimeType(FileFormat format) noexcept;
// Identifies the format from the first bytes of the source.
LUMENLIB_EXPORT FileFormat detectFormat(const InputSource& source);
// As above, reading the file's first bytes. Throws Error(io).
LUMENLIB_EXPORT FileFormat detectFormat(const std::filesystem::path& path);

enum class MetadataKind : unsigned {
  exif = 1,
  iptc = 2,
  xmp = 4,
  comment = 8,
  iccProfile = 16,
};

// What the format holds / what this library writes into it, without opening
// a file.
LUMENLIB_EXPORT bool formatCanRead(FileFormat format, MetadataKind kind) noexcept;
LUMENLIB_EXPORT bool formatCanWrite(FileFormat format, MetadataKind kind) noexcept;

class LUMENLIB_EXPORT ImageFile {
 public:
  virtual ~ImageFile();
  ImageFile(const ImageFile&) = delete;
  ImageFile& operator=(const ImageFile&) = delete;

  // Opens a file, detecting its format from its content. Nothing is read
  // until load(). Throws Error(io) or Error(unsupportedFormat).
  static std::unique_ptr<ImageFile> open(const std::filesystem::path& path);
  // Opens an image held in memory; save() replaces the buffer.
  static std::unique_ptr<ImageFile> open(Bytes data);
  // Opens a custom source. save() without a sink is not possible; write
  // with saveTo(OutputSink&).
  static std::unique_ptr<ImageFile> open(std::unique_ptr<InputSource> source);
  // A new, empty XMP sidecar in memory.
  static std::unique_ptr<ImageFile> newXmpSidecar();

  FileFormat format() const noexcept { return format_; }
  const char* mimeType() const noexcept { return lumenlib::mimeType(format_); }

  // Reads the metadata, replacing what the containers hold.
  // Throws Error(corruptData) when the file is damaged.
  void load();

  // Writes the metadata back to the file (through a temporary file that
  // replaces it) or the memory buffer the image was opened from.
  void save();
  // Writes the whole image, with the current metadata, to the sink.
  // Throws Error(unsupportedOperation) for a read-only format.
  void saveTo(OutputSink& sink) const;

  // Whether the format can hold / this library writes the kind.
  bool canRead(MetadataKind kind) const noexcept;
  bool canWrite(MetadataKind kind) const noexcept;

  ExifMetadata& exif() noexcept { return exif_; }
  const ExifMetadata& exif() const noexcept { return exif_; }
  IptcMetadata& iptc() noexcept { return iptc_; }
  const IptcMetadata& iptc() const noexcept { return iptc_; }
  XmpMetadata& xmp() noexcept { return xmp_; }
  const XmpMetadata& xmp() const noexcept { return xmp_; }
  // The JPEG comment (COM segment).
  std::string& comment() noexcept { return comment_; }
  const std::string& comment() const noexcept { return comment_; }

  // The embedded ICC profile, if any.
  const Bytes& iccProfile() const noexcept { return icc_; }
  // Replaces the profile written; empty removes it. A profile nobody set is
  // copied as it was.
  void setIccProfile(Bytes profile);

  // Removes the Exif, IPTC and XMP data and the comment. The ICC profile,
  // which says how to read the pixels, stays.
  void clearMetadata();

  // The XMP packet as read, before parsing; empty if there was none.
  const std::string& xmpPacket() const noexcept { return xmpPacket_; }

  // The pixel size from the image's own headers (not Exif); 0 when unknown.
  std::uint32_t width() const noexcept { return width_; }
  std::uint32_t height() const noexcept { return height_; }

  // The file's path, when opened from one.
  const std::optional<std::filesystem::path>& path() const noexcept { return path_; }
  // The buffer of an image opened from memory (after save(), the new
  // bytes), or nullptr.
  const Bytes* buffer() const noexcept;
  const InputSource& source() const noexcept { return *source_; }

 protected:
  ImageFile(FileFormat format, std::unique_ptr<InputSource> source);

  virtual void doLoad() = 0;
  virtual void doSave(OutputSink& sink) const;

  // For the format readers: parse an XMP packet into xmp_ (keeping it in
  // xmpPacket_), tolerating a damaged one.
  void setXmpPacket(std::string packet);

  ExifMetadata exif_;
  IptcMetadata iptc_;
  XmpMetadata xmp_;
  std::string comment_;
  Bytes icc_;
  bool iccChanged_ = false;
  std::string xmpPacket_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;

 private:
  static std::unique_ptr<ImageFile> create(FileFormat format, std::unique_ptr<InputSource> source);

  FileFormat format_;
  std::unique_ptr<InputSource> source_;
  std::optional<std::filesystem::path> path_;
};

}  // namespace lumenlib

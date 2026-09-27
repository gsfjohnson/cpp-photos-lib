// Internal: the format implementations behind ImageFile::open.
#pragma once

#include <lumenlib/error.hpp>
#include <lumenlib/image_file.hpp>

#include <memory>
#include <string>

namespace lumenlib::detail {

constexpr unsigned kExif = static_cast<unsigned>(MetadataKind::exif);
constexpr unsigned kIptc = static_cast<unsigned>(MetadataKind::iptc);
constexpr unsigned kXmp = static_cast<unsigned>(MetadataKind::xmp);
constexpr unsigned kComment = static_cast<unsigned>(MetadataKind::comment);
constexpr unsigned kIcc = static_cast<unsigned>(MetadataKind::iccProfile);

std::unique_ptr<ImageFile> newJpegFile(std::unique_ptr<InputSource> source);
std::unique_ptr<ImageFile> newPngFile(std::unique_ptr<InputSource> source);
std::unique_ptr<ImageFile> newWebpFile(std::unique_ptr<InputSource> source);
// TIFF and TIFF-based raw, and RW2 (read only).
std::unique_ptr<ImageFile> newTiffFile(FileFormat format, std::unique_ptr<InputSource> source);
std::unique_ptr<ImageFile> newRafFile(std::unique_ptr<InputSource> source);
std::unique_ptr<ImageFile> newBmffFile(FileFormat format, std::unique_ptr<InputSource> source);
std::unique_ptr<ImageFile> newXmpSidecarFile(std::unique_ptr<InputSource> source);

// Non-owning view of bytes, for decoding a block already in memory.
class SpanSource final : public InputSource {
 public:
  SpanSource(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  std::uint64_t size() const override { return size_; }
  void read(std::uint64_t offset, void* dst, std::size_t n) const override;

 private:
  const std::uint8_t* data_;
  std::size_t size_;
};

// A window [offset, offset + size) of another source.
class SubSource final : public InputSource {
 public:
  SubSource(const InputSource& parent, std::uint64_t offset, std::uint64_t size)
      : parent_(parent), offset_(offset), size_(size) {}
  std::uint64_t size() const override { return size_; }
  void read(std::uint64_t offset, void* dst, std::size_t n) const override {
    if (!contains(offset, n)) throw Error(ErrorCode::corruptData, "read past the end of the data");
    parent_.read(offset_ + offset, dst, n);
  }

 private:
  const InputSource& parent_;
  std::uint64_t offset_;
  std::uint64_t size_;
};

// Strips an "Exif\0\0" prefix some writers put before the TIFF header.
inline void stripExifPrefix(const std::uint8_t*& data, std::size_t& size) {
  if (size >= 6 && data[0] == 'E' && data[1] == 'x' && data[2] == 'i' && data[3] == 'f' && data[4] == 0) {
    data += 6;
    size -= 6;
  }
}

// UTF-8 text of a path, in C++17 and C++20 alike.
inline std::string pathText(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// Reads the JPEG segments' metadata into `file`'s containers (jpeg.cpp);
// the RAF reader uses it for the JPEG a RAF file carries.
struct JpegMetadata {
  ExifMetadata exif;
  IptcMetadata iptc;
  std::string xmpPacket;
  std::string comment;
  Bytes icc;
  std::uint32_t width = 0, height = 0;
};
JpegMetadata readJpegMetadata(const InputSource& source);

}  // namespace lumenlib::detail

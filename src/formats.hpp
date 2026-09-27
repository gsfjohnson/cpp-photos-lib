// Internal: the format implementations behind Image::open.
#pragma once

#include <photos/image.hpp>

#include <memory>

namespace photos::detail {

constexpr unsigned kExif = static_cast<unsigned>(MetadataKind::exif);
constexpr unsigned kIptc = static_cast<unsigned>(MetadataKind::iptc);
constexpr unsigned kXmp = static_cast<unsigned>(MetadataKind::xmp);
constexpr unsigned kComment = static_cast<unsigned>(MetadataKind::comment);
constexpr unsigned kIcc = static_cast<unsigned>(MetadataKind::iccProfile);

std::unique_ptr<Image> newJpegImage(std::unique_ptr<InputSource> source);
std::unique_ptr<Image> newPngImage(std::unique_ptr<InputSource> source);
std::unique_ptr<Image> newWebpImage(std::unique_ptr<InputSource> source);
std::unique_ptr<Image> newTiffImage(std::unique_ptr<InputSource> source);
std::unique_ptr<Image> newBmffImage(ImageType type, std::unique_ptr<InputSource> source);
std::unique_ptr<Image> newXmpSidecar(std::unique_ptr<InputSource> source);

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

// Strips an "Exif\0\0" prefix some writers put before the TIFF header.
inline void stripExifPrefix(const std::uint8_t*& data, std::size_t& size) {
  if (size >= 6 && data[0] == 'E' && data[1] == 'x' && data[2] == 'i' && data[3] == 'f' && data[4] == 0) {
    data += 6;
    size -= 6;
  }
}

}  // namespace photos::detail

namespace photos::detail {

// UTF-8 text of a path, in C++17 and C++20 alike.
inline std::string pathText(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

}  // namespace photos::detail

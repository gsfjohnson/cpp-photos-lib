// Fujifilm RAF, read only. After the "FUJIFILMCCD-RAW " signature and the
// camera's name, a directory gives the offset and length of a JPEG preview
// (big-endian, at bytes 84 and 88); that JPEG carries the file's Exif
// (with Fujifilm's maker note), and any XMP and IPTC. The raw data's size is
// not in that JPEG, so width() and height() stay 0.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "formats.hpp"

namespace lumenlib::detail {
namespace {

class RafFile final : public ImageFile {
 public:
  explicit RafFile(std::unique_ptr<InputSource> source) : ImageFile(FileFormat::raf, std::move(source)) {}

 protected:
  void doLoad() override {
    const InputSource& src = source();
    if (src.size() < 92) corrupt("truncated RAF header");
    std::uint8_t directory[8];
    src.read(84, directory, 8);
    const std::uint32_t offset = getBe32(directory);
    const std::uint32_t length = getBe32(directory + 4);
    if (length == 0 || !src.contains(offset, length)) corrupt("RAF preview outside the file");
    const SubSource jpeg(src, offset, length);
    JpegMetadata m = readJpegMetadata(jpeg);
    exif_ = std::move(m.exif);
    iptc_ = std::move(m.iptc);
    comment_ = std::move(m.comment);
    icc_ = std::move(m.icc);
    if (!m.xmpPacket.empty()) setXmpPacket(std::move(m.xmpPacket));
  }
};

}  // namespace

std::unique_ptr<ImageFile> newRafFile(std::unique_ptr<InputSource> source) {
  return std::make_unique<RafFile>(std::move(source));
}

}  // namespace lumenlib::detail

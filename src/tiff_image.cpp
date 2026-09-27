// TIFF and TIFF-based raw formats (DNG, CR2, NEF, ARW, ORF, RW2, PEF, SRW,
// ...), read only. IFD0 and its Exif and GPS IFDs are the Exif data; XMP is
// tag 0x02BC, IPTC tag 0x83BB or a Photoshop resource block in tag 0x8649.
#include <photos/error.hpp>

#include "exif_internal.hpp"
#include "formats.hpp"
#include "photoshop.hpp"

namespace photos::detail {
namespace {

constexpr std::uint16_t kXmlPacket = 0x02bc;
constexpr std::uint16_t kIptcNaa = 0x83bb;
constexpr std::uint16_t kImageResources = 0x8649;

class TiffImage final : public Image {
 public:
  explicit TiffImage(std::unique_ptr<InputSource> source)
      : Image(ImageType::tiff, std::move(source), kExif | kIptc | kXmp, 0) {}

 protected:
  void doReadMetadata() override {
    TiffDecodeOptions options;
    options.keepOrigin = false;
    options.allowRawMagic = true;
    decodeTiff(source(), exif_, options);

    // The metadata blocks are not Exif data.
    const auto order = exif_.byteOrder();
    if (auto it = exif_.findKey(ExifKey(IfdId::ifd0, kXmlPacket)); it != exif_.end()) {
      const Bytes b = it->value().toBytes(order);
      setXmpPacket(std::string(b.begin(), b.end()));
      exif_.erase(it);
    }
    if (auto it = exif_.findKey(ExifKey(IfdId::ifd0, kIptcNaa)); it != exif_.end()) {
      // Often typed LONG, but the bytes are IIM.
      const Bytes b = it->value().toBytes(order);
      try {
        iptc_ = IptcData::decode(b.data(), b.size());
      } catch (const Error&) {
        iptc_.clear();
      }
      exif_.erase(it);
    }
    if (auto it = exif_.findKey(ExifKey(IfdId::ifd0, kImageResources)); it != exif_.end()) {
      const Bytes b = it->value().toBytes(order);
      if (iptc_.empty()) {
        try {
          if (auto iptc = iptcFromImageResources(parseImageResources(b.data(), b.size()))) iptc_ = std::move(*iptc);
        } catch (const Error&) {
          iptc_.clear();
        }
      }
      exif_.erase(it);
    }
    if (const auto* w = exif_.find("Exif.Image.ImageWidth"); w && w->count()) {
      width_ = static_cast<std::uint32_t>(w->toInt64());
    }
    if (const auto* h = exif_.find("Exif.Image.ImageLength"); h && h->count()) {
      height_ = static_cast<std::uint32_t>(h->toInt64());
    }
  }
};

}  // namespace

std::unique_ptr<Image> newTiffImage(std::unique_ptr<InputSource> source) {
  return std::make_unique<TiffImage>(std::move(source));
}

}  // namespace photos::detail

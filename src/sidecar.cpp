// XMP sidecar files (.xmp): an XMP packet on its own.
#include <photos/error.hpp>

#include "formats.hpp"

namespace photos::detail {
namespace {

constexpr std::uint64_t kMaxSidecar = 64u << 20;

class XmpSidecar final : public Image {
 public:
  explicit XmpSidecar(std::unique_ptr<InputSource> source)
      : Image(ImageType::xmpSidecar, std::move(source), kXmp, kXmp) {}

 protected:
  void doReadMetadata() override {
    const InputSource& src = source();
    if (src.size() == 0) return;
    if (src.size() > kMaxSidecar) throw Error(ErrorCode::dataTooLarge, "XMP sidecar exceeds 64 MB");
    const Bytes d = src.readBytes(0, static_cast<std::size_t>(src.size()));
    xmpPacket_.assign(d.begin(), d.end());
    // Unlike an embedded packet, a sidecar is nothing but XMP: damage is an
    // error.
    xmp_ = XmpData::parse(xmpPacket_);
  }

  void doWriteMetadata(OutputSink& sink) const override {
    XmpWriteOptions options;
    options.padding = 0;
    const std::string packet = xmp_.serialize(options);
    sink.write(packet.data(), packet.size());
  }
};

}  // namespace

std::unique_ptr<Image> newXmpSidecar(std::unique_ptr<InputSource> source) {
  return std::make_unique<XmpSidecar>(std::move(source));
}

}  // namespace photos::detail

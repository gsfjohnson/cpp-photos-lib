// WebP: Exif in the EXIF chunk, XMP in "XMP ", ICC in ICCP. Metadata needs
// the extended format, so writing it into a simple (VP8/VP8L) file adds a
// VP8X chunk. Chunk order follows the container specification: VP8X, ICCP,
// ANIM, image data, EXIF, XMP, then anything unknown.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"

#include <algorithm>

namespace lumenlib::detail {
namespace {

constexpr std::uint8_t kFlagIcc = 0x20;
constexpr std::uint8_t kFlagAlpha = 0x10;
constexpr std::uint8_t kFlagExif = 0x08;
constexpr std::uint8_t kFlagXmp = 0x04;
constexpr std::uint8_t kFlagAnimation = 0x02;

struct Chunk {
  std::string fourcc;
  std::uint64_t offset;  // of the header
  std::uint32_t size;    // of the payload
  std::uint64_t payload() const { return offset + 8; }
};

std::uint32_t getLe24(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (static_cast<std::uint32_t>(p[2]) << 16); }

void put24(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
}

std::vector<Chunk> scan(const InputSource& src) {
  std::uint8_t h[12];
  if (src.size() < 12) corrupt("not a WebP");
  src.read(0, h, 12);
  if (std::string_view(reinterpret_cast<char*>(h), 4) != "RIFF" ||
      std::string_view(reinterpret_cast<char*>(h + 8), 4) != "WEBP") {
    corrupt("not a WebP");
  }
  const std::uint64_t end = std::min<std::uint64_t>(src.size(), 8 + static_cast<std::uint64_t>(getLe32(h + 4)));
  std::vector<Chunk> chunks;
  std::uint64_t pos = 12;
  while (inBounds(end, pos, 8)) {
    std::uint8_t c[8];
    src.read(pos, c, 8);
    Chunk chunk{toText(c, 4), pos, getLe32(c + 4)};
    if (!inBounds(end, chunk.payload(), chunk.size)) corrupt("WebP chunk runs past the end");
    chunks.push_back(chunk);
    pos = chunk.payload() + chunk.size + (chunk.size & 1);
  }
  return chunks;
}

class WebpFile final : public ImageFile {
 public:
  explicit WebpFile(std::unique_ptr<InputSource> source) : ImageFile(FileFormat::webp, std::move(source)) {}

 protected:
  void doLoad() override {
    const InputSource& src = source();
    for (const auto& c : scan(src)) {
      if (c.fourcc == "VP8X" && c.size >= 10) {
        const Bytes d = src.readBytes(c.payload(), 10);
        width_ = getLe24(d.data() + 4) + 1;
        height_ = getLe24(d.data() + 7) + 1;
      } else if (c.fourcc == "VP8 " && c.size >= 10 && width_ == 0) {
        const Bytes d = src.readBytes(c.payload(), 10);
        width_ = getLe16(d.data() + 6) & 0x3fff;
        height_ = getLe16(d.data() + 8) & 0x3fff;
      } else if (c.fourcc == "VP8L" && c.size >= 5 && width_ == 0) {
        const Bytes d = src.readBytes(c.payload(), 5);
        const std::uint32_t bits = getLe32(d.data() + 1);
        width_ = (bits & 0x3fff) + 1;
        height_ = ((bits >> 14) & 0x3fff) + 1;
      } else if (c.fourcc == "ICCP" && icc_.empty()) {
        icc_ = src.readBytes(c.payload(), c.size);
      } else if (c.fourcc == "EXIF" && exif_.empty()) {
        const Bytes d = src.readBytes(c.payload(), c.size);
        const std::uint8_t* p = d.data();
        std::size_t n = d.size();
        stripExifPrefix(p, n);
        try {
          decodeTiff(p, n, exif_);
        } catch (const Error&) {
          exif_.clear();
        }
      } else if (c.fourcc == "XMP " && xmpPacket_.empty()) {
        const Bytes d = src.readBytes(c.payload(), c.size);
        setXmpPacket(toText(d.data(), d.size()));
      }
    }
  }

  void doSave(OutputSink& sink) const override {
    const InputSource& src = source();
    const auto chunks = scan(src);
    const Bytes tiff = encodeExif(exif_);
    const std::string packet = xmp_.empty() ? std::string() : xmp_.serialize();

    const Chunk* vp8x = nullptr;
    bool icc = false, alpha = false, animation = false;
    std::uint32_t width = 0, height = 0;
    for (const auto& c : chunks) {
      if (c.fourcc == "VP8X" && c.size >= 10) vp8x = &c;
      if (c.fourcc == "ICCP") icc = !iccChanged_;
      if (c.fourcc == "ALPH") alpha = true;
      if (c.fourcc == "ANIM") animation = true;
      if (c.fourcc == "VP8 " && c.size >= 10) {
        const Bytes d = src.readBytes(c.payload(), 10);
        width = getLe16(d.data() + 6) & 0x3fff;
        height = getLe16(d.data() + 8) & 0x3fff;
      }
      if (c.fourcc == "VP8L" && c.size >= 5) {
        const Bytes d = src.readBytes(c.payload(), 5);
        const std::uint32_t bits = getLe32(d.data() + 1);
        width = (bits & 0x3fff) + 1;
        height = ((bits >> 14) & 0x3fff) + 1;
        alpha |= ((bits >> 28) & 1) != 0;
      }
    }

    if (iccChanged_) icc = !icc_.empty();
    Bytes header;
    if (vp8x || !tiff.empty() || !packet.empty() || icc) {
      std::uint8_t flags = 0;
      if (vp8x) {
        const Bytes d = src.readBytes(vp8x->payload(), 10);
        flags = d[0];
        width = getLe24(d.data() + 4) + 1;
        height = getLe24(d.data() + 7) + 1;
      }
      flags = static_cast<std::uint8_t>(flags & ~(kFlagExif | kFlagXmp | kFlagIcc));
      if (icc) flags |= kFlagIcc;
      if (alpha) flags |= kFlagAlpha;
      if (animation) flags |= kFlagAnimation;
      if (!tiff.empty()) flags |= kFlagExif;
      if (!packet.empty()) flags |= kFlagXmp;
      if (width == 0 || height == 0) corrupt("WebP has no image size");
      std::uint8_t chunk[18] = {'V', 'P', '8', 'X', 10, 0, 0, 0, flags};
      put24(chunk + 12, width - 1);
      put24(chunk + 15, height - 1);
      header.assign(chunk, chunk + sizeof chunk);
    }

    Bytes trailer;
    const auto addChunk = [&](std::string_view fourcc, const std::uint8_t* data, std::size_t n) {
      if (n > 0xfffffff0u) throw Error(ErrorCode::dataTooLarge, "WebP chunk too large");
      append(trailer, fourcc);
      appendLe32(trailer, static_cast<std::uint32_t>(n));
      append(trailer, data, n);
      if (n & 1) trailer.push_back(0);
    };
    if (!tiff.empty()) addChunk("EXIF", tiff.data(), tiff.size());
    if (!packet.empty()) addChunk("XMP ", reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size());

    // Chunks kept: all but VP8X, EXIF and XMP, and unknown chunks after the
    // metadata.
    const auto known = [](const std::string& f) {
      return f == "ICCP" || f == "ANIM" || f == "ANMF" || f == "ALPH" || f == "VP8 " || f == "VP8L";
    };
    Bytes iccChunk;
    if (iccChanged_ && !icc_.empty()) {
      if (icc_.size() > 0xfffffff0u) throw Error(ErrorCode::dataTooLarge, "WebP chunk too large");
      append(iccChunk, std::string_view("ICCP"));
      appendLe32(iccChunk, static_cast<std::uint32_t>(icc_.size()));
      append(iccChunk, icc_);
      if (icc_.size() & 1) iccChunk.push_back(0);
    }
    std::vector<const Chunk*> kept, unknown;
    for (const auto& c : chunks) {
      if (c.fourcc == "VP8X" || c.fourcc == "EXIF" || c.fourcc == "XMP ") continue;
      if (c.fourcc == "ICCP" && iccChanged_) continue;
      (known(c.fourcc) ? kept : unknown).push_back(&c);
    }
    std::uint64_t total = 4 + header.size() + iccChunk.size() + trailer.size();
    for (const auto* list : {&kept, &unknown}) {
      for (const auto* c : *list) total += 8 + static_cast<std::uint64_t>(c->size) + (c->size & 1);
    }
    if (total > 0xfffffff0u) throw Error(ErrorCode::dataTooLarge, "WebP exceeds 4 GB");

    std::uint8_t riff[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    put32(riff + 4, static_cast<std::uint32_t>(total), ByteOrder::little);
    sink.write(riff, sizeof riff);
    sink.write(header);
    sink.write(iccChunk);
    const auto copy = [&](const Chunk* c) {
      sink.copyFrom(src, c->offset, 8 + static_cast<std::uint64_t>(c->size));
      if (c->size & 1) {
        const std::uint8_t pad = 0;
        sink.write(&pad, 1);
      }
    };
    for (const auto* c : kept) copy(c);
    sink.write(trailer);
    for (const auto* c : unknown) copy(c);
  }
};

}  // namespace

std::unique_ptr<ImageFile> newWebpFile(std::unique_ptr<InputSource> source) {
  return std::make_unique<WebpFile>(std::move(source));
}

}  // namespace lumenlib::detail

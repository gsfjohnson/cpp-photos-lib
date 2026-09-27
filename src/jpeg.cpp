// JPEG: Exif and XMP in APP1, ICC profiles in APP2, IPTC in APP13 (a
// Photoshop resource block), the comment in COM.
//
// Writing puts the metadata segments right after SOI (and any leading APP0
// JFIF/JFXX segments) and copies every other segment, and everything from SOS
// on, unchanged. A multi-picture (MPF, APP2) segment's offsets point past the
// primary image, so they are moved by however much the segments before them
// grew or shrank.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"
#include "photoshop.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace lumenlib::detail {
namespace {

constexpr std::uint8_t kSOI = 0xd8;
constexpr std::uint8_t kEOI = 0xd9;
constexpr std::uint8_t kSOS = 0xda;
constexpr std::uint8_t kAPP0 = 0xe0;
constexpr std::uint8_t kAPP1 = 0xe1;
constexpr std::uint8_t kAPP2 = 0xe2;
constexpr std::uint8_t kAPP13 = 0xed;
constexpr std::uint8_t kCOM = 0xfe;

constexpr std::string_view kExifId("Exif\0\0", 6);
constexpr std::string_view kXmpId("http://ns.adobe.com/xap/1.0/\0", 29);
constexpr std::string_view kXmpExtId("http://ns.adobe.com/xmp/extension/\0", 35);
constexpr std::string_view kIccId("ICC_PROFILE\0", 12);
constexpr std::string_view kPhotoshopId("Photoshop 3.0\0", 14);
constexpr std::string_view kMpfId("MPF\0", 4);

// Most a segment's payload can hold (its length field counts itself).
constexpr std::size_t kMaxPayload = 0xffff - 2;

struct Segment {
  std::uint8_t marker;
  std::uint64_t offset;  // of the 0xFF
  std::size_t length;    // payload bytes
  Bytes head;            // the payload's first bytes, enough to identify it
  std::uint64_t payload() const { return offset + 4; }
  std::uint64_t end() const { return offset + 4 + length; }
};

enum class Kind { other, jfif, exif, xmp, xmpExtension, icc, photoshop, mpf, comment };

Kind kindOf(const Segment& s) {
  const auto has = [&](std::string_view id) { return startsWith(s.head, id); };
  switch (s.marker) {
    case kAPP0:
      return (has("JFIF\0") || has("JFXX\0")) ? Kind::jfif : Kind::other;
    case kAPP1:
      if (has(kExifId) || has(std::string_view("Exif\0\xff", 6))) return Kind::exif;
      if (has(kXmpId)) return Kind::xmp;
      if (has(kXmpExtId)) return Kind::xmpExtension;
      return Kind::other;
    case kAPP2:
      if (has(kIccId)) return Kind::icc;
      if (has(kMpfId)) return Kind::mpf;
      return Kind::other;
    case kAPP13:
      return has(kPhotoshopId) ? Kind::photoshop : Kind::other;
    case kCOM:
      return Kind::comment;
    default:
      return Kind::other;
  }
}

bool isSof(std::uint8_t m) { return m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc; }

// The segments before SOS, and where SOS starts (the file's size when
// there is none).
struct Layout {
  std::vector<Segment> segments;
  std::uint64_t scanStart = 0;
};

Layout scan(const InputSource& src) {
  Layout layout;
  const std::uint64_t size = src.size();
  std::uint8_t b[4];
  if (size < 2) corrupt("not a JPEG");
  src.read(0, b, 2);
  if (b[0] != 0xff || b[1] != kSOI) corrupt("not a JPEG");
  std::uint64_t pos = 2;
  for (;;) {
    // Fill bytes before a marker are allowed.
    std::uint8_t m = 0;
    for (;;) {
      if (!inBounds(size, pos, 2)) {
        layout.scanStart = size;
        return layout;
      }
      src.read(pos, b, 2);
      if (b[0] != 0xff) corrupt("JPEG segment does not start with 0xFF");
      if (b[1] != 0xff) {
        m = b[1];
        break;
      }
      ++pos;
    }
    if (m == kSOS || m == kEOI) {
      layout.scanStart = pos;
      return layout;
    }
    if ((m >= 0xd0 && m <= 0xd7) || m == 0x01) {  // no length
      pos += 2;
      continue;
    }
    if (!inBounds(size, pos, 4)) corrupt("truncated JPEG segment");
    src.read(pos + 2, b, 2);
    const std::size_t length = getBe16(b);
    if (length < 2 || !inBounds(size, pos + 4, length - 2)) corrupt("JPEG segment runs past the end");
    Segment s{m, pos, length - 2, {}};
    s.head = src.readBytes(s.payload(), std::min<std::size_t>(s.length, 40));
    layout.segments.push_back(std::move(s));
    pos += 2 + length;
  }
}

void writeSegment(Bytes& out, std::uint8_t marker, std::string_view id, const std::uint8_t* data, std::size_t n) {
  const std::size_t payload = id.size() + n;
  if (payload > kMaxPayload) throw Error(ErrorCode::dataTooLarge, "JPEG segment too large");
  out.push_back(0xff);
  out.push_back(marker);
  appendBe16(out, static_cast<std::uint16_t>(payload + 2));
  append(out, id);
  append(out, data, n);
}

// Moves the MP Entry offsets of an MPF payload (after "MPF\0") by delta.
void patchMpf(Bytes& mpf, std::int64_t delta) {
  if (delta == 0 || !isTiffHeader(mpf.data(), mpf.size())) return;
  const ByteOrder order = mpf[0] == 'I' ? ByteOrder::little : ByteOrder::big;
  const std::uint32_t ifd = get32(mpf.data() + 4, order);
  if (!inBounds(mpf.size(), ifd, 2)) return;
  const std::uint16_t count = get16(mpf.data() + ifd, order);
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::size_t e = ifd + 2 + 12 * static_cast<std::size_t>(i);
    if (!inBounds(mpf.size(), e, 12)) return;
    if (get16(mpf.data() + e, order) != 0xb002) continue;  // MPEntry
    const std::uint32_t n = get32(mpf.data() + e + 4, order);
    const std::uint32_t at = get32(mpf.data() + e + 8, order);
    if (!inBounds(mpf.size(), at, n)) return;
    for (std::size_t k = 0; k + 16 <= n; k += 16) {
      std::uint8_t* offset = mpf.data() + at + k + 8;
      const std::uint32_t old = get32(offset, order);
      if (old == 0) continue;  // the primary image
      const std::int64_t moved = static_cast<std::int64_t>(old) + delta;
      if (moved > 0 && moved <= 0xffffffffLL) put32(offset, static_cast<std::uint32_t>(moved), order);
    }
  }
}

}  // namespace

JpegMetadata readJpegMetadata(const InputSource& src) {
  JpegMetadata m;
  const Layout layout = scan(src);
  std::map<int, Bytes> iccChunks;
  Bytes irb;
  bool haveExif = false, haveXmp = false, haveComment = false;
  for (const auto& s : layout.segments) {
    if (isSof(s.marker) && s.length >= 5 && m.width == 0) {
      m.height = getBe16(s.head.data() + 1);
      m.width = getBe16(s.head.data() + 3);
      continue;
    }
    switch (kindOf(s)) {
      case Kind::exif:
        if (!haveExif) {
          haveExif = true;
          const Bytes data = src.readBytes(s.payload() + 6, s.length - 6);
          try {
            decodeTiff(data.data(), data.size(), m.exif);
          } catch (const Error& e) {
            m.exif.clear();  // not TIFF after all
            warn(std::string("JPEG Exif segment not read: ") + e.what());
          }
        }
        break;
      case Kind::xmp:
        if (!haveXmp) {
          haveXmp = true;
          const Bytes data = src.readBytes(s.payload() + kXmpId.size(), s.length - kXmpId.size());
          m.xmpPacket = toText(data.data(), data.size());
        }
        break;
      case Kind::icc:
        if (s.length > 14) {
          iccChunks[s.head[12]] = src.readBytes(s.payload() + 14, s.length - 14);
        }
        break;
      case Kind::photoshop: {
        const Bytes data = src.readBytes(s.payload() + kPhotoshopId.size(), s.length - kPhotoshopId.size());
        append(irb, data);
        break;
      }
      case Kind::comment:
        if (!haveComment) {
          haveComment = true;
          const Bytes data = src.readBytes(s.payload(), s.length);
          m.comment = toText(data.data(), data.size());
          while (!m.comment.empty() && m.comment.back() == '\0') m.comment.pop_back();
        }
        break;
      default:
        break;
    }
  }
  for (const auto& [seq, chunk] : iccChunks) append(m.icc, chunk);
  if (!irb.empty()) {
    try {
      if (auto iptc = iptcFromImageResources(parseImageResources(irb.data(), irb.size()))) m.iptc = std::move(*iptc);
    } catch (const Error& e) {
      m.iptc.clear();
      warn(std::string("JPEG IPTC not read: ") + e.what());
    }
  }
  return m;
}

namespace {

class JpegFile final : public ImageFile {
 public:
  explicit JpegFile(std::unique_ptr<InputSource> source) : ImageFile(FileFormat::jpeg, std::move(source)) {}

 protected:
  void doLoad() override {
    JpegMetadata m = readJpegMetadata(source());
    exif_ = std::move(m.exif);
    iptc_ = std::move(m.iptc);
    comment_ = std::move(m.comment);
    icc_ = std::move(m.icc);
    width_ = m.width;
    height_ = m.height;
    if (!m.xmpPacket.empty()) setXmpPacket(std::move(m.xmpPacket));
  }

  void doSave(OutputSink& sink) const override {
    const InputSource& src = source();
    const Layout layout = scan(src);

    Bytes out;
    out.push_back(0xff);
    out.push_back(kSOI);
    std::size_t first = 0;
    for (; first < layout.segments.size() && kindOf(layout.segments[first]) == Kind::jfif; ++first) {
      const auto& s = layout.segments[first];
      append(out, src.readBytes(s.offset, 4 + s.length));
    }

    // Exif; if it is too big for a segment, without its thumbnail.
    Bytes tiff = encodeExif(exif_);
    if (tiff.size() + kExifId.size() > kMaxPayload && !exif_.thumbnail().empty()) {
      ExifMetadata smaller = exif_;
      smaller.removeThumbnail();
      tiff = encodeExif(smaller);
    }
    if (!tiff.empty()) {
      if (tiff.size() + kExifId.size() > kMaxPayload) throw Error(ErrorCode::dataTooLarge, "Exif data exceeds 64 KB");
      writeSegment(out, kAPP1, kExifId, tiff.data(), tiff.size());
    }

    if (!xmp_.empty()) {
      std::string packet = xmp_.serialize();
      if (packet.size() + kXmpId.size() > kMaxPayload) packet = xmp_.serialize({0, true, false});
      if (packet.size() + kXmpId.size() > kMaxPayload) {
        throw Error(ErrorCode::dataTooLarge, "XMP packet exceeds 64 KB (extended XMP is not written)");
      }
      writeSegment(out, kAPP1, kXmpId, reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size());
      // The extension of an unchanged packet stays valid (it is found by
      // the GUID in xmpNote:HasExtendedXMP).
      if (xmp_.find("xmpNote:HasExtendedXMP")) {
        for (const auto& s : layout.segments) {
          if (kindOf(s) == Kind::xmpExtension) append(out, src.readBytes(s.offset, 4 + s.length));
        }
      }
    }

    // IPTC, keeping the other Photoshop resources.
    Bytes irbIn;
    for (const auto& s : layout.segments) {
      if (kindOf(s) == Kind::photoshop) {
        append(irbIn, src.readBytes(s.payload() + kPhotoshopId.size(), s.length - kPhotoshopId.size()));
      }
    }
    auto resources = parseImageResources(irbIn.data(), irbIn.size());
    setIptcImageResource(resources, iptc_);
    const Bytes irb = serializeImageResources(resources);
    for (std::size_t at = 0; at < irb.size(); at += kMaxPayload - kPhotoshopId.size()) {
      const std::size_t n = std::min(irb.size() - at, kMaxPayload - kPhotoshopId.size());
      writeSegment(out, kAPP13, kPhotoshopId, irb.data() + at, n);
    }

    if (!comment_.empty()) {
      writeSegment(out, kCOM, {}, reinterpret_cast<const std::uint8_t*>(comment_.data()), comment_.size());
    }

    // A new ICC profile, in as many APP2 segments as it needs (numbered from
    // 1); an unchanged one is copied with the other segments.
    if (iccChanged_ && !icc_.empty()) {
      const std::size_t chunk = kMaxPayload - kIccId.size() - 2;
      const std::size_t chunks = (icc_.size() + chunk - 1) / chunk;
      if (chunks > 255) throw Error(ErrorCode::dataTooLarge, "ICC profile too large for a JPEG");
      for (std::size_t i = 0; i < chunks; ++i) {
        const std::size_t n = std::min(chunk, icc_.size() - i * chunk);
        Bytes payload;
        payload.push_back(static_cast<std::uint8_t>(i + 1));
        payload.push_back(static_cast<std::uint8_t>(chunks));
        append(payload, icc_.data() + i * chunk, n);
        writeSegment(out, kAPP2, kIccId, payload.data(), payload.size());
      }
    }

    // Everything else, in order. MPF offsets are relative to the MPF header,
    // and point past the image data.
    std::vector<std::pair<std::size_t, std::uint64_t>> mpfAt;  // position in out, position in the source
    for (std::size_t i = first; i < layout.segments.size(); ++i) {
      const auto& s = layout.segments[i];
      switch (kindOf(s)) {
        case Kind::exif:
        case Kind::xmp:
        case Kind::xmpExtension:
        case Kind::photoshop:
        case Kind::comment:
          continue;
        case Kind::icc:
          if (iccChanged_) continue;
          append(out, src.readBytes(s.offset, 4 + s.length));
          break;
        case Kind::mpf:
          mpfAt.emplace_back(out.size(), s.offset);
          [[fallthrough]];
        default:
          append(out, src.readBytes(s.offset, 4 + s.length));
      }
    }
    // Where the MPF header is now versus then, relative to the image data
    // after it (which moves by out.size() - scanStart).
    const std::int64_t tailDelta = static_cast<std::int64_t>(out.size()) - static_cast<std::int64_t>(layout.scanStart);
    for (const auto& [outPos, srcPos] : mpfAt) {
      const std::int64_t headerDelta = static_cast<std::int64_t>(outPos) - static_cast<std::int64_t>(srcPos);
      const std::size_t payloadAt = outPos + 4 + kMpfId.size();
      Bytes mpf(out.begin() + static_cast<std::ptrdiff_t>(payloadAt),
                out.begin() + static_cast<std::ptrdiff_t>(outPos + 4 + getBe16(out.data() + outPos + 2) - 2));
      patchMpf(mpf, tailDelta - headerDelta);
      std::copy(mpf.begin(), mpf.end(), out.begin() + static_cast<std::ptrdiff_t>(payloadAt));
    }

    sink.write(out);
    sink.copyFrom(src, layout.scanStart, src.size() - layout.scanStart);
  }
};

}  // namespace

std::unique_ptr<ImageFile> newJpegFile(std::unique_ptr<InputSource> source) {
  return std::make_unique<JpegFile>(std::move(source));
}

}  // namespace lumenlib::detail

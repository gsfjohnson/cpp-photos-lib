// PNG: Exif in eXIf, XMP in iTXt "XML:com.adobe.xmp", IPTC (and, from older
// writers, Exif and XMP) in ImageMagick-style "Raw profile type ..." text
// chunks, ICC in iCCP.
//
// Writing replaces those chunks with eXIf, iTXt and (for IPTC) a raw profile
// right after IHDR, and copies every other chunk unchanged.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"
#include "photoshop.hpp"
#include "zlib_support.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace lumenlib::detail {
namespace {

constexpr std::string_view kSignature("\x89PNG\r\n\x1a\n", 8);
constexpr std::string_view kXmpKeyword = "XML:com.adobe.xmp";
constexpr std::size_t kMaxText = 64u << 20;

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

void writeChunk(Bytes& out, std::string_view type, const Bytes& data) {
  if (data.size() > 0x7fffffffu) throw Error(ErrorCode::dataTooLarge, "PNG chunk too large");
  appendBe32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t typeAt = out.size();
  append(out, type);
  append(out, data);
  appendBe32(out, crc32(out.data() + typeAt, 4 + data.size()));
}

struct Chunk {
  std::string type;
  std::uint64_t offset;  // of the length field
  std::uint32_t length;  // of the data
  std::uint64_t data() const { return offset + 8; }
  std::uint64_t total() const { return 12 + static_cast<std::uint64_t>(length); }
};

std::vector<Chunk> scan(const InputSource& src) {
  const std::uint64_t size = src.size();
  if (size < 8 || src.readBytes(0, 8) != Bytes(kSignature.begin(), kSignature.end())) corrupt("not a PNG");
  std::vector<Chunk> chunks;
  std::uint64_t pos = 8;
  while (inBounds(size, pos, 12)) {
    std::uint8_t h[8];
    src.read(pos, h, 8);
    Chunk c{toText(h + 4, 4), pos, getBe32(h)};
    if (c.length > 0x7fffffffu || !inBounds(size, pos, c.total())) corrupt("PNG chunk runs past the end");
    chunks.push_back(c);
    pos += c.total();
    if (c.type == "IEND") break;
  }
  if (chunks.empty() || chunks.front().type != "IHDR") corrupt("PNG does not start with IHDR");
  return chunks;
}

struct TextChunk {
  std::string keyword;
  Bytes text;  // decompressed
};

// Decodes tEXt, zTXt and iTXt. std::nullopt for text that cannot be
// decompressed (no zlib).
std::optional<TextChunk> decodeText(const std::string& type, const Bytes& d) {
  const auto nul = std::find(d.begin(), d.end(), 0);
  if (nul == d.end()) return std::nullopt;
  TextChunk t{std::string(d.begin(), nul), {}};
  std::size_t pos = static_cast<std::size_t>(nul - d.begin()) + 1;
  bool compressed = false;
  if (type == "zTXt") {
    if (pos >= d.size()) return std::nullopt;
    compressed = true;
    ++pos;  // method
  } else if (type == "iTXt") {
    if (pos + 2 > d.size()) return std::nullopt;
    compressed = d[pos] != 0;
    pos += 2;
    for (int field = 0; field < 2; ++field) {  // language tag, translated keyword
      const auto end = std::find(d.begin() + static_cast<std::ptrdiff_t>(pos), d.end(), 0);
      if (end == d.end()) return std::nullopt;
      pos = static_cast<std::size_t>(end - d.begin()) + 1;
    }
  }
  if (compressed) {
    auto inflated = zlibInflate(d.data() + pos, d.size() - pos, kMaxText);
    if (!inflated) return std::nullopt;
    t.text = std::move(*inflated);
  } else {
    t.text.assign(d.begin() + static_cast<std::ptrdiff_t>(pos), d.end());
  }
  return t;
}

// ImageMagick's "\n<type>\n<length>\n<hex>" encoding.
std::optional<Bytes> decodeRawProfile(const Bytes& text) {
  std::size_t pos = 0;
  while (pos < text.size() && text[pos] == '\n') ++pos;
  while (pos < text.size() && text[pos] != '\n') ++pos;  // the type
  while (pos < text.size() && (text[pos] == '\n' || text[pos] == ' ')) ++pos;
  std::uint64_t length = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    length = length * 10 + (text[pos++] - '0');
    if (length > kMaxText) return std::nullopt;
  }
  Bytes out;
  out.reserve(static_cast<std::size_t>(length));
  int high = -1;
  for (; pos < text.size() && out.size() < length; ++pos) {
    const char c = static_cast<char>(text[pos]);
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      continue;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<std::uint8_t>(high << 4 | v));
      high = -1;
    }
  }
  if (out.size() != length) return std::nullopt;
  return out;
}

Bytes encodeRawProfile(std::string_view type, const Bytes& data) {
  static const char digits[] = "0123456789abcdef";
  std::string s = "\n" + std::string(type) + "\n";
  const std::string n = std::to_string(data.size());
  s += std::string(n.size() < 8 ? 8 - n.size() : 0, ' ') + n;
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (i % 36 == 0) s += '\n';
    s += digits[data[i] >> 4];
    s += digits[data[i] & 0xf];
  }
  s += '\n';
  return Bytes(s.begin(), s.end());
}

bool isRawProfile(const std::string& keyword, std::string_view type) {
  return keyword == "Raw profile type " + std::string(type);
}

class PngFile final : public ImageFile {
 public:
  explicit PngFile(std::unique_ptr<InputSource> source) : ImageFile(FileFormat::png, std::move(source)) {}

 protected:
  void doLoad() override {
    const InputSource& src = source();
    bool haveExif = false, haveXmp = false, haveIptc = false;
    for (const auto& c : scan(src)) {
      if (c.type == "IHDR" && c.length >= 8) {
        const Bytes d = src.readBytes(c.data(), 8);
        width_ = getBe32(d.data());
        height_ = getBe32(d.data() + 4);
      } else if (c.type == "eXIf" && !haveExif) {
        haveExif = readExif(src.readBytes(c.data(), c.length));
      } else if (c.type == "iCCP" && icc_.empty()) {
        const Bytes d = src.readBytes(c.data(), c.length);
        const auto nul = std::find(d.begin(), d.end(), 0);
        const auto at = static_cast<std::size_t>(nul - d.begin()) + 2;  // name, NUL, method
        if (nul != d.end() && at <= d.size()) {
          if (auto icc = zlibInflate(d.data() + at, d.size() - at, kMaxText)) icc_ = std::move(*icc);
        }
      } else if (c.type == "tEXt" || c.type == "zTXt" || c.type == "iTXt") {
        const auto t = decodeText(c.type, src.readBytes(c.data(), c.length));
        if (!t) continue;
        if (t->keyword == kXmpKeyword && !haveXmp) {
          haveXmp = true;
          setXmpPacket(toText(t->text.data(), t->text.size()));
        } else if ((isRawProfile(t->keyword, "exif") || isRawProfile(t->keyword, "APP1")) && !haveExif) {
          if (auto raw = decodeRawProfile(t->text)) haveExif = readExif(*raw);
        } else if (isRawProfile(t->keyword, "xmp") && !haveXmp) {
          if (auto raw = decodeRawProfile(t->text)) {
            haveXmp = true;
            setXmpPacket(toText(raw->data(), raw->size()));
          }
        } else if (isRawProfile(t->keyword, "iptc") && !haveIptc) {
          if (auto raw = decodeRawProfile(t->text)) haveIptc = readIptc(*raw);
        }
      }
    }
  }

  void doSave(OutputSink& sink) const override {
    const InputSource& src = source();
    const auto chunks = scan(src);
    Bytes out(kSignature.begin(), kSignature.end());
    for (const auto& c : chunks) {
      if (c.type == "eXIf") continue;
      if (c.type == "iCCP" && iccChanged_) continue;
      if (c.type == "tEXt" || c.type == "zTXt" || c.type == "iTXt") {
        // Only the keyword is needed to decide.
        const Bytes head = src.readBytes(c.data(), std::min<std::uint32_t>(c.length, 80));
        const auto nul = std::find(head.begin(), head.end(), 0);
        const std::string keyword(head.begin(), nul);
        if (keyword == kXmpKeyword || isRawProfile(keyword, "exif") || isRawProfile(keyword, "APP1") ||
            isRawProfile(keyword, "iptc") || isRawProfile(keyword, "xmp")) {
          continue;
        }
      }
      sink.write(out);
      out.clear();
      sink.copyFrom(src, c.offset, c.total());
      if (c.type == "IHDR") writeMetadataChunks(out);
    }
    sink.write(out);
  }

 private:
  bool readExif(const Bytes& raw) {
    const std::uint8_t* p = raw.data();
    std::size_t n = raw.size();
    stripExifPrefix(p, n);
    try {
      decodeTiff(p, n, exif_);
      return true;
    } catch (const Error&) {
      exif_.clear();
      return false;
    }
  }

  bool readIptc(const Bytes& raw) {
    try {
      if (startsWith(raw, "8BIM")) {
        if (auto iptc = iptcFromImageResources(parseImageResources(raw.data(), raw.size()))) {
          iptc_ = std::move(*iptc);
          return true;
        }
        return false;
      }
      iptc_ = IptcMetadata::decode(raw.data(), raw.size());
      return true;
    } catch (const Error&) {
      iptc_.clear();
      return false;
    }
  }

  void writeMetadataChunks(Bytes& out) const {
    // A new ICC profile: iCCP must come before PLTE and IDAT, as this does.
    if (iccChanged_ && !icc_.empty()) {
      const std::string name = "ICC profile";
      Bytes d(name.begin(), name.end());
      d.push_back(0);
      d.push_back(0);  // deflate
      const auto z = zlibDeflate(icc_.data(), icc_.size());
      if (!z) throw Error(ErrorCode::unsupportedOperation, "cannot compress the ICC profile");
      append(d, *z);
      writeChunk(out, "iCCP", d);
    }

    const Bytes tiff = encodeExif(exif_);
    if (!tiff.empty()) writeChunk(out, "eXIf", tiff);

    if (!xmp_.empty()) {
      Bytes d(kXmpKeyword.begin(), kXmpKeyword.end());
      // NUL, uncompressed, method, empty language tag and translated keyword.
      const std::uint8_t fields[] = {0, 0, 0, 0, 0};
      append(d, fields, sizeof fields);
      append(d, xmp_.serialize());
      writeChunk(out, "iTXt", d);
    }

    if (!iptc_.empty()) {
      std::vector<ImageResource> resources;
      setIptcImageResource(resources, iptc_);
      const Bytes profile = encodeRawProfile("iptc", serializeImageResources(resources));
      const std::string keyword = "Raw profile type iptc";
      Bytes d(keyword.begin(), keyword.end());
      d.push_back(0);
      if (auto z = zlibDeflate(profile.data(), profile.size())) {
        d.push_back(0);  // method
        append(d, *z);
        writeChunk(out, "zTXt", d);
      } else {
        append(d, profile);
        writeChunk(out, "tEXt", d);
      }
    }
  }
};

}  // namespace

std::unique_ptr<ImageFile> newPngFile(std::unique_ptr<InputSource> source) {
  return std::make_unique<PngFile>(std::move(source));
}

}  // namespace lumenlib::detail

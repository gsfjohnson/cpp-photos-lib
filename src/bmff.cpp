// ISO base media file format images, read only:
//
//   HEIF/HEIC, AVIF  Exif and XMP are items in the meta box (item types
//                    "Exif" and "mime" application/rdf+xml), located by iloc;
//                    the primary item's ispe, clap and irot properties give
//                    the size.
//   CR3              moov/uuid(Canon) holds CMT1 (IFD0), CMT2 (Exif IFD),
//                    CMT4 (GPS IFD) as TIFF structures; XMP is a top-level
//                    uuid box.
//   JPEG XL          "Exif" and "xml " boxes. Brotli-compressed ("brob")
//                    boxes are skipped.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace lumenlib::detail {
namespace {

constexpr std::size_t kMaxBoxes = 100000;
constexpr std::uint64_t kMaxItem = 64u << 20;

const std::uint8_t kCanonUuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                     0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
const std::uint8_t kXmpUuid[16] = {0xbe, 0x7a, 0xcf, 0xcb, 0x97, 0xa9, 0x42, 0xe8,
                                   0x9c, 0x71, 0x99, 0x94, 0x91, 0xe3, 0xaf, 0xac};

struct Box {
  std::string type;
  std::uint64_t offset;   // of the header
  std::uint64_t payload;  // start of the payload (after any uuid)
  std::uint64_t end;
  std::uint8_t uuid[16] = {};
  std::uint64_t size() const { return end - payload; }
};

// The boxes in [begin, end).
std::vector<Box> boxes(const InputSource& src, std::uint64_t begin, std::uint64_t end, std::size_t& budget) {
  std::vector<Box> out;
  std::uint64_t pos = begin;
  while (inBounds(end, pos, 8)) {
    if (budget-- == 0) corrupt("too many boxes");
    std::uint8_t h[16];
    src.read(pos, h, 8);
    std::uint64_t size = getBe32(h);
    Box b;
    b.type = toText(h + 4, 4);
    b.offset = pos;
    std::uint64_t header = 8;
    if (size == 1) {
      if (!inBounds(end, pos, 16)) corrupt("truncated box header");
      src.read(pos + 8, h + 8, 8);
      size = getBe64(h + 8);
      header = 16;
    } else if (size == 0) {
      size = end - pos;
    }
    if (size < header || !inBounds(end, pos, size)) corrupt("box '" + b.type + "' runs past its parent");
    if (b.type == "uuid") {
      if (size < header + 16) corrupt("truncated uuid box");
      src.read(pos + header, b.uuid, 16);
      header += 16;
    }
    b.payload = pos + header;
    b.end = pos + size;
    out.push_back(b);
    pos += size;
  }
  return out;
}

// Big-endian reads within a box payload that is in memory.
class Reader {
 public:
  explicit Reader(Bytes data) : d_(std::move(data)) {}
  bool has(std::size_t n) const { return inBounds(d_.size(), pos_, n); }
  std::uint64_t read(std::size_t n) {
    if (n > 8 || !has(n)) corrupt("truncated or malformed box");
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | d_[pos_ + i];
    pos_ += n;
    return v;
  }
  std::string cstring() {
    const auto begin = d_.begin() + static_cast<std::ptrdiff_t>(pos_);
    const auto nul = std::find(begin, d_.end(), 0);
    std::string s(begin, nul);
    pos_ = static_cast<std::size_t>(nul - d_.begin()) + (nul == d_.end() ? 0 : 1);
    return s;
  }
  std::string fourcc() {
    if (!has(4)) corrupt("truncated box");
    std::string s = toText(d_.data() + pos_, 4);
    pos_ += 4;
    return s;
  }
  std::size_t pos() const { return pos_; }
  const Bytes& data() const { return d_; }

 private:
  Bytes d_;
  std::size_t pos_ = 0;
};

struct Item {
  std::string type;
  std::string contentType;
  int constructionMethod = 0;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> extents;  // offset, length
};

struct Property {
  enum class Kind { size, crop, rotation } kind;
  std::uint32_t width, height;
  unsigned rotation;  // quarter turns
};

class BmffFile final : public ImageFile {
 public:
  BmffFile(FileFormat format, std::unique_ptr<InputSource> source) : ImageFile(format, std::move(source)) {}

 protected:
  void doLoad() override {
    const InputSource& src = source();
    std::size_t budget = kMaxBoxes;
    std::uint64_t begin = 0;
    // JPEG XL's signature box comes before ftyp.
    for (const auto& b : boxes(src, begin, src.size(), budget)) {
      if (b.type == "meta") {
        readMeta(b, budget);
      } else if (b.type == "moov") {
        readMoov(b, budget);
      } else if (b.type == "uuid" && std::memcmp(b.uuid, kXmpUuid, 16) == 0 && xmpPacket_.empty()) {
        setXmpPacket(readText(b));
      } else if (b.type == "Exif" && exif_.empty()) {
        readExifItem(src.readBytes(b.payload, checkedSize(b)));
      } else if (b.type == "xml " && xmpPacket_.empty()) {
        setXmpPacket(readText(b));
      }
    }
  }

 private:
  std::size_t checkedSize(const Box& b) const {
    if (b.size() > kMaxItem) throw Error(ErrorCode::dataTooLarge, "'" + b.type + "' box too large");
    return static_cast<std::size_t>(b.size());
  }

  std::string readText(const Box& b) const {
    const Bytes d = source().readBytes(b.payload, checkedSize(b));
    return toText(d.data(), d.size());
  }

  // HEIF and JPEG XL Exif: a 4-byte offset to the TIFF header, then the data.
  void readExifItem(const Bytes& d) {
    if (d.size() < 4) return;
    std::size_t at = 4 + getBe32(d.data());
    if (at > d.size()) at = 4;
    const std::uint8_t* p = d.data() + at;
    std::size_t n = d.size() - at;
    stripExifPrefix(p, n);
    if (!isTiffHeader(p, n)) {
      // Some writers get the offset wrong; look for the header.
      p = nullptr;
      for (std::size_t i = 4; i + 8 <= d.size(); ++i) {
        if (isTiffHeader(d.data() + i, d.size() - i)) {
          p = d.data() + i;
          n = d.size() - i;
          break;
        }
      }
      if (!p) return;
    }
    try {
      decodeTiff(p, n, exif_);
    } catch (const Error&) {
      exif_.clear();
    }
  }

  void readMeta(const Box& meta, std::size_t& budget) {
    const InputSource& src = source();
    // meta is a full box: 4 bytes of version and flags first.
    std::map<std::uint32_t, Item> items;
    std::uint32_t primary = 0;
    Bytes idat;
    std::map<std::uint32_t, Property> properties;                      // by 1-based index
    std::map<std::uint32_t, std::vector<std::uint32_t>> associations;  // item -> property indexes
    for (const auto& b : boxes(src, meta.payload + 4, meta.end, budget)) {
      if (b.type == "pitm") {
        Reader r(src.readBytes(b.payload, checkedSize(b)));
        const auto version = r.read(1);
        r.read(3);
        primary = static_cast<std::uint32_t>(r.read(version == 0 ? 2 : 4));
      } else if (b.type == "iinf") {
        readIinf(b, items, budget);
      } else if (b.type == "iloc") {
        readIloc(src.readBytes(b.payload, checkedSize(b)), items);
      } else if (b.type == "idat") {
        idat = src.readBytes(b.payload, checkedSize(b));
      } else if (b.type == "iprp") {
        readIprp(b, properties, associations, budget);
      }
    }
    // The primary item's size as decoded: its ispe, then any crop and
    // rotation, in the order they are associated.
    if (const auto it = associations.find(primary); it != associations.end()) {
      for (auto index : it->second) {
        const auto p = properties.find(index);
        if (p == properties.end()) continue;
        switch (p->second.kind) {
          case Property::Kind::size:
            width_ = p->second.width;
            height_ = p->second.height;
            break;
          case Property::Kind::crop:
            if (p->second.width && p->second.height) {
              width_ = p->second.width;
              height_ = p->second.height;
            }
            break;
          case Property::Kind::rotation:
            if (p->second.rotation & 1) std::swap(width_, height_);
            break;
        }
      }
    }
    for (const auto& [id, item] : items) {
      const bool isExif = item.type == "Exif";
      const bool isXmp = item.type == "mime" && item.contentType == "application/rdf+xml";
      if ((!isExif || !exif_.empty()) && (!isXmp || !xmpPacket_.empty())) continue;
      Bytes data;
      for (const auto& [offset, length] : item.extents) {
        if (length > kMaxItem || data.size() + length > kMaxItem) {
          throw Error(ErrorCode::dataTooLarge, "metadata item too large");
        }
        if (item.constructionMethod == 1) {
          if (!inBounds(idat.size(), offset, length)) corrupt("item extent outside idat");
          append(data, idat.data() + offset, static_cast<std::size_t>(length));
        } else if (item.constructionMethod == 0) {
          append(data, src.readBytes(offset, static_cast<std::size_t>(length)));
        }
      }
      if (isExif) {
        readExifItem(data);
      } else {
        setXmpPacket(toText(data.data(), data.size()));
      }
    }
  }

  void readIinf(const Box& iinf, std::map<std::uint32_t, Item>& items, std::size_t& budget) {
    const InputSource& src = source();
    Reader head(src.readBytes(iinf.payload, std::min<std::size_t>(checkedSize(iinf), 8)));
    const auto version = head.read(1);
    head.read(3);
    head.read(version == 0 ? 2 : 4);  // entry count
    const std::uint64_t first = iinf.payload + 4 + (version == 0 ? 2 : 4);
    for (const auto& b : boxes(src, first, iinf.end, budget)) {
      if (b.type != "infe") continue;
      Reader r(src.readBytes(b.payload, checkedSize(b)));
      const auto v = r.read(1);
      r.read(3);
      if (v < 2) continue;  // no item types before version 2
      const auto id = static_cast<std::uint32_t>(r.read(v == 2 ? 2 : 4));
      r.read(2);  // protection index
      Item& item = items[id];
      item.type = r.fourcc();
      r.cstring();  // name
      if (item.type == "mime") item.contentType = r.cstring();
    }
  }

  static void readIloc(Bytes data, std::map<std::uint32_t, Item>& items) {
    Reader r(std::move(data));
    const auto version = r.read(1);
    r.read(3);
    const auto sizes = static_cast<std::size_t>(r.read(1));
    const std::size_t offsetSize = sizes >> 4, lengthSize = sizes & 0xf;
    const auto sizes2 = static_cast<std::size_t>(r.read(1));
    const std::size_t baseOffsetSize = sizes2 >> 4;
    const std::size_t indexSize = (version == 1 || version == 2) ? (sizes2 & 0xf) : 0;
    const auto count = r.read(version < 2 ? 2 : 4);
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto id = static_cast<std::uint32_t>(r.read(version < 2 ? 2 : 4));
      int method = 0;
      if (version == 1 || version == 2) method = static_cast<int>(r.read(2) & 0xf);
      r.read(2);  // data reference index
      const std::uint64_t base = r.read(baseOffsetSize);
      const auto extents = r.read(2);
      Item& item = items[id];
      item.constructionMethod = method;
      item.extents.clear();
      for (std::uint64_t e = 0; e < extents; ++e) {
        r.read(indexSize);
        const std::uint64_t offset = r.read(offsetSize);
        const std::uint64_t length = r.read(lengthSize);
        if (offset > UINT64_MAX - base) corrupt("bad iloc extent");
        item.extents.emplace_back(base + offset, length);
      }
    }
  }

  void readIprp(const Box& iprp, std::map<std::uint32_t, Property>& properties,
                std::map<std::uint32_t, std::vector<std::uint32_t>>& associations, std::size_t& budget) {
    const InputSource& src = source();
    for (const auto& b : boxes(src, iprp.payload, iprp.end, budget)) {
      if (b.type == "ipco") {
        std::uint32_t index = 0;
        for (const auto& p : boxes(src, b.payload, b.end, budget)) {
          ++index;
          if (p.type == "ispe" && p.size() >= 12) {
            const Bytes d = src.readBytes(p.payload, 12);
            properties[index] = {Property::Kind::size, getBe32(d.data() + 4), getBe32(d.data() + 8), 0};
          } else if (p.type == "clap" && p.size() >= 32) {
            // Clean aperture: width and height as fractions.
            const Bytes d = src.readBytes(p.payload, 16);
            const std::uint32_t wd = getBe32(d.data() + 4), hd = getBe32(d.data() + 12);
            if (wd && hd) {
              properties[index] = {Property::Kind::crop, getBe32(d.data()) / wd, getBe32(d.data() + 8) / hd, 0};
            }
          } else if (p.type == "irot" && p.size() >= 1) {
            const Bytes d = src.readBytes(p.payload, 1);
            properties[index] = {Property::Kind::rotation, 0, 0, d[0] & 3u};
          }
        }
      } else if (b.type == "ipma") {
        Reader r(src.readBytes(b.payload, checkedSize(b)));
        const auto version = r.read(1);
        const auto flags = r.read(3);
        const auto count = r.read(4);
        for (std::uint64_t i = 0; i < count; ++i) {
          const auto id = static_cast<std::uint32_t>(r.read(version < 1 ? 2 : 4));
          const auto n = r.read(1);
          auto& list = associations[id];
          for (std::uint64_t k = 0; k < n; ++k) {
            const auto v = (flags & 1) ? r.read(2) & 0x7fff : r.read(1) & 0x7f;
            list.push_back(static_cast<std::uint32_t>(v));
          }
        }
      }
    }
  }

  void readMoov(const Box& moov, std::size_t& budget) {
    const InputSource& src = source();
    for (const auto& b : boxes(src, moov.payload, moov.end, budget)) {
      if (b.type != "uuid" || std::memcmp(b.uuid, kCanonUuid, 16) != 0) continue;
      for (const auto& c : boxes(src, b.payload, b.end, budget)) {
        Ifd root;
        if (c.type == "CMT1")
          root = Ifd::ifd0;
        else if (c.type == "CMT2")
          root = Ifd::exif;
        else if (c.type == "CMT4")
          root = Ifd::gps;
        else if (c.type == "CMT3") {
          readCanonMakerNote(c);
          continue;
        } else
          continue;
        const Bytes d = src.readBytes(c.payload, checkedSize(c));
        TiffDecodeOptions options;
        options.root = root;
        options.keepOrigin = false;
        try {
          decodeTiff(d.data(), d.size(), exif_, options);
        } catch (const Error& e) {
          // A damaged block loses its own tags only.
          warn("CR3 " + c.type + " not read: " + e.what());
        }
      }
    }
  }

  // CMT3 is Canon's maker note as a TIFF structure of its own.
  void readCanonMakerNote(const Box& box) {
    const Bytes d = source().readBytes(box.payload, checkedSize(box));
    if (!isTiffHeader(d.data(), d.size())) return;
    const ByteOrder order = d[0] == 'I' ? ByteOrder::little : ByteOrder::big;
    const MakerNoteLayout layout{MakerNoteFormat::canon, order, get32(d.data() + 4, order), MakerNoteBase::own, 0};
    if (layout.ifd >= d.size()) return;
    ExifAccess::makerNote(exif_) = decodeMakerNote(SpanSource(d.data(), d.size()), 0, d.size(), layout);
  }
};

}  // namespace

std::unique_ptr<ImageFile> newBmffFile(FileFormat format, std::unique_ptr<InputSource> source) {
  return std::make_unique<BmffFile>(format, std::move(source));
}

}  // namespace lumenlib::detail

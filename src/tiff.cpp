// TIFF structure decoding and encoding for Exif blocks.
#include <photos/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace photos::detail {
namespace {

constexpr std::uint16_t kExifIfdPointer = 0x8769;
constexpr std::uint16_t kGpsIfdPointer = 0x8825;
constexpr std::uint16_t kInteropIfdPointer = 0xa005;
constexpr std::uint16_t kThumbnailOffset = 0x0201;
constexpr std::uint16_t kThumbnailLength = 0x0202;

// Deeper nesting than Exif's (IFD0 -> Exif -> Interop) means a damaged file.
constexpr int kMaxDepth = 4;
constexpr std::uint32_t kMaxEntries = 4096;

// Values larger than this are skipped rather than read (raw files hold
// multi-megabyte strips; metadata never is).
constexpr std::uint64_t kMaxValueSize = 64u << 20;

class Decoder {
 public:
  Decoder(const InputSource& source, ByteOrder order, ExifData& exif, ExifOrigin* origin)
      : source_(source), size_(source.size()), order_(order), exif_(exif), origin_(origin) {}

  void parseIfd(std::uint64_t offset, IfdId ifd, int depth, bool followNext) {
    if (depth > kMaxDepth || !visited_.insert(offset).second) return;
    if (!inBounds(size_, offset, 2)) return;
    std::uint8_t countBytes[2];
    source_.read(offset, countBytes, 2);
    const std::uint32_t count = get16(countBytes, order_);
    if (count == 0 || count > kMaxEntries) return;
    // Truncated IFDs keep the entries that are there.
    const std::uint64_t available = (size_ - offset - 2) / 12;
    const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, available));
    const Bytes entries = source_.readBytes(offset + 2, 12 * static_cast<std::size_t>(n));

    std::uint32_t thumbOffset = 0, thumbLength = 0;
    std::vector<std::pair<std::uint64_t, IfdId>> subIfds;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint8_t* e = entries.data() + 12 * static_cast<std::size_t>(i);
      const std::uint64_t entryOffset = offset + 2 + 12 * static_cast<std::uint64_t>(i);
      const std::uint16_t tag = get16(e, order_);
      const std::uint16_t type = get16(e + 2, order_);
      const std::uint32_t components = get32(e + 4, order_);
      if (!isValidType(type)) continue;
      const auto typeId = static_cast<TypeId>(type);
      const std::uint64_t byteCount = static_cast<std::uint64_t>(components) * typeSize(typeId);
      const bool inlineValue = byteCount <= 4;
      const std::uint64_t valueOffset = inlineValue ? entryOffset + 8 : get32(e + 8, order_);
      if (!inBounds(size_, valueOffset, byteCount) || byteCount > kMaxValueSize) continue;
      Bytes raw = inlineValue ? Bytes(e + 8, e + 8 + byteCount)
                              : source_.readBytes(valueOffset, static_cast<std::size_t>(byteCount));

      if (isPointer(ifd, tag)) {
        if (components >= 1 && (typeId == TypeId::unsignedLong || typeId == TypeId::tiffIfd)) {
          subIfds.emplace_back(get32(raw.data(), order_), pointerTarget(tag));
        }
        continue;
      }
      if (ifd == IfdId::ifd1 && (tag == kThumbnailOffset || tag == kThumbnailLength)) {
        if (components >= 1) {
          const auto v = Value::fromBytes(typeId, raw.data(), raw.size(), 1, order_).toInt64();
          if (v >= 0 && v <= 0xffffffffLL) {
            (tag == kThumbnailOffset ? thumbOffset : thumbLength) = static_cast<std::uint32_t>(v);
          }
        }
        continue;
      }

      Value value = Value::fromBytes(typeId, raw.data(), raw.size(), components, order_);
      if (origin_) {
        origin_->entries.push_back({ifd, tag, value, static_cast<std::size_t>(entryOffset),
                                    static_cast<std::size_t>(valueOffset),
                                    inlineValue ? 4 : static_cast<std::size_t>(byteCount)});
      }
      exif_.add(ExifKey(ifd, tag), std::move(value));
    }

    if (ifd == IfdId::ifd1 && thumbLength > 0 && inBounds(size_, thumbOffset, thumbLength) &&
        thumbLength <= kMaxValueSize) {
      ExifAccess::thumbnail(exif_) = source_.readBytes(thumbOffset, thumbLength);
    }

    // Sub-IFDs are parsed after this IFD's entries so the groups come out in
    // file order.
    for (const auto& sub : subIfds) parseIfd(sub.first, sub.second, depth + 1, false);

    if (followNext && count == n) {
      const std::uint64_t nextPos = offset + 2 + 12 * static_cast<std::uint64_t>(n);
      if (inBounds(size_, nextPos, 4)) {
        std::uint8_t b[4];
        source_.read(nextPos, b, 4);
        const std::uint32_t next = get32(b, order_);
        if (next != 0) parseIfd(next, IfdId::ifd1, depth + 1, false);
      }
    }
  }

 private:
  static bool isPointer(IfdId ifd, std::uint16_t tag) {
    return (ifd == IfdId::ifd0 && (tag == kExifIfdPointer || tag == kGpsIfdPointer)) ||
           (ifd == IfdId::exif && tag == kInteropIfdPointer);
  }
  static IfdId pointerTarget(std::uint16_t tag) {
    switch (tag) {
      case kExifIfdPointer:
        return IfdId::exif;
      case kGpsIfdPointer:
        return IfdId::gps;
      default:
        return IfdId::interop;
    }
  }

  const InputSource& source_;
  std::uint64_t size_;
  ByteOrder order_;
  ExifData& exif_;
  ExifOrigin* origin_;
  std::set<std::uint64_t> visited_;
};

// ---- Encoding -----------------------------------------------------------------

struct Entry {
  std::uint16_t tag;
  TypeId type;
  std::uint32_t count;
  Bytes data;
};

// Offsets of pointers and the thumbnail are filled in once the layout is known.
struct Ifd {
  IfdId id;
  std::vector<Entry> entries;

  std::size_t size() const {
    std::size_t s = 2 + 12 * entries.size() + 4;
    for (const auto& e : entries) {
      if (e.data.size() > 4) s += e.data.size() + (e.data.size() & 1);
    }
    return s;
  }
};

Entry longEntry(std::uint16_t tag, std::uint32_t v, ByteOrder order) {
  Entry e{tag, TypeId::unsignedLong, 1, {}};
  append32(e.data, v, order);
  return e;
}

void setLong(Ifd& ifd, std::uint16_t tag, std::uint32_t v, ByteOrder order) {
  for (auto& e : ifd.entries) {
    if (e.tag == tag) {
      e.data.clear();
      append32(e.data, v, order);
      return;
    }
  }
}

void sortEntries(Ifd& ifd) {
  std::stable_sort(ifd.entries.begin(), ifd.entries.end(),
                   [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
}

// Writes the IFD at `offset` (its position in the output), with `next`.
void writeIfd(Bytes& out, const Ifd& ifd, std::uint32_t offset, std::uint32_t next, ByteOrder order) {
  if (out.size() != offset) throw Error(ErrorCode::invalidArgument, "TIFF layout mismatch");
  append16(out, static_cast<std::uint16_t>(ifd.entries.size()), order);
  std::uint32_t dataOffset = offset + static_cast<std::uint32_t>(2 + 12 * ifd.entries.size() + 4);
  Bytes dataArea;
  for (const auto& e : ifd.entries) {
    append16(out, e.tag, order);
    append16(out, static_cast<std::uint16_t>(e.type), order);
    append32(out, e.count, order);
    if (e.data.size() <= 4) {
      Bytes v = e.data;
      v.resize(4, 0);
      append(out, v);
    } else {
      append32(out, dataOffset + static_cast<std::uint32_t>(dataArea.size()), order);
      append(dataArea, e.data);
      if (dataArea.size() & 1) dataArea.push_back(0);
    }
  }
  append32(out, next, order);
  append(out, dataArea);
}

Bytes encodeFull(const ExifData& exif) {
  const ByteOrder order = exif.byteOrder();
  std::array<Ifd, 5> ifds{Ifd{IfdId::ifd0, {}}, Ifd{IfdId::exif, {}}, Ifd{IfdId::interop, {}}, Ifd{IfdId::gps, {}},
                          Ifd{IfdId::ifd1, {}}};
  auto slot = [&](IfdId id) -> Ifd& {
    for (auto& i : ifds) {
      if (i.id == id) return i;
    }
    return ifds[0];
  };
  for (const auto& d : exif) {
    // Structural tags come from the layout, whatever the data say.
    const bool structural = (d.ifd() == IfdId::ifd0 && (d.tag() == kExifIfdPointer || d.tag() == kGpsIfdPointer)) ||
                            (d.ifd() == IfdId::exif && d.tag() == kInteropIfdPointer) ||
                            (d.ifd() == IfdId::ifd1 && (d.tag() == kThumbnailOffset || d.tag() == kThumbnailLength));
    if (d.value().empty() || structural) continue;
    const auto& v = d.value();
    if (!fits32(v.sizeInBytes())) {
      throw Error(ErrorCode::dataTooLarge, d.key() + " is too large");
    }
    slot(d.ifd()).entries.push_back({d.tag(), v.typeId(), static_cast<std::uint32_t>(v.count()), v.toBytes(order)});
  }
  const Bytes& thumb = exif.thumbnail();
  Ifd& ifd0 = slot(IfdId::ifd0);
  Ifd& exifIfd = slot(IfdId::exif);
  Ifd& interop = slot(IfdId::interop);
  Ifd& gps = slot(IfdId::gps);
  Ifd& ifd1 = slot(IfdId::ifd1);

  if (!interop.entries.empty()) exifIfd.entries.push_back(longEntry(kInteropIfdPointer, 0, order));
  if (!exifIfd.entries.empty()) ifd0.entries.push_back(longEntry(kExifIfdPointer, 0, order));
  if (!gps.entries.empty()) ifd0.entries.push_back(longEntry(kGpsIfdPointer, 0, order));
  if (!thumb.empty()) {
    ifd1.entries.push_back(longEntry(kThumbnailOffset, 0, order));
    ifd1.entries.push_back(longEntry(kThumbnailLength, static_cast<std::uint32_t>(thumb.size()), order));
  }
  const bool hasIfd1 = !ifd1.entries.empty();
  if (ifd0.entries.empty() && !hasIfd1) return {};
  for (auto& i : ifds) sortEntries(i);

  // Layout: header, IFD0, Exif, Interop, GPS, IFD1, thumbnail.
  std::map<IfdId, std::uint32_t> at;
  std::uint64_t pos = 8;
  for (auto& i : ifds) {
    if (i.entries.empty() && i.id != IfdId::ifd0) continue;
    at[i.id] = static_cast<std::uint32_t>(pos);
    pos += i.size();
  }
  const std::uint64_t thumbAt = pos;
  pos += thumb.size();
  if (pos > 0xffffffffULL) throw Error(ErrorCode::dataTooLarge, "Exif data exceeds 4 GB");

  if (at.count(IfdId::exif)) setLong(ifd0, kExifIfdPointer, at[IfdId::exif], order);
  if (at.count(IfdId::gps)) setLong(ifd0, kGpsIfdPointer, at[IfdId::gps], order);
  if (at.count(IfdId::interop)) setLong(exifIfd, kInteropIfdPointer, at[IfdId::interop], order);
  if (!thumb.empty()) setLong(ifd1, kThumbnailOffset, static_cast<std::uint32_t>(thumbAt), order);

  Bytes out;
  out.reserve(static_cast<std::size_t>(pos));
  if (order == ByteOrder::littleEndian) {
    append(out, std::string_view("II*\0", 4));
  } else {
    append(out, std::string_view("MM\0*", 4));
  }
  append32(out, 8, order);
  for (auto& i : ifds) {
    if (!at.count(i.id)) continue;
    const std::uint32_t next = (i.id == IfdId::ifd0 && hasIfd1) ? at[IfdId::ifd1] : 0;
    writeIfd(out, i, at[i.id], next, order);
  }
  append(out, thumb);
  return out;
}

// The original block with changed values written over the old ones, when
// every change fits where the old value was. std::nullopt otherwise.
std::optional<Bytes> encodeInPlace(const ExifData& exif, const ExifOrigin& origin) {
  if (!origin.patchable || exif.byteOrder() != origin.order || exif.thumbnail() != origin.thumbnail) {
    return std::nullopt;
  }
  std::map<std::pair<IfdId, std::uint16_t>, const OriginEntry*> byKey;
  for (const auto& e : origin.entries) byKey[{e.ifd, e.tag}] = &e;
  if (byKey.size() != origin.entries.size() || exif.size() != origin.entries.size()) return std::nullopt;

  Bytes out = origin.tiff;
  std::set<std::pair<IfdId, std::uint16_t>> seen;
  for (const auto& d : exif) {
    const auto key = std::make_pair(d.ifd(), d.tag());
    const auto it = byKey.find(key);
    if (it == byKey.end() || !seen.insert(key).second) return std::nullopt;
    const OriginEntry& e = *it->second;
    if (d.value() == e.value) continue;
    Value v = d.value();
    if (v.empty() || v.typeId() != e.value.typeId() || v.sizeInBytes() > e.capacity) return std::nullopt;
    const bool wasInline = e.value.sizeInBytes() <= 4;
    if (!wasInline && v.sizeInBytes() <= 4) {
      // A value of 4 bytes or fewer belongs in the entry. Text can stay where
      // it is, padded with NULs past the 4 bytes; anything else cannot.
      if (v.typeId() != TypeId::asciiString) return std::nullopt;
      Bytes padded = v.rawBytes();
      padded.resize(5, 0);
      v = Value::bytes(TypeId::asciiString, std::move(padded));
    }
    const Bytes data = v.toBytes(origin.order);
    put32(out.data() + e.entryOffset + 4, static_cast<std::uint32_t>(v.count()), origin.order);
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(e.valueOffset),
              out.begin() + static_cast<std::ptrdiff_t>(e.valueOffset + e.capacity), 0);
    std::copy(data.begin(), data.end(), out.begin() + static_cast<std::ptrdiff_t>(e.valueOffset));
  }
  return out;
}

}  // namespace

bool isTiffHeader(const std::uint8_t* data, std::size_t size, bool allowRawMagic) noexcept {
  if (size < 8) return false;
  if (data[0] == 'I' && data[1] == 'I') {
    const std::uint16_t magic = getLe16(data + 2);
    return magic == 42 || (allowRawMagic && (magic == 0x4f52 || magic == 0x5352 || magic == 0x55));
  }
  if (data[0] == 'M' && data[1] == 'M') {
    const std::uint16_t magic = getBe16(data + 2);
    return magic == 42 || (allowRawMagic && magic == 0x4f52);
  }
  return false;
}

void decodeTiff(const InputSource& source, ExifData& exif, const TiffDecodeOptions& options) {
  std::uint8_t header[8] = {};
  const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), 8));
  source.read(0, header, n);
  if (!isTiffHeader(header, n, options.allowRawMagic)) corrupt("no TIFF header");
  const ByteOrder order = header[0] == 'I' ? ByteOrder::littleEndian : ByteOrder::bigEndian;
  exif.setByteOrder(order);

  // Only a block that is all the Exif data, and small enough to keep, can be
  // patched in place.
  std::shared_ptr<ExifOrigin> origin;
  if (options.keepOrigin && options.root == IfdId::ifd0 && exif.size() == 0 && source.size() <= kMaxValueSize) {
    origin = std::make_shared<ExifOrigin>();
    origin->order = order;
  }
  Decoder decoder(source, order, exif, origin.get());
  decoder.parseIfd(get32(header + 4, order), options.root, 0, options.root == IfdId::ifd0);

  if (origin) {
    origin->tiff = source.readBytes(0, static_cast<std::size_t>(source.size()));
    origin->thumbnail = exif.thumbnail();
    ExifAccess::origin(exif) = std::move(origin);
  } else {
    ExifAccess::origin(exif).reset();
  }
}

void decodeTiff(const std::uint8_t* data, std::size_t size, ExifData& exif, const TiffDecodeOptions& options) {
  decodeTiff(SpanSource(data, size), exif, options);
}

Bytes encodeExif(const ExifData& exif) {
  if (const auto& origin = ExifAccess::origin(exif)) {
    if (auto patched = encodeInPlace(exif, *origin)) return std::move(*patched);
  }
  return encodeFull(exif);
}

}  // namespace photos::detail

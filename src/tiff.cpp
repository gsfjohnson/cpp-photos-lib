// TIFF structure decoding and encoding for Exif blocks.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace lumenlib::detail {
namespace {

// Deeper nesting than Exif's (IFD0 -> Exif -> Interop) means a damaged file.
constexpr int kMaxDepth = 4;
constexpr std::uint32_t kMaxEntries = 4096;

bool isPointer(Ifd ifd, std::uint16_t tag) {
  return (ifd == Ifd::ifd0 && (tag == kExifIfdPointer || tag == kGpsIfdPointer)) ||
         (ifd == Ifd::exif && tag == kInteropIfdPointer);
}

Ifd pointerTarget(std::uint16_t tag) {
  switch (tag) {
    case kExifIfdPointer:
      return Ifd::exif;
    case kGpsIfdPointer:
      return Ifd::gps;
    default:
      return Ifd::interop;
  }
}

class Decoder {
 public:
  Decoder(const InputSource& source, ByteOrder order, ExifMetadata& exif, ExifOrigin* origin,
          const TiffDecodeOptions& options)
      : source_(source), size_(source.size()), order_(order), exif_(exif), origin_(origin), options_(options) {}

  void parseIfd(std::uint64_t offset, Ifd ifd, int depth, bool followNext) {
    if (depth > kMaxDepth || !visited_.insert(offset).second) return;
    if (!inBounds(size_, offset, 2)) {
      ++skipped_;
      return;
    }
    std::uint8_t countBytes[2];
    source_.read(offset, countBytes, 2);
    const std::uint32_t count = get16(countBytes, order_);
    if (count == 0 || count > kMaxEntries) return;
    // Truncated IFDs keep the entries that are there.
    const std::uint64_t available = (size_ - offset - 2) / 12;
    const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, available));
    if (n < count) ++skipped_;
    const Bytes entries = source_.readBytes(offset + 2, 12 * static_cast<std::size_t>(n));

    std::uint32_t thumbOffset = 0, thumbLength = 0;
    std::vector<std::pair<std::uint64_t, Ifd>> subIfds;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint8_t* e = entries.data() + 12 * static_cast<std::size_t>(i);
      const std::uint64_t entryOffset = offset + 2 + 12 * static_cast<std::uint64_t>(i);
      const std::uint16_t tag = get16(e, order_);
      const std::uint16_t type = get16(e + 2, order_);
      const std::uint32_t components = get32(e + 4, order_);
      if (!isFieldType(type)) {
        ++skipped_;
        continue;
      }
      const auto fieldType = static_cast<FieldType>(type);
      const std::uint64_t byteCount = static_cast<std::uint64_t>(components) * fieldTypeSize(fieldType);
      const bool inlineValue = byteCount <= 4;
      const std::uint64_t valueOffset = inlineValue ? entryOffset + 8 : get32(e + 8, order_);
      if (!inBounds(size_, valueOffset, byteCount) || byteCount > options_.maxValueSize) {
        ++skipped_;
        continue;
      }
      Bytes raw = inlineValue ? Bytes(e + 8, e + 8 + byteCount)
                              : source_.readBytes(valueOffset, static_cast<std::size_t>(byteCount));

      if (isPointer(ifd, tag)) {
        if (components >= 1 && (fieldType == FieldType::u32 || fieldType == FieldType::ifd)) {
          subIfds.emplace_back(get32(raw.data(), order_), pointerTarget(tag));
        }
        continue;
      }
      if (ifd == Ifd::ifd1 && (tag == kThumbnailOffset || tag == kThumbnailLength)) {
        if (components >= 1) {
          const auto v = FieldValue::decode(fieldType, raw.data(), raw.size(), 1, order_).asInt();
          if (v >= 0 && v <= 0xffffffffLL) {
            (tag == kThumbnailOffset ? thumbOffset : thumbLength) = static_cast<std::uint32_t>(v);
          }
        }
        continue;
      }
      if (ifd == Ifd::exif && tag == kMakerNote && !inlineValue) {
        makerNoteOffset_ = valueOffset;
        makerNoteSize_ = static_cast<std::size_t>(byteCount);
      }

      FieldValue value = FieldValue::decode(fieldType, raw.data(), raw.size(), components, order_);
      if (origin_) {
        origin_->entries.push_back({ifd, tag, value, static_cast<std::size_t>(entryOffset),
                                    static_cast<std::size_t>(valueOffset),
                                    inlineValue ? 4 : static_cast<std::size_t>(byteCount)});
      }
      exif_.append(ExifTag(ifd, tag), std::move(value));
    }

    if (ifd == Ifd::ifd1 && thumbLength > 0 && inBounds(size_, thumbOffset, thumbLength) &&
        thumbLength <= options_.maxValueSize) {
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
        if (next != 0) parseIfd(next, Ifd::ifd1, depth + 1, false);
      }
    }
  }

  void decodeMakerNote() {
    if (makerNoteSize_ == 0) return;
    std::string make;
    if (const auto* m = exif_.find(ExifTag(Ifd::ifd0, 0x010f)); m && m->type() == FieldType::ascii) make = m->text();
    const auto layout = identifyMakerNote(source_, makerNoteOffset_, makerNoteSize_, order_, make);
    if (!layout) return;
    ExifAccess::makerNote(exif_) =
        lumenlib::detail::decodeMakerNote(source_, makerNoteOffset_, makerNoteSize_, *layout);
    if (const auto* note = exif_.find(ExifTag(Ifd::exif, kMakerNote))) {
      ExifAccess::makerNoteOrigin(exif_) =
          std::make_shared<const MakerNoteOrigin>(MakerNoteOrigin{*layout, makerNoteOffset_, note->value()});
    }
  }

  std::size_t skipped() const { return skipped_; }

 private:
  const InputSource& source_;
  std::uint64_t size_;
  ByteOrder order_;
  ExifMetadata& exif_;
  ExifOrigin* origin_;
  const TiffDecodeOptions& options_;
  std::set<std::uint64_t> visited_;
  std::size_t skipped_ = 0;
  std::uint64_t makerNoteOffset_ = 0;
  std::size_t makerNoteSize_ = 0;
};

// ---- Encoding -----------------------------------------------------------------

IfdEntry longEntry(std::uint16_t tag, std::uint32_t v, ByteOrder order) {
  IfdEntry e{tag, FieldType::u32, 1, {}};
  append32(e.data, v, order);
  return e;
}

void setLong(IfdLayout& ifd, std::uint16_t tag, std::uint32_t v, ByteOrder order) {
  for (auto& e : ifd.entries) {
    if (e.tag == tag) {
      e.data.clear();
      append32(e.data, v, order);
      return;
    }
  }
}

Bytes encodeFull(const ExifMetadata& exif) {
  const ByteOrder order = exif.byteOrder();
  struct Slot {
    Ifd id;
    IfdLayout layout;
  };
  std::array<Slot, 5> ifds{Slot{Ifd::ifd0, {}}, Slot{Ifd::exif, {}}, Slot{Ifd::interop, {}}, Slot{Ifd::gps, {}},
                           Slot{Ifd::ifd1, {}}};
  auto slot = [&](Ifd id) -> IfdLayout& {
    for (auto& i : ifds) {
      if (i.id == id) return i.layout;
    }
    return ifds[0].layout;
  };
  for (const auto& d : exif) {
    if (d.value().empty() || isStructuralTag(d.ifd(), d.number())) continue;
    const auto& v = d.value();
    if (!fits32(v.byteSize())) throw Error(ErrorCode::dataTooLarge, d.tag().str() + " is too large");
    slot(d.ifd()).entries.push_back({d.number(), v.type(), static_cast<std::uint32_t>(v.count()), v.encode(order)});
  }
  const Bytes& thumb = exif.thumbnail();
  IfdLayout& ifd0 = slot(Ifd::ifd0);
  IfdLayout& exifIfd = slot(Ifd::exif);
  IfdLayout& interop = slot(Ifd::interop);
  IfdLayout& gps = slot(Ifd::gps);
  IfdLayout& ifd1 = slot(Ifd::ifd1);

  if (!interop.entries.empty()) exifIfd.entries.push_back(longEntry(kInteropIfdPointer, 0, order));
  if (!exifIfd.entries.empty()) ifd0.entries.push_back(longEntry(kExifIfdPointer, 0, order));
  if (!gps.entries.empty()) ifd0.entries.push_back(longEntry(kGpsIfdPointer, 0, order));
  if (!thumb.empty()) {
    ifd1.entries.push_back(longEntry(kThumbnailOffset, 0, order));
    ifd1.entries.push_back(longEntry(kThumbnailLength, static_cast<std::uint32_t>(thumb.size()), order));
  }
  const bool hasIfd1 = !ifd1.entries.empty();
  if (ifd0.entries.empty() && !hasIfd1) return {};
  for (auto& i : ifds) i.layout.sort();

  // Layout: header, IFD0, Exif, Interop, GPS, IFD1, thumbnail.
  std::map<Ifd, std::uint32_t> at;
  std::uint64_t pos = 8;
  for (auto& i : ifds) {
    if (i.layout.entries.empty() && i.id != Ifd::ifd0) continue;
    at[i.id] = static_cast<std::uint32_t>(pos);
    pos += i.layout.size();
  }
  const std::uint64_t thumbAt = pos;
  pos += thumb.size();
  if (pos > 0xffffffffULL) throw Error(ErrorCode::dataTooLarge, "Exif data exceeds 4 GB");

  if (at.count(Ifd::exif)) setLong(ifd0, kExifIfdPointer, at[Ifd::exif], order);
  if (at.count(Ifd::gps)) setLong(ifd0, kGpsIfdPointer, at[Ifd::gps], order);
  if (at.count(Ifd::interop)) setLong(exifIfd, kInteropIfdPointer, at[Ifd::interop], order);
  if (!thumb.empty()) setLong(ifd1, kThumbnailOffset, static_cast<std::uint32_t>(thumbAt), order);

  // An unchanged maker note whose offsets count from the TIFF header follows
  // itself to where it now is.
  const auto& noteOrigin = ExifAccess::makerNoteOrigin(exif);
  if (noteOrigin && at.count(Ifd::exif) && noteOrigin->layout.order == order) {
    const auto offsets = exifIfd.valueOffsets(at[Ifd::exif]);
    for (std::size_t i = 0; i < exifIfd.entries.size(); ++i) {
      auto& e = exifIfd.entries[i];
      if (e.tag != kMakerNote || offsets[i] == 0) continue;
      const auto* current = exif.find(ExifTag(Ifd::exif, kMakerNote));
      if (current && current->value() == noteOrigin->value) {
        if (!relocateMakerNote(e.data, noteOrigin->layout, noteOrigin->offset, offsets[i])) {
          warn("the maker note could not be moved; its offsets may be wrong");
        }
      }
    }
  }

  Bytes out;
  out.reserve(static_cast<std::size_t>(pos));
  if (order == ByteOrder::little) {
    append(out, std::string_view("II*\0", 4));
  } else {
    append(out, std::string_view("MM\0*", 4));
  }
  append32(out, 8, order);
  for (auto& i : ifds) {
    if (!at.count(i.id)) continue;
    const std::uint32_t next = (i.id == Ifd::ifd0 && hasIfd1) ? at[Ifd::ifd1] : 0;
    i.layout.write(out, 0, next, order);
  }
  append(out, thumb);
  return out;
}

// The original block with changed values written over the old ones, when
// every change fits where the old value was. std::nullopt otherwise.
std::optional<Bytes> encodeInPlace(const ExifMetadata& exif, const ExifOrigin& origin) {
  if (!origin.patchable || exif.byteOrder() != origin.order || exif.thumbnail() != origin.thumbnail) {
    return std::nullopt;
  }
  std::map<std::pair<Ifd, std::uint16_t>, const OriginEntry*> byKey;
  for (const auto& e : origin.entries) byKey[{e.ifd, e.tag}] = &e;
  if (byKey.size() != origin.entries.size() || exif.size() != origin.entries.size()) return std::nullopt;

  Bytes out = origin.tiff;
  std::set<std::pair<Ifd, std::uint16_t>> seen;
  for (const auto& d : exif) {
    const auto key = std::make_pair(d.ifd(), d.number());
    const auto it = byKey.find(key);
    if (it == byKey.end() || !seen.insert(key).second) return std::nullopt;
    const OriginEntry& e = *it->second;
    if (d.value() == e.value) continue;
    FieldValue v = d.value();
    if (v.empty() || v.type() != e.value.type() || v.byteSize() > e.capacity) return std::nullopt;
    const bool wasInline = e.value.byteSize() <= 4;
    if (!wasInline && v.byteSize() <= 4) {
      // A value of 4 bytes or fewer belongs in the entry. Text can stay where
      // it is, padded with NULs past the 4 bytes; anything else cannot.
      if (v.type() != FieldType::ascii) return std::nullopt;
      Bytes padded = v.bytes();
      padded.resize(5, 0);
      v = FieldValue::fromBytes(FieldType::ascii, std::move(padded));
    }
    const Bytes data = v.encode(origin.order);
    put32(out.data() + e.entryOffset + 4, static_cast<std::uint32_t>(v.count()), origin.order);
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(e.valueOffset),
              out.begin() + static_cast<std::ptrdiff_t>(e.valueOffset + e.capacity), 0);
    std::copy(data.begin(), data.end(), out.begin() + static_cast<std::ptrdiff_t>(e.valueOffset));
  }
  return out;
}

}  // namespace

bool isStructuralTag(Ifd ifd, std::uint16_t tag) noexcept {
  return isPointer(ifd, tag) || (ifd == Ifd::ifd1 && (tag == kThumbnailOffset || tag == kThumbnailLength));
}

// ---- IfdLayout ----------------------------------------------------------------

void IfdLayout::sort() {
  std::stable_sort(entries.begin(), entries.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
}

std::size_t IfdLayout::size() const {
  std::size_t s = 2 + 12 * entries.size() + 4;
  for (const auto& e : entries) {
    if (e.data.size() > 4 && !e.inPlace) s += e.data.size() + (e.data.size() & 1);
  }
  return s;
}

std::vector<std::uint64_t> IfdLayout::valueOffsets(std::uint64_t offset) const {
  std::vector<std::uint64_t> out;
  std::uint64_t at = offset + 2 + 12 * entries.size() + 4;
  for (const auto& e : entries) {
    if (e.data.size() > 4 && !e.inPlace) {
      out.push_back(at);
      at += e.data.size() + (e.data.size() & 1);
    } else {
      out.push_back(0);
    }
  }
  return out;
}

void IfdLayout::write(Bytes& out, std::uint64_t base, std::uint32_t next, ByteOrder order) const {
  const std::uint64_t offset = base + out.size();
  if (offset + size() > 0xffffffffULL) throw Error(ErrorCode::dataTooLarge, "TIFF data exceeds 4 GB");
  const auto offsets = valueOffsets(offset);
  append16(out, static_cast<std::uint16_t>(entries.size()), order);
  Bytes dataArea;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    append16(out, e.tag, order);
    append16(out, static_cast<std::uint16_t>(e.type), order);
    append32(out, e.count, order);
    if (e.inPlace) {
      append(out, e.data);  // the value's offset, as it is in the file
    } else if (e.data.size() <= 4) {
      Bytes v = e.data;
      v.resize(4, 0);
      append(out, v);
    } else {
      append32(out, static_cast<std::uint32_t>(offsets[i]), order);
      append(dataArea, e.data);
      if (dataArea.size() & 1) dataArea.push_back(0);
    }
  }
  append32(out, next, order);
  append(out, dataArea);
}

// ---- Entry points -------------------------------------------------------------

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

void decodeTiff(const InputSource& source, ExifMetadata& exif, const TiffDecodeOptions& options) {
  std::uint8_t header[8] = {};
  const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), 8));
  source.read(0, header, n);
  if (!isTiffHeader(header, n, options.allowRawMagic)) corrupt("no TIFF header");
  const ByteOrder order = header[0] == 'I' ? ByteOrder::little : ByteOrder::big;
  exif.setByteOrder(order);

  // Only a block that is all the Exif data, and small enough to keep, can be
  // patched in place.
  std::shared_ptr<ExifOrigin> origin;
  if (options.keepOrigin && options.root == Ifd::ifd0 && exif.size() == 0 && source.size() <= (64u << 20)) {
    origin = std::make_shared<ExifOrigin>();
    origin->order = order;
  }
  Decoder decoder(source, order, exif, origin.get(), options);
  decoder.parseIfd(get32(header + 4, order), options.root, 0, options.root == Ifd::ifd0 && options.readIfd1);
  decoder.decodeMakerNote();
  if (decoder.skipped()) warn(std::to_string(decoder.skipped()) + " damaged Exif entries or IFDs skipped");

  if (origin) {
    origin->tiff = source.readBytes(0, static_cast<std::size_t>(source.size()));
    origin->thumbnail = exif.thumbnail();
    ExifAccess::origin(exif) = std::move(origin);
  } else {
    ExifAccess::origin(exif).reset();
  }
}

void decodeTiff(const std::uint8_t* data, std::size_t size, ExifMetadata& exif, const TiffDecodeOptions& options) {
  decodeTiff(SpanSource(data, size), exif, options);
}

Bytes encodeExif(const ExifMetadata& exif) {
  if (const auto& origin = ExifAccess::origin(exif)) {
    if (auto patched = encodeInPlace(exif, *origin)) return std::move(*patched);
  }
  return encodeFull(exif);
}

}  // namespace lumenlib::detail

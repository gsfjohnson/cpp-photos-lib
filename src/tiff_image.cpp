// TIFF and TIFF-based raw formats (DNG, CR2, NEF, ARW, ORF, RW2, PEF, SRW,
// ...). IFD0 and its Exif and GPS IFDs are the Exif data; XMP is tag 0x02BC,
// IPTC tag 0x83BB or a Photoshop resource block in tag 0x8649, the ICC
// profile tag 0x8773.
//
// Writing leaves the image alone. The file is copied as it is, up to the
// point where metadata this writer added last time begins; new IFD0, Exif,
// interoperability and GPS IFDs are appended, and the header is pointed at
// the new IFD0. The new IFD0 takes IFD0's descriptive tags from the Exif
// data and keeps every other entry as it was, values where they are (strips,
// tiles, a DNG's colour data, sub-IFDs), and the next-IFD link, so later
// IFDs (thumbnails, pages, raw data) stay reachable. An unchanged maker note
// also stays where it is, so its offsets hold. The old IFDs, and the values
// only they used, are overwritten with zeros: what was removed cannot be read
// back.
#include <lumenlib/error.hpp>

#include "bytes.hpp"
#include "exif_internal.hpp"
#include "formats.hpp"
#include "photoshop.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace lumenlib::detail {
namespace {

constexpr std::uint16_t kXmlPacket = 0x02bc;
constexpr std::uint16_t kIptcNaa = 0x83bb;
constexpr std::uint16_t kImageResources = 0x8649;
constexpr std::uint16_t kIccProfile = 0x8773;
constexpr std::uint16_t kJpgFromRaw = 0x002e;  // Panasonic RW2
constexpr std::uint16_t kStripOffsets = 0x0111, kStripByteCounts = 0x0117;
constexpr std::uint16_t kTileOffsets = 0x0144, kTileByteCounts = 0x0145;
constexpr std::uint16_t kJpegOffset = 0x0201, kJpegLength = 0x0202;
constexpr std::uint32_t kMaxEntries = 4096;
constexpr std::uint64_t kMaxValueSize = 64u << 20;

// IFD0 tags that describe the photo rather than the image data: written from
// the Exif data. Every other IFD0 tag of the file is kept as it is.
bool isDescriptive(std::uint16_t tag) {
  switch (tag) {
    case 0x000b:  // ProcessingSoftware
    case 0x010d:  // DocumentName
    case 0x010e:  // ImageDescription
    case 0x010f:  // Make
    case 0x0110:  // Model
    case 0x0112:  // Orientation
    case 0x011a:  // XResolution
    case 0x011b:  // YResolution
    case 0x011d:  // PageName
    case 0x0128:  // ResolutionUnit
    case 0x0131:  // Software
    case 0x0132:  // DateTime
    case 0x013b:  // Artist
    case 0x013c:  // HostComputer
    case 0x4746:  // Rating
    case 0x4749:  // RatingPercent
    case 0x800d:  // ImageID
    case 0x8298:  // Copyright
    case 0x882a:  // TimeZoneOffset
    case 0x9003:  // DateTimeOriginal
    case 0x9c9b:  // XPTitle
    case 0x9c9c:  // XPComment
    case 0x9c9d:  // XPAuthor
    case 0x9c9e:  // XPKeywords
    case 0x9c9f:  // XPSubject
      return true;
    default:
      return false;
  }
}

// Tags of IFD0 this writer makes itself.
bool isManaged(std::uint16_t tag) {
  return tag == kExifIfdPointer || tag == kGpsIfdPointer || tag == kXmlPacket || tag == kIptcNaa ||
         tag == kImageResources || tag == kIccProfile;
}

struct Range {
  std::uint64_t begin, end;
};

// An IFD entry as it is in the file.
struct RawEntry {
  std::uint16_t tag;
  FieldType type;
  std::uint32_t count;
  std::uint8_t field[4];  // the value, or its offset
  std::uint64_t byteCount;
  std::uint64_t valueOffset;  // where the value is (inside the entry when inline)
  bool isInline() const { return byteCount <= 4; }
};

struct RawIfd {
  std::uint64_t offset = 0;
  std::vector<RawEntry> entries;
  std::uint32_t next = 0;
  Range table() const { return {offset, offset + 2 + 12 * entries.size() + 4}; }
  const RawEntry* find(std::uint16_t tag) const {
    for (const auto& e : entries) {
      if (e.tag == tag) return &e;
    }
    return nullptr;
  }
};

// The IFD at `offset`, keeping the entries whose values are in the file.
std::optional<RawIfd> readRawIfd(const InputSource& src, std::uint64_t offset, ByteOrder order) {
  const std::uint64_t size = src.size();
  if (offset < 8 || (offset & 1) != 0 || !inBounds(size, offset, 2)) return std::nullopt;
  std::uint8_t b[4];
  src.read(offset, b, 2);
  const std::uint32_t n = get16(b, order);
  if (n == 0 || n > kMaxEntries || !inBounds(size, offset + 2, 12ull * n + 4)) return std::nullopt;
  const Bytes table = src.readBytes(offset + 2, 12 * static_cast<std::size_t>(n) + 4);
  RawIfd ifd;
  ifd.offset = offset;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint8_t* e = table.data() + 12 * static_cast<std::size_t>(i);
    RawEntry r{};
    r.tag = get16(e, order);
    const std::uint16_t type = get16(e + 2, order);
    r.count = get32(e + 4, order);
    std::copy(e + 8, e + 12, r.field);
    if (!isFieldType(type)) continue;  // damaged: dropped
    r.type = static_cast<FieldType>(type);
    r.byteCount = std::uint64_t{r.count} * fieldTypeSize(r.type);
    r.valueOffset = r.isInline() ? offset + 2 + 12ull * i + 8 : get32(e + 8, order);
    if (!inBounds(size, r.valueOffset, r.byteCount)) continue;
    ifd.entries.push_back(r);
  }
  ifd.next = get32(table.data() + 12 * static_cast<std::size_t>(n), order);
  // IFDs are entries of their own; one entry per tag is kept, the first.
  std::set<std::uint16_t> seen;
  ifd.entries.erase(std::remove_if(ifd.entries.begin(), ifd.entries.end(),
                                   [&](const RawEntry& r) { return !seen.insert(r.tag).second; }),
                    ifd.entries.end());
  return ifd;
}

// The raw value of an entry in a sub-IFD pointer, or 0.
std::uint32_t pointerOf(const RawIfd& ifd, std::uint16_t tag, ByteOrder order) {
  const auto* e = ifd.find(tag);
  if (!e || e->count < 1 || (e->type != FieldType::u32 && e->type != FieldType::ifd)) return 0;
  return get32(e->field, order);
}

// The entry kept as it is: its value stays where it is in the file.
IfdEntry keptEntry(const RawEntry& r) {
  IfdEntry e{r.tag, r.type, r.count, {}};
  if (r.isInline()) {
    e.data.assign(r.field, r.field + r.byteCount);
  } else {
    e.data.assign(r.field, r.field + 4);
    e.inPlace = true;
  }
  return e;
}

IfdEntry valueEntry(std::uint16_t tag, const FieldValue& v, ByteOrder order) {
  if (!fits32(v.byteSize())) throw Error(ErrorCode::dataTooLarge, "a TIFF value is too large");
  return {tag, v.type(), static_cast<std::uint32_t>(v.count()), v.encode(order)};
}

IfdEntry longPointer(std::uint16_t tag, std::uint32_t v, ByteOrder order) {
  IfdEntry e{tag, FieldType::u32, 1, {}};
  append32(e.data, v, order);
  return e;
}

void setPointer(IfdLayout& ifd, std::uint16_t tag, std::uint64_t v, ByteOrder order) {
  for (auto& e : ifd.entries) {
    if (e.tag == tag) {
      e.data.clear();
      append32(e.data, static_cast<std::uint32_t>(v), order);
    }
  }
}

// The ranges image data occupies, from IFD0's strip, tile and JPEG tags.
void imageDataRanges(const InputSource& src, const RawIfd& ifd0, ByteOrder order, std::vector<Range>& out) {
  const auto array = [&](std::uint16_t tag) {
    std::vector<std::uint64_t> v;
    const auto* e = ifd0.find(tag);
    if (!e || e->byteCount > kMaxValueSize) return v;
    const Bytes raw = src.readBytes(e->valueOffset, static_cast<std::size_t>(e->byteCount));
    const FieldValue value = FieldValue::decode(e->type, raw.data(), raw.size(), e->count, order);
    for (std::size_t i = 0; i < value.count(); ++i) v.push_back(static_cast<std::uint64_t>(value.asInt(i)));
    return v;
  };
  for (const auto& [offsets, counts] :
       {std::make_pair(kStripOffsets, kStripByteCounts), std::make_pair(kTileOffsets, kTileByteCounts),
        std::make_pair(kJpegOffset, kJpegLength)}) {
    const auto o = array(offsets), c = array(counts);
    for (std::size_t i = 0; i < o.size() && i < c.size(); ++i) out.push_back({o[i], o[i] + c[i]});
  }
}

// `ranges` less every part of them that lies in one of `keep`.
std::vector<Range> subtract(std::vector<Range> ranges, std::vector<Range> keep) {
  std::sort(keep.begin(), keep.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
  std::vector<Range> out;
  for (auto r : ranges) {
    for (const auto& k : keep) {
      if (k.end <= r.begin || k.begin >= r.end) continue;
      if (k.begin > r.begin) out.push_back({r.begin, k.begin});
      r.begin = std::max(r.begin, k.end);
      if (r.begin >= r.end) break;
    }
    if (r.begin < r.end) out.push_back(r);
  }
  std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
  std::vector<Range> merged;
  for (const auto& r : out) {
    if (!merged.empty() && r.begin <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, r.end);
    } else {
      merged.push_back(r);
    }
  }
  return merged;
}

class TiffFile final : public ImageFile {
 public:
  TiffFile(FileFormat format, std::unique_ptr<InputSource> source) : ImageFile(format, std::move(source)) {}

 protected:
  void doLoad() override {
    TiffDecodeOptions options;
    options.keepOrigin = false;
    options.allowRawMagic = true;
    decodeTiff(source(), exif_, options);

    // The metadata blocks are not Exif data.
    const auto order = exif_.byteOrder();
    if (auto* e = exif_.find(ExifTag(Ifd::ifd0, kXmlPacket))) {
      const Bytes b = e->value().encode(order);
      setXmpPacket(std::string(b.begin(), b.end()));
      exif_.remove(ExifTag(Ifd::ifd0, kXmlPacket));
    }
    if (auto* e = exif_.find(ExifTag(Ifd::ifd0, kIptcNaa))) {
      // Often typed LONG, but the bytes are IIM.
      const Bytes b = e->value().encode(order);
      try {
        iptc_ = IptcMetadata::decode(b.data(), b.size());
      } catch (const Error& err) {
        iptc_.clear();
        warn(std::string("TIFF IPTC not read: ") + err.what());
      }
      exif_.remove(ExifTag(Ifd::ifd0, kIptcNaa));
    }
    if (auto* e = exif_.find(ExifTag(Ifd::ifd0, kImageResources))) {
      const Bytes b = e->value().encode(order);
      if (iptc_.empty()) {
        try {
          if (auto iptc = iptcFromImageResources(parseImageResources(b.data(), b.size()))) iptc_ = std::move(*iptc);
        } catch (const Error& err) {
          iptc_.clear();
          warn(std::string("TIFF IPTC not read: ") + err.what());
        }
      }
      exif_.remove(ExifTag(Ifd::ifd0, kImageResources));
    }
    if (auto* e = exif_.find(ExifTag(Ifd::ifd0, kIccProfile))) {
      icc_ = e->value().encode(order);
      exif_.remove(ExifTag(Ifd::ifd0, kIccProfile));
    }
    // Panasonic's RW2 keeps the maker note in the Exif of the JPEG it
    // carries (tag 0x002E); its decoded view is taken from there.
    if (!exif_.makerNote()) {
      if (const auto* jpeg = exif_.find(ExifTag(Ifd::ifd0, kJpgFromRaw));
          jpeg && jpeg->type() == FieldType::undefined) {
        try {
          const Bytes& b = jpeg->value().bytes();
          JpegMetadata preview = readJpegMetadata(SpanSource(b.data(), b.size()));
          ExifAccess::makerNote(exif_) = ExifAccess::makerNote(preview.exif);
        } catch (const Error&) {
          // No preview metadata: no maker note.
        }
      }
    }
    if (const auto* w = exif_.find(ExifTag(Ifd::ifd0, 0x0100)); w && w->count()) {
      width_ = static_cast<std::uint32_t>(w->asInt());
    }
    if (const auto* h = exif_.find(ExifTag(Ifd::ifd0, 0x0101)); h && h->count()) {
      height_ = static_cast<std::uint32_t>(h->asInt());
    }
  }

  void doSave(OutputSink& sink) const override {
    const InputSource& src = source();
    const std::uint64_t fileSize = src.size();
    std::uint8_t header[8];
    if (fileSize < 8) corrupt("not a TIFF");
    src.read(0, header, 8);
    if (!isTiffHeader(header, 8, true)) corrupt("not a TIFF");
    const ByteOrder order = header[0] == 'I' ? ByteOrder::little : ByteOrder::big;
    const auto ifd0 = readRawIfd(src, get32(header + 4, order), order);
    if (!ifd0) corrupt("TIFF IFD0 is damaged");

    std::vector<Range> blank;  // old metadata, zeroed
    std::vector<Range> keep;   // what must not be zeroed
    const auto retire = [&](const RawIfd& ifd) {
      blank.push_back(ifd.table());
      for (const auto& r : ifd.entries) {
        if (!r.isInline()) blank.push_back({r.valueOffset, r.valueOffset + r.byteCount});
      }
    };
    const auto keepValue = [&](const RawEntry& r) {
      if (!r.isInline()) keep.push_back({r.valueOffset, r.valueOffset + r.byteCount});
    };
    imageDataRanges(src, *ifd0, order, keep);

    std::optional<RawIfd> exifIfd, gpsIfd, interopIfd;
    if (auto p = pointerOf(*ifd0, kExifIfdPointer, order)) exifIfd = readRawIfd(src, p, order);
    if (auto p = pointerOf(*ifd0, kGpsIfdPointer, order)) gpsIfd = readRawIfd(src, p, order);
    if (exifIfd) {
      if (auto p = pointerOf(*exifIfd, kInteropIfdPointer, order)) interopIfd = readRawIfd(src, p, order);
    }
    retire(*ifd0);
    if (exifIfd) retire(*exifIfd);
    if (gpsIfd) retire(*gpsIfd);
    if (interopIfd) retire(*interopIfd);

    // ---- IFD0
    IfdLayout newIfd0;
    std::set<std::uint16_t> inFile;
    for (const auto& r : ifd0->entries) {
      inFile.insert(r.tag);
      if (isManaged(r.tag) || isDescriptive(r.tag)) continue;
      newIfd0.entries.push_back(keptEntry(r));
      keepValue(r);
    }
    std::set<std::uint16_t> written;
    for (const auto& e : exif_) {
      if (e.ifd() != Ifd::ifd0 || e.value().empty() || isManaged(e.number())) continue;
      if (!written.insert(e.number()).second) continue;
      if (isDescriptive(e.number()) || !inFile.count(e.number())) {
        newIfd0.entries.push_back(valueEntry(e.number(), e.value(), order));
      }
    }
    if (!xmp_.empty()) {
      const std::string packet = xmp_.serialize();
      newIfd0.entries.push_back(
          valueEntry(kXmlPacket, FieldValue::fromBytes(FieldType::u8, Bytes(packet.begin(), packet.end())), order));
    }
    const RawEntry* resources = ifd0->find(kImageResources);
    if (resources && resources->byteCount <= kMaxValueSize) {
      // Photoshop's resources keep everything but the IPTC they carry, which
      // follows the IPTC data.
      const Bytes raw = src.readBytes(resources->valueOffset, static_cast<std::size_t>(resources->byteCount));
      auto list = parseImageResources(raw.data(), raw.size());
      setIptcImageResource(list, iptc_);
      const Bytes block = serializeImageResources(list);
      if (!block.empty()) {
        newIfd0.entries.push_back(valueEntry(kImageResources, FieldValue::fromBytes(FieldType::u8, block), order));
      }
    }
    if (!iptc_.empty() && !(resources && resources->byteCount <= kMaxValueSize)) {
      Bytes iim = iptc_.encode();
      const RawEntry* old = ifd0->find(kIptcNaa);
      if (old && (old->type == FieldType::u32)) {
        // As LONGs, the way Photoshop writes it.
        iim.resize((iim.size() + 3) & ~std::size_t{3}, 0);
        std::vector<std::int64_t> longs;
        for (std::size_t i = 0; i < iim.size(); i += 4) longs.push_back(get32(iim.data() + i, order));
        newIfd0.entries.push_back(valueEntry(kIptcNaa, FieldValue::integers(FieldType::u32, longs), order));
      } else {
        newIfd0.entries.push_back(valueEntry(kIptcNaa, FieldValue::fromBytes(FieldType::undefined, iim), order));
      }
    }
    if (iccChanged_) {
      if (!icc_.empty()) {
        newIfd0.entries.push_back(valueEntry(kIccProfile, FieldValue::fromBytes(FieldType::undefined, icc_), order));
      }
    } else if (const auto* icc = ifd0->find(kIccProfile)) {
      newIfd0.entries.push_back(keptEntry(*icc));
      keepValue(*icc);
    }

    // ---- Exif, GPS and interoperability IFDs, from the Exif data
    const auto fromData = [&](Ifd which, IfdLayout& layout) {
      std::set<std::uint16_t> tags;
      for (const auto& e : exif_) {
        if (e.ifd() != which || e.value().empty() || isStructuralTag(e.ifd(), e.number())) continue;
        if (!tags.insert(e.number()).second) continue;
        layout.entries.push_back(valueEntry(e.number(), e.value(), order));
      }
    };
    IfdLayout newExif, newGps, newInterop;
    fromData(Ifd::exif, newExif);
    fromData(Ifd::gps, newGps);
    fromData(Ifd::interop, newInterop);

    // An unchanged maker note stays where it is.
    if (exifIfd) {
      if (const RawEntry* note = exifIfd->find(kMakerNote); note && !note->isInline()) {
        const auto* current = exif_.find(ExifTag(Ifd::exif, kMakerNote));
        if (current && note->byteCount <= kMaxValueSize) {
          const Bytes raw = src.readBytes(note->valueOffset, static_cast<std::size_t>(note->byteCount));
          if (current->value() == FieldValue::decode(note->type, raw.data(), raw.size(), note->count, order)) {
            for (auto& e : newExif.entries) {
              if (e.tag == kMakerNote) e = keptEntry(*note);
            }
            keepValue(*note);
          }
        }
      }
    }

    if (!newInterop.entries.empty()) newExif.entries.push_back(longPointer(kInteropIfdPointer, 0, order));
    if (!newExif.entries.empty()) newIfd0.entries.push_back(longPointer(kExifIfdPointer, 0, order));
    if (!newGps.entries.empty()) newIfd0.entries.push_back(longPointer(kGpsIfdPointer, 0, order));
    for (auto* l : {&newIfd0, &newExif, &newInterop, &newGps}) l->sort();

    // ---- Where the new IFDs go: after the file, or over metadata this
    // writer appended before, which ends the file.
    const auto zeros = subtract(blank, keep);
    std::uint64_t end = fileSize;
    for (bool moved = true; moved;) {
      moved = false;
      for (const auto& z : zeros) {
        if (z.end > end || z.end + 3 < end || z.begin >= end) continue;
        // Only zeros (padding) may lie between the range and the end.
        if (z.end < end) {
          const Bytes gap = src.readBytes(z.end, static_cast<std::size_t>(end - z.end));
          if (std::any_of(gap.begin(), gap.end(), [](std::uint8_t b) { return b != 0; })) continue;
        }
        end = z.begin;
        moved = true;
      }
    }
    // Everything kept must lie before that point.
    for (const auto& k : keep) end = std::max(end, std::min(k.end, fileSize));
    const std::uint64_t start = (end + 1) & ~std::uint64_t{1};

    std::uint64_t at = start;
    const std::uint64_t ifd0At = at;
    at += newIfd0.size();
    const std::uint64_t exifAt = at;
    if (!newExif.entries.empty()) at += newExif.size();
    const std::uint64_t interopAt = at;
    if (!newInterop.entries.empty()) at += newInterop.size();
    const std::uint64_t gpsAt = at;
    if (!newGps.entries.empty()) at += newGps.size();
    if (at > 0xffffffffULL) throw Error(ErrorCode::dataTooLarge, "TIFF exceeds 4 GB");
    setPointer(newIfd0, kExifIfdPointer, exifAt, order);
    setPointer(newIfd0, kGpsIfdPointer, gpsAt, order);
    setPointer(newExif, kInteropIfdPointer, interopAt, order);

    Bytes tail(static_cast<std::size_t>(start - std::min(start, end)), 0);
    newIfd0.write(tail, end, ifd0->next, order);
    if (!newExif.entries.empty()) newExif.write(tail, end, 0, order);
    if (!newInterop.entries.empty()) newInterop.write(tail, end, 0, order);
    if (!newGps.entries.empty()) newGps.write(tail, end, 0, order);

    // ---- Out: the header, the file with the old metadata zeroed, the tail.
    put32(header + 4, static_cast<std::uint32_t>(ifd0At), order);
    sink.write(header, 8);
    std::uint64_t pos = 8;
    const Bytes zeroChunk(64 * 1024, 0);
    for (const auto& z : zeros) {
      const std::uint64_t b = std::max<std::uint64_t>(z.begin, 8), e = std::min(z.end, end);
      if (b >= e) continue;
      if (b > pos) sink.copyFrom(src, pos, b - pos);
      for (std::uint64_t n = e - b; n > 0;) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(n, zeroChunk.size()));
        sink.write(zeroChunk.data(), chunk);
        n -= chunk;
      }
      pos = e;
    }
    if (end > pos) sink.copyFrom(src, pos, end - pos);
    sink.write(tail);
  }
};

}  // namespace

std::unique_ptr<ImageFile> newTiffFile(FileFormat format, std::unique_ptr<InputSource> source) {
  return std::make_unique<TiffFile>(format, std::move(source));
}

}  // namespace lumenlib::detail

// MP4 and QuickTime movies, read only (include/lumenlib/movie.hpp):
//
//   moov/mvhd                 the movie's times, timescale and duration
//   moov/mvex                 present in a fragmented movie (mehd: its length)
//   moov/trak/tkhd            the track's id, flags and display matrix
//   moov/trak/edts/elst       its edit list
//   moov/trak/mdia/hdlr       the track's kind
//   moov/trak/mdia/mdhd       its timescale and duration
//   .../minf/stbl/stsd        the first sample entry: the codec, a picture's
//                             size and pasp, an encrypted entry's frma
//   .../minf/stbl/stts        the samples' durations
//   moov/meta                 QuickTime keys (hdlr "mdta": keys + ilst)
//   moov/udta/meta            keys again (FFmpeg's use_metadata_tags), or
//                             iTunes items (hdlr "mdir": ilst by box type)
//   moov/udta/©xxx            QuickTime user data text: ©xyz, ©day, ©mak...
//
// The movie box is all that is read: the samples in mdat never are. A
// damaged tag loses itself only, with a warning; a damaged track or header
// is corrupt data.
#include <lumenlib/error.hpp>
#include <lumenlib/movie.hpp>

#include "bmff_boxes.hpp"
#include "bytes.hpp"

#include <cstdio>
#include <cstring>
#include <limits>

namespace lumenlib {

namespace detail {
namespace {

constexpr std::uint64_t kMaxBox = 64u << 20;  // a box read into memory whole
constexpr std::size_t kMaxText = 1u << 16;    // a tag's value

Bytes payloadOf(const InputSource& src, const Box& b) {
  if (b.size() > kMaxBox) throw Error(ErrorCode::dataTooLarge, "'" + b.type + "' box too large");
  return src.readBytes(b.payload, static_cast<std::size_t>(b.size()));
}

const Box* find(const std::vector<Box>& list, const char* type) {
  for (const Box& b : list) {
    if (b.type == type) return &b;
  }
  return nullptr;
}

// duration / timescale seconds in milliseconds, to the nearest.
std::uint64_t toMs(std::uint64_t duration, std::uint32_t timescale) {
  if (timescale == 0) return 0;
  return duration / timescale * 1000 + ((duration % timescale) * 1000 + timescale / 2) / timescale;
}

// A version 0 or 1 full box's time pair, timescale and duration (mvhd, mdhd).
struct Times {
  std::uint64_t created = 0;
  std::uint64_t modified = 0;
  std::uint32_t timescale = 0;
  std::uint64_t duration = 0;
};

Times readTimes(Reader& r) {
  const auto version = r.read(1);
  r.skip(3);
  const std::size_t width = version == 1 ? 8 : 4;
  Times t;
  t.created = r.read(width);
  t.modified = r.read(width);
  t.timescale = static_cast<std::uint32_t>(r.read(4));
  t.duration = r.read(width);
  // All ones: unknown.
  if (t.duration == (width == 8 ? std::numeric_limits<std::uint64_t>::max() : 0xffffffffu)) t.duration = 0;
  return t;
}

bool validUtf8(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (std::size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
    }
    i += n;
  }
  return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
}

// Text that should be UTF-8; bytes that are not are taken as Latin-1.
std::string asUtf8(const std::uint8_t* p, std::size_t n) {
  while (n > 0 && p[n - 1] == 0) --n;
  std::string s = toText(p, n);
  if (validUtf8(s)) return s;
  std::string out;
  for (std::size_t i = 0; i < n; ++i) appendUtf8(out, p[i]);
  return out;
}

std::string fromUtf16Be(const std::uint8_t* p, std::size_t n) {
  std::string out;
  for (std::size_t i = 0; i + 1 < n; i += 2) {
    std::uint32_t cp = getBe16(p + i);
    if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < n) {
      const std::uint32_t low = getBe16(p + i + 2);
      if (low >= 0xdc00 && low < 0xe000) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        i += 2;
      }
    }
    if (cp == 0) break;
    appendUtf8(out, cp);
  }
  return out;
}

// A box type as an item's name: QuickTime's 0xA9 lead byte as "©". Empty for
// a type that is not text.
std::string itemName(const std::string& type) {
  std::string name;
  for (std::size_t i = 0; i < type.size(); ++i) {
    const auto c = static_cast<unsigned char>(type[i]);
    if (i == 0 && c == 0xa9) {
      name += "\xc2\xa9";
    } else if (c >= 0x20 && c < 0x7f) {
      name += static_cast<char>(c);
    } else {
      return {};
    }
  }
  return name;
}

// An iTunes-style `data` box's value as text: UTF-8, UTF-16, integers and
// floats. Absent for anything else (pictures, the implicit type).
std::optional<std::string> dataValue(const Bytes& d) {
  if (d.size() < 8) return std::nullopt;
  const std::uint32_t type = getBe32(d.data()) & 0xffffff;
  const std::uint8_t* p = d.data() + 8;
  const std::size_t n = d.size() - 8;
  if (n > kMaxText) return std::nullopt;
  char text[40];
  switch (type) {
    case 1:
      return asUtf8(p, n);
    case 2:
      return fromUtf16Be(p, n);
    case 21:
    case 22: {
      if (n == 0 || n > 8 || n == 5 || n == 6 || n == 7) return std::nullopt;
      std::uint64_t v = 0;
      for (std::size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
      if (type == 21 && (p[0] & 0x80)) {
        const std::uint64_t sign = n == 8 ? 0 : (~std::uint64_t{0} << (8 * n));
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(v | sign));
      } else {
        std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(v));
      }
      return std::string(text);
    }
    case 23: {
      if (n != 4) return std::nullopt;
      const std::uint32_t bits = getBe32(p);
      float f;
      std::memcpy(&f, &bits, 4);
      std::snprintf(text, sizeof text, "%.9g", static_cast<double>(f));
      return std::string(text);
    }
    case 24: {
      if (n != 8) return std::nullopt;
      const std::uint64_t bits = getBe64(p);
      double f;
      std::memcpy(&f, &bits, 8);
      std::snprintf(text, sizeof text, "%.17g", f);
      return std::string(text);
    }
    default:
      return std::nullopt;
  }
}

using Items = std::vector<MovieItem>;

class MovieReader {
 public:
  explicit MovieReader(const InputSource& src) : src_(src) {}

  MovieInfo read() {
    const Box moov = findMoov();
    const auto children = boxes(src_, moov.payload, moov.end, budget_);
    const Box* mvhd = find(children, "mvhd");
    if (!mvhd) corrupt("the movie box has no header");
    Reader r(payloadOf(src_, *mvhd));
    const Times t = readTimes(r);
    if (t.created != 0) info_.created = t.created;
    if (t.modified != 0) info_.modified = t.modified;
    info_.timescale = t.timescale;
    info_.duration_ms = toMs(t.duration, t.timescale);

    Items keys, udtaKeys, userData;
    for (const Box& b : children) {
      if (b.type == "trak") {
        readTrak(b);
      } else if (b.type == "mvex") {
        info_.fragmented = true;
        readMvex(b);
      } else if (b.type == "meta") {
        tags(b, [&] { readMeta(b, keys); });
      } else if (b.type == "udta") {
        tags(b, [&] { readUdta(b, udtaKeys, userData); });
      }
    }
    if (info_.duration_ms == 0) {
      for (const MovieTrack& track : info_.tracks) info_.duration_ms = std::max(info_.duration_ms, track.duration_ms);
    }
    for (Items* list : {&keys, &udtaKeys, &userData}) {
      info_.items.insert(info_.items.end(), list->begin(), list->end());
    }
    return std::move(info_);
  }

 private:
  // The top level is walked leniently: a file cut short, or with junk after
  // its movie box, still reads when the movie box itself is whole.
  Box findMoov() {
    const std::uint64_t end = src_.size();
    std::uint64_t pos = 0;
    bool first = true;
    bool damaged = false;
    while (inBounds(end, pos, 8)) {
      if (budget_-- == 0) corrupt("too many boxes");
      std::uint8_t h[16];
      src_.read(pos, h, 8);
      std::uint64_t size = getBe32(h);
      const std::string type = toText(h + 4, 4);
      if (first) {
        // Old QuickTime files have no ftyp.
        static const char* const kFirst[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "pnot", "uuid"};
        bool known = false;
        for (const char* k : kFirst) known = known || type == k;
        if (!known) throw Error(ErrorCode::unsupportedFormat, "not an MP4 or QuickTime movie");
        first = false;
      }
      std::uint64_t header = 8;
      if (size == 1) {
        if (!inBounds(end, pos, 16)) {
          damaged = true;
          break;
        }
        src_.read(pos + 8, h + 8, 8);
        size = getBe64(h + 8);
        header = 16;
      } else if (size == 0) {
        size = end - pos;
      }
      if (size < header || !inBounds(end, pos, size)) {
        if (type == "moov") corrupt("the movie box runs past the end of the file");
        damaged = true;
        break;
      }
      if (type == "ftyp" && size >= header + 4) {
        std::uint8_t brand[4];
        src_.read(pos + header, brand, 4);
        info_.brand = toText(brand, 4);
      }
      if (type == "moov") {
        Box b;
        b.type = type;
        b.offset = pos;
        b.payload = pos + header;
        b.end = pos + size;
        return b;
      }
      pos += size;
    }
    if (first) throw Error(ErrorCode::unsupportedFormat, "not an MP4 or QuickTime movie");
    if (damaged) corrupt("damaged before its movie box");
    throw Error(ErrorCode::unsupportedFormat, "no movie box");
  }

  // Runs a tag reader; a damaged tag box loses only what it holds.
  template <typename F>
  void tags(const Box& b, F read) {
    try {
      read();
    } catch (const Error& e) {
      if (e.code() == ErrorCode::io) throw;
      warn("movie tags in '" + b.type + "' not read: " + e.what());
    }
  }

  void readMvex(const Box& mvex) {
    const auto children = boxes(src_, mvex.payload, mvex.end, budget_);
    const Box* mehd = find(children, "mehd");
    if (!mehd || info_.duration_ms != 0) return;
    Reader r(payloadOf(src_, *mehd));
    const auto version = r.read(1);
    r.skip(3);
    info_.duration_ms = toMs(r.read(version == 1 ? 8 : 4), info_.timescale);
  }

  void readTrak(const Box& trak) {
    MovieTrack track;
    const auto children = boxes(src_, trak.payload, trak.end, budget_);
    if (const Box* tkhd = find(children, "tkhd")) {
      Reader r(payloadOf(src_, *tkhd));
      const auto version = r.read(1);
      const auto flags = r.read(3);
      track.enabled = (flags & 1) != 0;
      const std::size_t width = version == 1 ? 8 : 4;
      r.skip(2 * width);  // created, modified
      track.id = static_cast<std::uint32_t>(r.read(4));
      r.skip(4 + width + 16);  // reserved, duration, reserved, layer, group, volume, reserved
      for (auto& m : track.matrix) m = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.read(4)));
    }
    if (const Box* edts = find(children, "edts")) {
      const auto list = boxes(src_, edts->payload, edts->end, budget_);
      if (const Box* elst = find(list, "elst")) readElst(*elst, track);
    }
    if (const Box* mdia = find(children, "mdia")) readMdia(*mdia, track);
    info_.tracks.push_back(std::move(track));
  }

  void readElst(const Box& elst, MovieTrack& track) {
    Reader r(payloadOf(src_, elst));
    const auto version = r.read(1);
    r.skip(3);
    const auto count = r.read(4);
    const std::size_t width = version == 1 ? 8 : 4;
    if (count > r.left() / (2 * width + 4)) corrupt("the edit list runs past its box");
    for (std::uint64_t i = 0; i < count; ++i) {
      MovieEdit e;
      e.duration = r.read(width);
      const std::uint64_t media = r.read(width);
      e.media_time = width == 8 ? static_cast<std::int64_t>(media)
                                : static_cast<std::int64_t>(static_cast<std::int32_t>(media));
      e.rate = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.read(4)));
      track.edits.push_back(e);
    }
  }

  void readMdia(const Box& mdia, MovieTrack& track) {
    const auto children = boxes(src_, mdia.payload, mdia.end, budget_);
    if (const Box* hdlr = find(children, "hdlr")) {
      Reader r(payloadOf(src_, *hdlr));
      r.skip(8);  // version and flags; QuickTime's component type
      track.handler = r.fourcc();
    }
    if (const Box* mdhd = find(children, "mdhd")) {
      Reader r(payloadOf(src_, *mdhd));
      const Times t = readTimes(r);
      track.timescale = t.timescale;
      track.duration_ms = toMs(t.duration, t.timescale);
    }
    const Box* minf = find(children, "minf");
    if (!minf) return;
    const auto media = boxes(src_, minf->payload, minf->end, budget_);
    const Box* stbl = find(media, "stbl");
    if (!stbl) return;
    const auto tables = boxes(src_, stbl->payload, stbl->end, budget_);
    if (const Box* stsd = find(tables, "stsd")) readStsd(*stsd, track);
    if (const Box* stts = find(tables, "stts")) readStts(*stts, track);
  }

  void readStsd(const Box& stsd, MovieTrack& track) {
    if (stsd.size() < 8) corrupt("truncated sample description");
    const auto entries = boxes(src_, stsd.payload + 8, stsd.end, budget_);
    if (entries.empty()) return;
    const Box& entry = entries.front();
    track.codec = entry.type;
    // A sample entry's own fields, then its boxes (pasp, sinf...).
    std::uint64_t first = 0;
    if (track.handler == "vide") {
      if (entry.size() < 78) return;
      std::uint8_t size[4];
      src_.read(entry.payload + 24, size, 4);
      track.width = getBe16(size);
      track.height = getBe16(size + 2);
      first = entry.payload + 78;
    } else if (track.handler == "soun") {
      if (entry.size() < 28) return;
      std::uint8_t version[2];
      src_.read(entry.payload + 8, version, 2);
      const std::uint16_t v = getBe16(version);
      first = entry.payload + (v == 1 ? 44 : v == 2 ? 64 : 28);
    } else {
      return;
    }
    if (first > entry.end) return;
    // Optional extras: a damaged one is skipped, not the track.
    try {
      const auto extras = boxes(src_, first, entry.end, budget_);
      if (const Box* pasp = find(extras, "pasp"); pasp && pasp->size() >= 8) {
        std::uint8_t d[8];
        src_.read(pasp->payload, d, 8);
        const std::uint32_t h = getBe32(d);
        const std::uint32_t v = getBe32(d + 4);
        if (h != 0 && v != 0) {
          track.par_h = h;
          track.par_v = v;
        }
      }
      if (const Box* sinf = find(extras, "sinf")) {
        const auto protection = boxes(src_, sinf->payload, sinf->end, budget_);
        if (const Box* frma = find(protection, "frma"); frma && frma->size() >= 4) {
          std::uint8_t d[4];
          src_.read(frma->payload, d, 4);
          track.codec = toText(d, 4);
        }
      }
    } catch (const Error& e) {
      if (e.code() == ErrorCode::io) throw;
      warn("the '" + entry.type + "' sample entry's boxes not read: " + e.what());
    }
  }

  void readStts(const Box& stts, MovieTrack& track) {
    Reader r(payloadOf(src_, stts));
    r.skip(4);
    const auto count = r.read(4);
    if (count > r.left() / 8) corrupt("the sample durations run past their box");
    std::uint64_t smallest = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto samples = r.read(4);
      const auto delta = r.read(4);
      track.samples += samples;
      // The last sample's length is what the writer had left.
      const bool lastAlone = i + 1 == count && count > 1 && samples == 1;
      if (samples == 0 || delta == 0 || lastAlone) continue;
      if (smallest == 0 || delta < smallest) smallest = delta;
    }
    if (smallest != 0 && track.timescale != 0) {
      track.min_sample_ms = static_cast<double>(smallest) * 1000.0 / track.timescale;
    }
  }

  // A meta box: QuickTime's holds its boxes directly, an ISO one after a
  // version and flags; which it is shows in where its hdlr begins.
  void readMeta(const Box& meta, Items& out) {
    std::uint64_t first = meta.payload;
    if (meta.size() >= 8) {
      std::uint8_t peek[8];
      src_.read(meta.payload, peek, 8);
      if (std::memcmp(peek + 4, "hdlr", 4) != 0) first += 4;
    }
    if (first > meta.end) return;
    const auto children = boxes(src_, first, meta.end, budget_);
    const Box* ilst = find(children, "ilst");
    if (!ilst) return;
    std::vector<std::string> keyNames;
    const bool byKey = find(children, "keys") != nullptr;
    if (const Box* keys = find(children, "keys")) {
      Reader r(payloadOf(src_, *keys));
      r.skip(4);
      const auto count = r.read(4);
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto size = r.read(4);
        if (size < 8 || size - 8 > r.left()) corrupt("a key runs past its box");
        r.skip(4);  // the namespace: "mdta"
        const std::size_t at = r.pos();
        r.skip(static_cast<std::size_t>(size - 8));
        keyNames.push_back(asUtf8(r.data().data() + at, static_cast<std::size_t>(size - 8)));
      }
    }
    for (const Box& item : boxes(src_, ilst->payload, ilst->end, budget_)) {
      std::string name;
      if (byKey) {
        std::uint8_t code[4];
        src_.read(item.offset + 4, code, 4);
        const std::uint32_t index = getBe32(code);
        if (index == 0 || index > keyNames.size()) continue;
        name = keyNames[index - 1];
      } else {
        name = itemName(item.type);
      }
      readItem(item, byKey ? MovieItemKind::key : MovieItemKind::itunes, name, out);
    }
  }

  // An ilst item: its `data` box's value; a "----" item names itself.
  void readItem(const Box& item, MovieItemKind kind, std::string name, Items& out) {
    const auto parts = boxes(src_, item.payload, item.end, budget_);
    if (item.type == "----") {
      if (const Box* n = find(parts, "name"); n && n->size() >= 4) {
        const Bytes d = payloadOf(src_, *n);
        name = asUtf8(d.data() + 4, d.size() - 4);
      }
    }
    const Box* data = find(parts, "data");
    if (name.empty() || !data || data->size() > kMaxText + 8) return;
    if (auto value = dataValue(payloadOf(src_, *data))) out.push_back({kind, std::move(name), std::move(*value)});
  }

  void readUdta(const Box& udta, Items& keys, Items& userData) {
    for (const Box& b : boxes(src_, udta.payload, udta.end, budget_)) {
      if (b.type == "meta") {
        readMeta(b, keys);
        continue;
      }
      if (static_cast<unsigned char>(b.type[0]) != 0xa9) continue;
      const std::string name = itemName(b.type);
      if (name.empty() || b.size() > kMaxText + 8) continue;
      const Bytes d = payloadOf(src_, b);
      if (d.size() >= 8 && std::memcmp(d.data() + 4, "data", 4) == 0) {
        readItem(b, MovieItemKind::userData, name, userData);  // an iTunes-style value where QuickTime's text would be
        continue;
      }
      // QuickTime text: a 16-bit length and a language, then the text; the
      // first of its languages is the one kept.
      if (d.size() < 4) continue;
      const std::size_t length = getBe16(d.data());
      if (length > d.size() - 4) continue;
      userData.push_back({MovieItemKind::userData, name, asUtf8(d.data() + 4, length)});
    }
  }

  const InputSource& src_;
  std::size_t budget_ = kMaxBoxes;
  MovieInfo info_;
};

}  // namespace
}  // namespace detail

const MovieTrack* MovieInfo::firstTrack(std::string_view handler) const noexcept {
  for (const MovieTrack& track : tracks) {
    if (track.handler == handler) return &track;
  }
  return nullptr;
}

const std::string* MovieInfo::item(std::string_view name) const noexcept {
  for (const MovieItem& entry : items) {
    if (entry.name == name) return &entry.value;
  }
  return nullptr;
}

MovieInfo readMovie(const InputSource& source) { return detail::MovieReader(source).read(); }

MovieInfo readMovie(const std::filesystem::path& path) {
  const FileSource source(path);
  return readMovie(source);
}

}  // namespace lumenlib

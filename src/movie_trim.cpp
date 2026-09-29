// A trimmed copy of an MP4 or QuickTime movie (include/lumenlib/movie.hpp),
// with no sample decoded:
//
//   1. Each track's edit list (or the one it implies) is cut to the range:
//      that gives the new edit list and the media times it shows.
//   2. The samples those need are one run in decode order: from the sync
//      sample at or before the first one shown to the last one shown, which
//      holds whatever frame reordering needs.
//   3. The run is copied chunk by chunk — the input's chunks, cut to the run —
//      in the order the chunks lie in the file, which keeps its interleaving.
//   4. The movie box is copied with each track's sample tables cut to its run,
//      its new edit list and durations, and the tags filtered, and is written
//      ahead of the one media data box.
#include <lumenlib/error.hpp>
#include <lumenlib/movie.hpp>

#include "bmff_boxes.hpp"
#include "bytes.hpp"
#include "formats.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

namespace lumenlib {

namespace detail {
namespace {

constexpr std::uint64_t kMaxBox = 64u << 20;     // a box read into memory whole
constexpr std::uint64_t kMaxWalked = 1u << 26;   // samples walked one by one
constexpr std::int32_t kRateOne = 0x10000;       // an edit's media rate of 1, 16.16
constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

Bytes load(const InputSource& src, const Box& b) {
  if (b.size() > kMaxBox) throw Error(ErrorCode::dataTooLarge, "'" + b.type + "' box too large");
  return src.readBytes(b.payload, static_cast<std::size_t>(b.size()));
}

// The box as it is, header and all.
Bytes whole(const InputSource& src, const Box& b) {
  if (b.end - b.offset > kMaxBox) throw Error(ErrorCode::dataTooLarge, "'" + b.type + "' box too large");
  return src.readBytes(b.offset, static_cast<std::size_t>(b.end - b.offset));
}

const Box* find(const std::vector<Box>& list, const char* type) {
  for (const Box& b : list) {
    if (b.type == type) return &b;
  }
  return nullptr;
}

void appendBe64(Bytes& out, std::uint64_t v) {
  appendBe32(out, static_cast<std::uint32_t>(v >> 32));
  appendBe32(out, static_cast<std::uint32_t>(v));
}

void versionFlags(Bytes& out, std::uint8_t version, std::uint32_t flags) {
  out.push_back(version);
  out.push_back(static_cast<std::uint8_t>(flags >> 16));
  out.push_back(static_cast<std::uint8_t>(flags >> 8));
  out.push_back(static_cast<std::uint8_t>(flags));
}

Bytes box(std::string_view type, const Bytes& payload) {
  if (payload.size() > 0xffffffffu - 8) throw Error(ErrorCode::dataTooLarge, "a movie box over 4 GiB");
  Bytes out;
  out.reserve(8 + payload.size());
  appendBe32(out, static_cast<std::uint32_t>(8 + payload.size()));
  append(out, type.substr(0, 4));
  append(out, payload);
  return out;
}

// Writes `value` over `width` bytes at `at`.
void putBe(Bytes& data, std::size_t at, std::uint64_t value, std::size_t width) {
  if (width == 4 && value > 0xffffffffu) throw Error(ErrorCode::dataTooLarge, "a duration over 32 bits");
  if (!inBounds(data.size(), at, width)) corrupt("truncated header");
  for (std::size_t i = 0; i < width; ++i) data[at + i] = static_cast<std::uint8_t>(value >> (8 * (width - 1 - i)));
}

std::uint64_t checkedAdd(std::uint64_t a, std::uint64_t b) {
  if (a > kMax - b) corrupt("a time or size out of range");
  return a + b;
}

std::uint64_t checkedMul(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > kMax / a) corrupt("a time or size out of range");
  return a * b;
}

// v * to / from, rounded down or up. Timescales are 32-bit, so the remainder's
// product fits.
std::uint64_t rescale(std::uint64_t v, std::uint32_t to, std::uint32_t from, bool up = false) {
  if (from == 0) corrupt("a timescale of 0");
  const std::uint64_t whole = checkedMul(v / from, to);
  const std::uint64_t part = (v % from) * to;
  return checkedAdd(whole, part / from + (up && part % from != 0 ? 1 : 0));
}

std::string lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// Whether a key's or an item's name could say where the movie was made:
// Apple's com.apple.quicktime.location.*, FFmpeg's and Android's location,
// a ©xyz.
bool namesPlace(const std::string& name) {
  const std::string text = lower(name);
  for (const char* word : {"location", "iso6709", "gps", "latitude", "longitude", "coordinates", "\xc2\xa9xyz",
                           "\xa9xyz", "loci"}) {
    if (text.find(word) != std::string::npos) return true;
  }
  return false;
}

// ---- a track's tables --------------------------------------------------------

// A run of samples sharing a value: stts's durations, ctts's offsets, sbgp's
// groups.
struct Run {
  std::uint32_t count;
  std::uint32_t value;
};

struct ChunkRun {
  std::uint32_t first_chunk;  // 1-based
  std::uint32_t samples;
  std::uint32_t description;
};

struct Edit {
  std::uint64_t duration;  // movie ticks
  std::int64_t media_time; // media ticks; -1: an empty edit
  std::int32_t rate;
};

// A media time span, [begin, end).
struct Span {
  std::uint64_t begin;
  std::uint64_t end;
};

struct Grouping {
  Bytes head;  // version, flags, grouping type and its parameter
  std::vector<Run> runs;
};

struct Track {
  Box trak;
  std::uint32_t id = 0;
  bool enabled = true;
  std::string handler;
  std::vector<std::string> entries;  // the sample entries' types
  bool external = false;             // samples in another file (dref)
  bool placeless = false;            // timed metadata whose keys are all known and name no place
  std::uint32_t timescale = 0;
  std::uint64_t track_duration = 0;  // tkhd's, in movie ticks
  std::uint64_t media_duration = 0;  // mdhd's
  bool has_edits = false;
  std::vector<Edit> edits;
  std::vector<Run> stts;
  bool has_ctts = false;
  std::uint8_t ctts_version = 0;
  std::vector<Run> ctts;
  bool has_stss = false;
  std::vector<std::uint32_t> stss;
  bool has_stps = false;
  std::vector<std::uint32_t> stps;
  std::uint32_t sample_size = 0;  // all alike; else sizes
  std::vector<std::uint32_t> sizes;
  std::uint64_t samples = 0;
  std::vector<ChunkRun> stsc;
  std::vector<std::uint64_t> chunks;
  bool has_sdtp = false;
  Bytes sdtp;  // its payload: version, flags, a byte a sample
  std::vector<Grouping> groupings;

  // What the copy holds.
  bool keep = true;
  std::uint64_t first = 0;  // samples [first, last)
  std::uint64_t last = 0;
  std::int64_t shift = 0;   // added to every composition offset kept
  std::vector<Edit> new_edits;
  std::uint64_t new_track_duration = 0;
  std::uint64_t new_media_duration = 0;
  std::vector<std::size_t> out_chunks;  // into the plan, in sample order

  std::uint64_t sizeOf(std::uint64_t i) const { return sample_size != 0 ? sample_size : sizes[i]; }

  // The bytes of samples [a, b).
  std::uint64_t bytesOf(std::uint64_t a, std::uint64_t b) const {
    if (sample_size != 0) return checkedMul(b - a, sample_size);
    std::uint64_t n = 0;
    for (std::uint64_t i = a; i < b; ++i) n += sizes[i];
    return n;
  }

  // Sample i's decode time.
  std::uint64_t dtsOf(std::uint64_t i) const {
    std::uint64_t t = 0;
    std::uint64_t at = 0;
    for (const Run& r : stts) {
      const std::uint64_t n = std::min<std::uint64_t>(r.count, i - at);
      t = checkedAdd(t, checkedMul(n, r.value));
      at += n;
      if (at == i) break;
    }
    return t;
  }

  // The durations of samples [a, b).
  std::uint64_t durationOf(std::uint64_t a, std::uint64_t b) const {
    std::uint64_t t = 0;
    std::uint64_t at = 0;
    for (const Run& r : stts) {
      const std::uint64_t begin = std::max(at, a);
      const std::uint64_t end = std::min(at + r.count, b);
      if (begin < end) t = checkedAdd(t, checkedMul(end - begin, r.value));
      at += r.count;
      if (at >= b) break;
    }
    return t;
  }
};

std::vector<Run> readRuns(Reader& r) {
  const auto count = r.read(4);
  if (count > r.left() / 8) corrupt("a sample table runs past its box");
  std::vector<Run> runs;
  runs.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto n = static_cast<std::uint32_t>(r.read(4));
    runs.push_back({n, static_cast<std::uint32_t>(r.read(4))});
  }
  return runs;
}

std::vector<std::uint32_t> readNumbers(Reader& r) {
  const auto count = r.read(4);
  if (count > r.left() / 4) corrupt("a sample table runs past its box");
  std::vector<std::uint32_t> out;
  out.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) out.push_back(static_cast<std::uint32_t>(r.read(4)));
  return out;
}

// Runs cut to samples [first, last).
std::vector<Run> cut(const std::vector<Run>& runs, std::uint64_t first, std::uint64_t last) {
  std::vector<Run> out;
  std::uint64_t at = 0;
  for (const Run& r : runs) {
    const std::uint64_t begin = std::max(at, first);
    const std::uint64_t end = std::min(at + r.count, last);
    if (begin < end) out.push_back({static_cast<std::uint32_t>(end - begin), r.value});
    at += r.count;
    if (at >= last) break;
  }
  return out;
}

// Sample numbers (1-based) in [first, last), renumbered from the copy's first.
std::vector<std::uint32_t> cutNumbers(const std::vector<std::uint32_t>& numbers, std::uint64_t first,
                                      std::uint64_t last) {
  std::vector<std::uint32_t> out;
  for (const std::uint32_t n : numbers) {
    if (n > first && n <= last) out.push_back(static_cast<std::uint32_t>(n - first));
  }
  return out;
}

Bytes runsBox(std::string_view type, std::uint8_t version, const std::vector<Run>& runs) {
  Bytes p;
  versionFlags(p, version, 0);
  appendBe32(p, static_cast<std::uint32_t>(runs.size()));
  for (const Run& r : runs) {
    appendBe32(p, r.count);
    appendBe32(p, r.value);
  }
  return box(type, p);
}

Bytes numbersBox(std::string_view type, const std::vector<std::uint32_t>& numbers) {
  Bytes p;
  versionFlags(p, 0, 0);
  appendBe32(p, static_cast<std::uint32_t>(numbers.size()));
  for (const std::uint32_t n : numbers) appendBe32(p, n);
  return box(type, p);
}

// ---- the copy ------------------------------------------------------------------

struct Chunk {
  std::size_t track;
  std::uint64_t offset;  // in the input
  std::uint64_t bytes;
  std::uint32_t samples;
  std::uint32_t description;
  std::uint64_t out = 0;  // in the copy
};

class Trimmer {
 public:
  Trimmer(const InputSource& src, const MovieTrim& trim) : src_(src), trim_(trim) {}

  TrimmedMovie write(OutputSink& out) {
    readTop();
    readMoov();
    decide();
    plan();

    // Everything before the media data: the file type, what else is kept at
    // the top, and the movie box, whose size the chunks' offsets depend on.
    Bytes head;
    if (ftyp_) append(head, whole(src_, *ftyp_));
    if (trim_.tags == MovieTags::all) {
      for (const Box& b : topKept_) append(head, whole(src_, b));
    }
    std::uint64_t total = 0;
    for (const Chunk& c : plan_) total = checkedAdd(total, c.bytes);
    const bool large = total > 0xffffffffu - 8;
    const std::uint64_t mdatHeader = large ? 16 : 8;

    Bytes moov;
    std::uint64_t moovSize = 0;
    for (int pass = 0;; ++pass) {
      place(head.size() + moovSize + mdatHeader);
      moov = buildMoov();
      if (moov.size() == moovSize) break;
      if (pass == 4) corrupt("the movie box does not settle");
      moovSize = moov.size();
    }

    out.write(head);
    out.write(moov);
    Bytes mdat;
    if (large) {
      appendBe32(mdat, 1);
      append(mdat, std::string_view("mdat"));
      appendBe64(mdat, total + 16);
    } else {
      appendBe32(mdat, static_cast<std::uint32_t>(total + 8));
      append(mdat, std::string_view("mdat"));
    }
    out.write(mdat);
    std::uint64_t done = 0;
    for (const std::size_t i : order_) {
      if (trim_.progress && !trim_.progress(done, total)) throw Error(ErrorCode::cancelled, "the trim was stopped");
      out.copyFrom(src_, plan_[i].offset, plan_[i].bytes);
      done += plan_[i].bytes;
    }
    if (trim_.progress && !trim_.progress(done, total)) throw Error(ErrorCode::cancelled, "the trim was stopped");

    std::uint64_t duration = 0;
    for (const Track& t : tracks_) {
      if (t.keep) duration = std::max(duration, t.new_track_duration);
    }
    TrimmedMovie result;
    // To the nearest millisecond, as the reader says.
    result.duration_ms = checkedAdd(checkedMul(duration / movieScale_, 1000),
                                    ((duration % movieScale_) * 1000 + movieScale_ / 2) / movieScale_);
    return result;
  }

 private:
  // ---- reading ----

  // The top level, walked leniently as the reader does.
  void readTop() {
    const std::uint64_t end = src_.size();
    std::uint64_t pos = 0;
    bool first = true;
    while (inBounds(end, pos, 8)) {
      if (budget_-- == 0) corrupt("too many boxes");
      std::uint8_t h[16];
      src_.read(pos, h, 8);
      std::uint64_t size = getBe32(h);
      const std::string type = toText(h + 4, 4);
      if (first) {
        static const char* const kFirst[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "pnot", "uuid"};
        bool known = false;
        for (const char* k : kFirst) known = known || type == k;
        if (!known) throw Error(ErrorCode::unsupportedFormat, "not an MP4 or QuickTime movie");
        first = false;
      }
      std::uint64_t header = 8;
      if (size == 1) {
        if (!inBounds(end, pos, 16)) break;
        src_.read(pos + 8, h + 8, 8);
        size = getBe64(h + 8);
        header = 16;
      } else if (size == 0) {
        size = end - pos;
      }
      if (size < header || !inBounds(end, pos, size)) {
        if (type == "moov") corrupt("the movie box runs past the end of the file");
        break;
      }
      Box b;
      b.type = type;
      b.offset = pos;
      b.payload = pos + header;
      b.end = pos + size;
      if (type == "ftyp" && !ftyp_) {
        ftyp_ = b;
      } else if (type == "moov" && !moov_) {
        moov_ = b;
      } else if (type == "moof") {
        fragmented_ = true;
      } else if (type == "uuid" || type == "meta") {
        topKept_.push_back(b);
      }
      pos += size;
    }
    if (first) throw Error(ErrorCode::unsupportedFormat, "not an MP4 or QuickTime movie");
    if (!moov_) throw Error(ErrorCode::unsupportedFormat, "no movie box");
  }

  void readMoov() {
    moovChildren_ = boxes(src_, moov_->payload, moov_->end, budget_);
    const Box* mvhd = find(moovChildren_, "mvhd");
    if (!mvhd) corrupt("the movie box has no header");
    if (fragmented_ || find(moovChildren_, "mvex")) {
      throw Error(ErrorCode::unsupportedOperation, "a fragmented movie cannot be trimmed");
    }
    Reader r(load(src_, *mvhd));
    const auto version = r.read(1);
    r.skip(3);
    const std::size_t width = version == 1 ? 8 : 4;
    r.skip(2 * width);
    movieScale_ = static_cast<std::uint32_t>(r.read(4));
    movieDuration_ = r.read(width);
    if (movieDuration_ == (width == 8 ? kMax : 0xffffffffu)) movieDuration_ = 0;
    if (movieScale_ == 0) corrupt("the movie's timescale is 0");
    for (const Box& b : moovChildren_) {
      if (b.type == "trak") tracks_.push_back(readTrack(b));
    }
    if (tracks_.empty()) corrupt("a movie with no track");
  }

  Track readTrack(const Box& trak) {
    Track t;
    t.trak = trak;
    const auto children = boxes(src_, trak.payload, trak.end, budget_);
    const Box* tkhd = find(children, "tkhd");
    if (!tkhd) corrupt("a track with no header");
    {
      Reader r(load(src_, *tkhd));
      const auto version = r.read(1);
      t.enabled = (r.read(3) & 1) != 0;
      const std::size_t width = version == 1 ? 8 : 4;
      r.skip(2 * width);
      t.id = static_cast<std::uint32_t>(r.read(4));
      r.skip(4);
      t.track_duration = r.read(width);
      if (t.track_duration == (width == 8 ? kMax : 0xffffffffu)) t.track_duration = 0;
    }
    if (const Box* edts = find(children, "edts")) {
      const auto list = boxes(src_, edts->payload, edts->end, budget_);
      if (const Box* elst = find(list, "elst")) {
        t.has_edits = true;
        Reader r(load(src_, *elst));
        const auto version = r.read(1);
        r.skip(3);
        const auto count = r.read(4);
        const std::size_t width = version == 1 ? 8 : 4;
        if (count > r.left() / (2 * width + 4)) corrupt("the edit list runs past its box");
        for (std::uint64_t i = 0; i < count; ++i) {
          Edit e;
          e.duration = r.read(width);
          const std::uint64_t media = r.read(width);
          e.media_time = width == 8 ? static_cast<std::int64_t>(media)
                                    : static_cast<std::int64_t>(static_cast<std::int32_t>(media));
          e.rate = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.read(4)));
          t.edits.push_back(e);
        }
      }
    }
    const Box* mdia = find(children, "mdia");
    if (!mdia) corrupt("a track with no media");
    const auto media = boxes(src_, mdia->payload, mdia->end, budget_);
    if (const Box* hdlr = find(media, "hdlr")) {
      Reader r(load(src_, *hdlr));
      r.skip(8);
      t.handler = r.fourcc();
    }
    const Box* mdhd = find(media, "mdhd");
    if (!mdhd) corrupt("a track with no media header");
    {
      Reader r(load(src_, *mdhd));
      const auto version = r.read(1);
      r.skip(3);
      const std::size_t width = version == 1 ? 8 : 4;
      r.skip(2 * width);
      t.timescale = static_cast<std::uint32_t>(r.read(4));
      t.media_duration = r.read(width);
      if (t.media_duration == (width == 8 ? kMax : 0xffffffffu)) t.media_duration = 0;
    }
    if (t.timescale == 0) corrupt("a track's timescale is 0");
    const Box* minf = find(media, "minf");
    if (!minf) corrupt("a track with no media information");
    const auto info = boxes(src_, minf->payload, minf->end, budget_);
    if (const Box* dinf = find(info, "dinf")) readDataReferences(*dinf, t);
    const Box* stbl = find(info, "stbl");
    if (!stbl) corrupt("a track with no sample tables");
    readTables(*stbl, t);
    return t;
  }

  // Whether every data reference is the file itself.
  void readDataReferences(const Box& dinf, Track& t) {
    const auto list = boxes(src_, dinf.payload, dinf.end, budget_);
    const Box* dref = find(list, "dref");
    if (!dref || dref->size() < 8) return;
    for (const Box& entry : boxes(src_, dref->payload + 8, dref->end, budget_)) {
      if (entry.size() < 4) corrupt("a truncated data reference");
      std::uint8_t vf[4];
      src_.read(entry.payload, vf, 4);
      if ((vf[3] & 1) == 0) t.external = true;
    }
  }

  void readTables(const Box& stbl, Track& t) {
    const auto tables = boxes(src_, stbl.payload, stbl.end, budget_);
    const Box* stsd = find(tables, "stsd");
    const Box* stts = find(tables, "stts");
    const Box* stsz = find(tables, "stsz");
    const Box* stz2 = find(tables, "stz2");
    const Box* stsc = find(tables, "stsc");
    const Box* stco = find(tables, "stco");
    const Box* co64 = find(tables, "co64");
    if (!stsd || !stts || !(stsz || stz2) || !stsc || !(stco || co64)) corrupt("a track without its sample tables");

    if (stsd->size() < 8) corrupt("truncated sample description");
    const auto entries = boxes(src_, stsd->payload + 8, stsd->end, budget_);
    bool placeless = !entries.empty();
    for (const Box& e : entries) {
      t.entries.push_back(e.type);
      placeless = placeless && e.type == "mebx" && keysNameNoPlace(e);
    }
    t.placeless = placeless;

    {
      Reader r(load(src_, *stts));
      r.skip(4);
      t.stts = readRuns(r);
    }
    if (const Box* ctts = find(tables, "ctts")) {
      Reader r(load(src_, *ctts));
      t.has_ctts = true;
      t.ctts_version = static_cast<std::uint8_t>(r.read(1));
      r.skip(3);
      t.ctts = readRuns(r);
    }
    if (const Box* stss = find(tables, "stss")) {
      Reader r(load(src_, *stss));
      r.skip(4);
      t.has_stss = true;
      t.stss = readNumbers(r);
    }
    if (const Box* stps = find(tables, "stps")) {
      Reader r(load(src_, *stps));
      r.skip(4);
      t.has_stps = true;
      t.stps = readNumbers(r);
    }
    if (stsz) {
      Reader r(load(src_, *stsz));
      r.skip(4);
      t.sample_size = static_cast<std::uint32_t>(r.read(4));
      t.samples = r.read(4);
      if (t.sample_size == 0) {
        if (t.samples > r.left() / 4) corrupt("the sample sizes run past their box");
        t.sizes.reserve(static_cast<std::size_t>(t.samples));
        for (std::uint64_t i = 0; i < t.samples; ++i) t.sizes.push_back(static_cast<std::uint32_t>(r.read(4)));
      }
    } else {
      Reader r(load(src_, *stz2));
      r.skip(7);
      const auto field = r.read(1);
      t.samples = r.read(4);
      if (field != 4 && field != 8 && field != 16) corrupt("a compact sample size of " + std::to_string(field) + " bits");
      if (t.samples > r.left() * 8 / field) corrupt("the sample sizes run past their box");
      t.sizes.reserve(static_cast<std::size_t>(t.samples));
      for (std::uint64_t i = 0; i < t.samples; ++i) {
        if (field == 4) {
          const std::uint8_t byte = r.data()[r.pos() + static_cast<std::size_t>(i / 2)];
          t.sizes.push_back(i % 2 == 0 ? byte >> 4 : byte & 0x0f);
        } else {
          t.sizes.push_back(static_cast<std::uint32_t>(r.read(field / 8)));
        }
      }
    }
    {
      Reader r(load(src_, *stsc));
      r.skip(4);
      const auto count = r.read(4);
      if (count > r.left() / 12) corrupt("the sample-to-chunk table runs past its box");
      for (std::uint64_t i = 0; i < count; ++i) {
        ChunkRun c;
        c.first_chunk = static_cast<std::uint32_t>(r.read(4));
        c.samples = static_cast<std::uint32_t>(r.read(4));
        c.description = static_cast<std::uint32_t>(r.read(4));
        t.stsc.push_back(c);
      }
    }
    {
      const bool wide = stco == nullptr;
      Reader r(load(src_, wide ? *co64 : *stco));
      r.skip(4);
      const auto count = r.read(4);
      if (count > r.left() / (wide ? 8 : 4)) corrupt("the chunk offsets run past their box");
      t.chunks.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i < count; ++i) t.chunks.push_back(r.read(wide ? 8 : 4));
    }
    if (const Box* sdtp = find(tables, "sdtp")) {
      t.has_sdtp = true;
      t.sdtp = load(src_, *sdtp);
    }
    for (const Box& b : tables) {
      if (b.type != "sbgp") continue;
      Reader r(load(src_, b));
      const auto version = r.read(1);
      Grouping g;
      const std::size_t head = version == 1 ? 12 : 8;
      if (!r.has(head)) corrupt("truncated sample grouping");
      g.head.assign(r.data().begin(), r.data().begin() + static_cast<std::ptrdiff_t>(head));
      r.skip(head - 1);
      g.runs = readRuns(r);
      t.groupings.push_back(std::move(g));
    }

    // Samples with no time are not samples.
    std::uint64_t timed = 0;
    for (const Run& r : t.stts) timed += r.count;
    t.samples = std::min(t.samples, timed);
  }

  // Whether a timed-metadata sample entry's keys (mebx: keys, then a box per
  // local key holding its keyd) are all readable and none names a place.
  bool keysNameNoPlace(const Box& entry) {
    try {
      if (entry.size() < 8) return false;
      const auto children = boxes(src_, entry.payload + 8, entry.end, budget_);
      const Box* keys = find(children, "keys");
      if (!keys) return false;
      for (const Box& local : boxes(src_, keys->payload, keys->end, budget_)) {
        const auto parts = boxes(src_, local.payload, local.end, budget_);
        const Box* keyd = find(parts, "keyd");
        if (!keyd || keyd->size() < 4) return false;
        const Bytes d = load(src_, *keyd);
        if (namesPlace(toText(d.data() + 4, d.size() - 4))) return false;
      }
      return true;
    } catch (const Error& e) {
      if (e.code() == ErrorCode::io) throw;
      return false;
    }
  }

  // ---- deciding ----

  bool keptForTags(const Track& t) const {
    if (t.handler == "hint") return false;  // points into other tracks' samples by offset
    if (trim_.tags == MovieTags::all) return true;
    if (t.handler == "text" || t.handler == "sbtl" || t.handler == "subt") return false;
    if (t.handler == "meta") return trim_.tags == MovieTags::noLocation && t.placeless;
    return true;
  }

  void decide() {
    std::uint64_t movieEnd = movieDuration_;
    if (movieEnd == 0) {
      for (const Track& t : tracks_) movieEnd = std::max(movieEnd, implicitDuration(t));
    }
    std::uint64_t start = rescale(trim_.start_ms, movieScale_, 1000);
    std::uint64_t end = trim_.end_ms != 0 ? rescale(trim_.end_ms, movieScale_, 1000, true) : kMax;
    if (movieEnd != 0) end = std::min(end, movieEnd);
    if (start >= end) throw Error(ErrorCode::invalidArgument, "the trim holds none of the movie");
    snapToFrames(start, end);
    if (movieEnd != 0) end = std::min(end, movieEnd);

    bool any = false;
    for (Track& t : tracks_) {
      t.keep = keptForTags(t) && cutTrack(t, start, end);
      if (!t.keep) continue;
      for (const std::string& e : t.entries) {
        if (e.size() == 4 && e.compare(0, 3, "enc") == 0) {
          throw Error(ErrorCode::unsupportedOperation, "an encrypted movie cannot be trimmed");
        }
      }
      if (t.external) throw Error(ErrorCode::unsupportedOperation, "a movie whose samples are in another file");
      any = true;
    }
    if (!any) throw Error(ErrorCode::invalidArgument, "the trim holds none of the movie");
  }

  std::uint64_t implicitDuration(const Track& t) const {
    return t.track_duration != 0 ? t.track_duration : rescale(t.media_duration, movieScale_, t.timescale, true);
  }

  // The track's edits, or the one it implies; a last edit of no length runs
  // to the end of the media.
  std::vector<Edit> editsOf(const Track& t) const {
    std::vector<Edit> edits = t.edits;
    if (!t.has_edits) edits.push_back({implicitDuration(t), 0, kRateOne});
    if (!edits.empty()) {
      Edit& e = edits.back();
      if (e.duration == 0 && e.media_time >= 0) {
        const std::uint64_t media = rescale(t.media_duration, movieScale_, t.timescale, true);
        const std::uint64_t skipped = rescale(static_cast<std::uint64_t>(e.media_time), movieScale_, t.timescale);
        e.duration = media > skipped ? media - skipped : 0;
      }
    }
    return edits;
  }

  // The picture a player shows: the first enabled video track with samples.
  const Track* pictureTrack() const {
    const Track* found = nullptr;
    for (const Track& t : tracks_) {
      if (t.handler != "vide" || t.samples == 0) continue;
      if (t.enabled) return &t;
      if (!found) found = &t;
    }
    return found;
  }

  // The composition span [cts, cts + duration) of the picture track's sample
  // shown at media time m, if one is (and the track is not too long to walk).
  std::optional<Span> frameAt(const Track& t, std::uint64_t m) const {
    if (t.samples > kMaxWalked) return std::nullopt;
    std::optional<Span> found;
    std::size_t sr = 0, cr = 0;
    std::uint64_t sleft = t.stts.empty() ? 0 : t.stts[0].count;
    std::uint64_t cleft = t.ctts.empty() ? 0 : t.ctts[0].count;
    std::int64_t dts = 0;
    const auto at = static_cast<std::int64_t>(m);
    for (std::uint64_t i = 0; i < t.samples; ++i) {
      while (sleft == 0 && sr + 1 < t.stts.size()) sleft = t.stts[++sr].count;
      while (cleft == 0 && cr + 1 < t.ctts.size()) cleft = t.ctts[++cr].count;
      const std::int64_t d = std::max<std::int64_t>(t.stts[sr].value, 1);
      const std::int64_t offset =
          t.has_ctts && cleft > 0 ? static_cast<std::int64_t>(static_cast<std::int32_t>(t.ctts[cr].value)) : 0;
      const std::int64_t cts = dts + offset;
      if (cts >= 0 && cts <= at && at < cts + d && (!found || static_cast<std::uint64_t>(cts) > found->begin)) {
        found = Span{static_cast<std::uint64_t>(cts), static_cast<std::uint64_t>(cts + d)};
      }
      dts += t.stts[sr].value;
      if (dts > std::numeric_limits<std::int64_t>::max() / 2) return std::nullopt;
      if (sleft > 0) --sleft;
      if (cleft > 0) --cleft;
    }
    return found;
  }

  // Widens [start, end) to whole pictures: from where the picture showing at
  // start begins to where the one showing just before end ends. Players
  // differ over a picture an edit list cuts into (FFmpeg drops it, AVFoundation
  // shows it), and agree over one it holds whole.
  void snapToFrames(std::uint64_t& start, std::uint64_t& end) {
    const Track* picture = pictureTrack();
    if (!picture) return;
    const std::uint32_t ts = picture->timescale;
    std::uint64_t at = 0;
    bool started = false;
    std::uint64_t newEnd = end;
    for (const Edit& e : editsOf(*picture)) {
      const std::uint64_t edit_end = checkedAdd(at, e.duration);
      if (e.media_time >= 0 && e.rate == kRateOne) {
        const auto media = static_cast<std::uint64_t>(e.media_time);
        if (!started && start >= at && start < edit_end) {
          const std::uint64_t m = checkedAdd(media, rescale(start - at, ts, movieScale_));
          if (const auto frame = frameAt(*picture, m)) {
            const std::uint64_t from = std::max(frame->begin, media);
            start = at + rescale(from - media, movieScale_, ts);
            startFrame_ = from;
          }
        }
        if (end > at && end <= edit_end) {
          const std::uint64_t m = checkedAdd(media, rescale(end - at, ts, movieScale_, true));
          if (m > media) {
            if (const auto frame = frameAt(*picture, m - 1)) {
              const std::uint64_t to =
                  std::min(frame->end, checkedAdd(media, rescale(e.duration, ts, movieScale_, true)));
              if (to > media) newEnd = std::max(end, std::min(edit_end, at + rescale(to - media, movieScale_, ts)));
            }
          }
        }
      }
      if (start >= at && start < edit_end) started = true;
      at = edit_end;
    }
    end = newEnd;
  }

  // Cuts the track's edits to [start, end) of the movie, and finds the run
  // of samples they show. False when it shows nothing there.
  bool cutTrack(Track& t, std::uint64_t start, std::uint64_t end) {
    const std::vector<Edit> edits = editsOf(t);
    std::vector<Span> spans;
    std::uint64_t at = 0;
    for (std::size_t i = 0; i < edits.size() && at < end; ++i) {
      const Edit& e = edits[i];
      const std::uint64_t edit_end = checkedAdd(at, e.duration);
      const std::uint64_t begin = std::max(at, start);
      const std::uint64_t stop = std::min(edit_end, end);
      if (begin < stop) {
        if (e.media_time == -1) {
          t.new_edits.push_back({stop - begin, -1, kRateOne});
        } else {
          if (e.media_time < 0) corrupt("an edit before the media begins");
          if (e.rate != kRateOne) {
            throw Error(ErrorCode::unsupportedOperation, "an edit that changes the movie's speed cannot be trimmed");
          }
          std::uint64_t from =
              checkedAdd(static_cast<std::uint64_t>(e.media_time), rescale(begin - at, t.timescale, movieScale_));
          // The picture track starts on its picture's own time, which the
          // movie's coarser ticks may fall a little short of.
          if (&t == pictureTrack() && begin == start && startFrame_ && *startFrame_ > from &&
              *startFrame_ - from <= rescale(1, t.timescale, movieScale_, true)) {
            from = *startFrame_;
          }
          const std::uint64_t to = checkedAdd(from, rescale(stop - begin, t.timescale, movieScale_, true));
          if (from > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2) ||
              to > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2)) {
            corrupt("a media time out of range");
          }
          t.new_edits.push_back({stop - begin, static_cast<std::int64_t>(from), kRateOne});
          spans.push_back({from, to});
        }
      }
      at = edit_end;
    }
    while (!t.new_edits.empty() && t.new_edits.back().media_time == -1) t.new_edits.pop_back();
    if (spans.empty() || !neededSamples(t, spans)) return false;

    // Decoding starts at a sync sample; sound one sample earlier still, for
    // the overlap its decoder needs before the first sample shown.
    if (t.has_stss) {
      std::uint64_t sync = 0;
      for (const std::uint32_t n : t.stss) {
        if (n >= 1 && n - 1 <= t.first) sync = std::max<std::uint64_t>(sync, n - 1);
      }
      t.first = sync;
    }
    if (t.handler == "soun" && t.first > 0) --t.first;

    // The copy's media time starts at its first sample's decode time.
    const std::uint64_t origin = t.dtsOf(t.first);
    std::int64_t lowest = std::numeric_limits<std::int64_t>::max();
    for (const Edit& e : t.new_edits) {
      if (e.media_time >= 0) lowest = std::min(lowest, e.media_time);
    }
    // A picture shown before it is decoded (a negative composition offset)
    // would need an edit before the media: every offset moves later instead.
    if (lowest < static_cast<std::int64_t>(origin)) {
      if (!t.has_ctts) corrupt("an edit before the media begins");
      t.shift = static_cast<std::int64_t>(origin) - lowest;
      if (t.shift > std::numeric_limits<std::int32_t>::max()) corrupt("a composition offset out of range");
    }
    for (Edit& e : t.new_edits) {
      if (e.media_time >= 0) e.media_time = e.media_time - static_cast<std::int64_t>(origin) + t.shift;
      t.new_track_duration = checkedAdd(t.new_track_duration, e.duration);
    }
    t.new_media_duration = t.durationOf(t.first, t.last);
    return true;
  }

  // The first and last (exclusive) samples, in decode order, shown within
  // any span. False when none is.
  bool neededSamples(Track& t, const std::vector<Span>& spans) {
    std::uint64_t first = kMax;
    std::uint64_t last = 0;
    const auto take = [&](std::uint64_t a, std::uint64_t b) {
      first = std::min(first, a);
      last = std::max(last, b);
    };
    if (!t.has_ctts) {
      // Shown in decode order: each run's samples found by arithmetic.
      std::uint64_t dts = 0;
      std::uint64_t k = 0;
      for (const Run& r : t.stts) {
        if (k >= t.samples) break;
        const std::uint64_t n = std::min<std::uint64_t>(r.count, t.samples - k);
        const std::uint64_t d = r.value;
        for (const Span& s : spans) {
          if (d == 0) {
            if (dts < s.end && dts + 1 > s.begin) take(k, k + n);
            continue;
          }
          const std::uint64_t lo = s.begin > dts ? (s.begin - dts) / d : 0;
          const std::uint64_t hi = s.end > dts ? std::min(n, (s.end - dts) / d + ((s.end - dts) % d != 0)) : 0;
          if (lo < hi) take(k + lo, k + hi);
        }
        dts = checkedAdd(dts, checkedMul(n, d));
        k += n;
      }
    } else {
      if (t.samples > kMaxWalked) throw Error(ErrorCode::dataTooLarge, "too many samples to trim");
      std::size_t sr = 0, cr = 0;
      std::uint64_t sleft = t.stts.empty() ? 0 : t.stts[0].count;
      std::uint64_t cleft = t.ctts.empty() ? 0 : t.ctts[0].count;
      std::int64_t dts = 0;
      for (std::uint64_t i = 0; i < t.samples; ++i) {
        while (sleft == 0 && sr + 1 < t.stts.size()) sleft = t.stts[++sr].count;
        while (cleft == 0 && cr + 1 < t.ctts.size()) cleft = t.ctts[++cr].count;
        const std::int64_t d = t.stts[sr].value;
        const std::int64_t offset =
            cleft > 0 ? static_cast<std::int64_t>(static_cast<std::int32_t>(t.ctts[cr].value)) : 0;
        const std::int64_t cts = dts + offset;
        for (const Span& s : spans) {
          if (cts < static_cast<std::int64_t>(s.end) && cts + std::max<std::int64_t>(d, 1) > static_cast<std::int64_t>(s.begin)) {
            take(i, i + 1);
          }
        }
        dts += d;
        if (dts > std::numeric_limits<std::int64_t>::max() / 2) corrupt("a time out of range");
        if (sleft > 0) --sleft;
        if (cleft > 0) --cleft;
      }
    }
    if (first >= last) return false;
    t.first = first;
    t.last = last;
    return true;
  }

  // ---- the chunks ----

  void plan() {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
      if (tracks_[i].keep) planTrack(i);
    }
    order_.resize(plan_.size());
    for (std::size_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::stable_sort(order_.begin(), order_.end(),
                     [&](std::size_t a, std::size_t b) { return plan_[a].offset < plan_[b].offset; });
  }

  void planTrack(std::size_t index) {
    Track& t = tracks_[index];
    std::uint64_t sample = 0;
    for (std::size_t r = 0; r < t.stsc.size() && sample < t.last; ++r) {
      const ChunkRun& run = t.stsc[r];
      const std::uint64_t firstChunk = run.first_chunk;
      const std::uint64_t endChunk = r + 1 < t.stsc.size() ? t.stsc[r + 1].first_chunk : t.chunks.size() + 1;
      if (firstChunk == 0 || endChunk < firstChunk || endChunk > t.chunks.size() + 1) {
        corrupt("the sample-to-chunk table is out of order");
      }
      if (run.description == 0) corrupt("a chunk with no sample description");
      for (std::uint64_t c = firstChunk; c < endChunk && sample < t.last; ++c) {
        const std::uint64_t n = run.samples;
        const std::uint64_t begin = std::max(sample, t.first);
        const std::uint64_t end = std::min(sample + n, t.last);
        if (begin < end) {
          if (end > t.samples) corrupt("the chunks hold more samples than the track");
          const std::uint64_t at = checkedAdd(t.chunks[c - 1], t.bytesOf(sample, begin));
          const std::uint64_t bytes = t.bytesOf(begin, end);
          if (!src_.contains(at, bytes)) corrupt("a sample lies outside the file");
          plan_.push_back({index, at, bytes, static_cast<std::uint32_t>(end - begin), run.description});
          t.out_chunks.push_back(plan_.size() - 1);
        }
        sample += n;
      }
    }
    if (sample < t.last) corrupt("the chunks hold fewer samples than the track");
  }

  // Where each chunk goes in the copy, from `at`.
  void place(std::uint64_t at) {
    for (const std::size_t i : order_) {
      plan_[i].out = at;
      at = checkedAdd(at, plan_[i].bytes);
    }
  }

  // ---- writing the movie box ----

  Bytes buildMoov() {
    Bytes p;
    for (const Box& b : moovChildren_) {
      if (b.type == "mvhd") {
        append(p, header(b, false, movieDuration()));
      } else if (b.type == "trak") {
        for (Track& t : tracks_) {
          if (t.trak.offset == b.offset && t.keep) append(p, buildTrak(t));
        }
      } else if (b.type == "meta") {
        append(p, filterMeta(b));
      } else if (b.type == "udta") {
        append(p, filterUdta(b));
      } else if (b.type == "uuid" || b.type == "XMP_") {
        if (trim_.tags == MovieTags::all) append(p, whole(src_, b));
      } else {
        append(p, whole(src_, b));
      }
    }
    return box("moov", p);
  }

  std::uint64_t movieDuration() const {
    std::uint64_t d = 0;
    for (const Track& t : tracks_) {
      if (t.keep) d = std::max(d, t.new_track_duration);
    }
    return d;
  }

  // An mvhd, mdhd or tkhd with its duration replaced, and its times cleared
  // when no tag goes along.
  Bytes header(const Box& b, bool tkhd, std::uint64_t duration) {
    Bytes d = whole(src_, b);
    const std::size_t h = static_cast<std::size_t>(b.payload - b.offset);
    if (d.size() < h + 1) corrupt("truncated header");
    const bool wide = d[h] == 1;
    const std::size_t width = wide ? 8 : 4;
    if (trim_.tags == MovieTags::none) {
      putBe(d, h + 4, 0, width);
      putBe(d, h + 4 + width, 0, width);
    }
    // tkhd: times, track id, reserved, duration; the others: times, timescale, duration.
    putBe(d, h + 4 + 2 * width + (tkhd ? 8 : 4), duration, width);
    return d;
  }

  Bytes buildTrak(Track& t) {
    Bytes p;
    bool edited = false;
    const auto edits = [&] {
      if (edited) return;
      edited = true;
      append(p, editBox(t));
    };
    for (const Box& b : boxes(src_, t.trak.payload, t.trak.end, budget_)) {
      if (b.type == "tkhd") {
        append(p, header(b, true, t.new_track_duration));
        edits();
      } else if (b.type == "edts") {
        edits();
      } else if (b.type == "tref") {
        append(p, filterTref(b));
      } else if (b.type == "mdia") {
        append(p, buildMdia(b, t));
      } else if (b.type == "meta") {
        append(p, filterMeta(b));
      } else if (b.type == "udta") {
        append(p, filterUdta(b));
      } else {
        append(p, whole(src_, b));
      }
    }
    return box("trak", p);
  }

  Bytes editBox(const Track& t) {
    bool wide = false;
    for (const Edit& e : t.new_edits) {
      wide = wide || e.duration > 0xffffffffu || e.media_time > std::numeric_limits<std::int32_t>::max();
    }
    Bytes p;
    versionFlags(p, wide ? 1 : 0, 0);
    appendBe32(p, static_cast<std::uint32_t>(t.new_edits.size()));
    for (const Edit& e : t.new_edits) {
      if (wide) {
        appendBe64(p, e.duration);
        appendBe64(p, static_cast<std::uint64_t>(e.media_time));
      } else {
        appendBe32(p, static_cast<std::uint32_t>(e.duration));
        appendBe32(p, static_cast<std::uint32_t>(static_cast<std::int32_t>(e.media_time)));
      }
      appendBe32(p, static_cast<std::uint32_t>(e.rate));
    }
    return box("edts", box("elst", p));
  }

  // A track reference with the tracks left out taken out of it.
  Bytes filterTref(const Box& tref) {
    Bytes p;
    for (const Box& kind : boxes(src_, tref.payload, tref.end, budget_)) {
      Reader r(load(src_, kind));
      Bytes ids;
      while (r.left() >= 4) {
        const auto id = static_cast<std::uint32_t>(r.read(4));
        bool dropped = false;
        for (const Track& t : tracks_) dropped = dropped || (t.id == id && !t.keep);
        if (!dropped) appendBe32(ids, id);
      }
      if (!ids.empty()) append(p, box(kind.type, ids));
    }
    if (p.empty()) return {};
    return box("tref", p);
  }

  Bytes buildMdia(const Box& mdia, Track& t) {
    Bytes p;
    for (const Box& b : boxes(src_, mdia.payload, mdia.end, budget_)) {
      if (b.type == "mdhd") {
        append(p, header(b, false, t.new_media_duration));
      } else if (b.type == "minf") {
        Bytes info;
        for (const Box& c : boxes(src_, b.payload, b.end, budget_)) {
          append(info, c.type == "stbl" ? buildStbl(c, t) : whole(src_, c));
        }
        append(p, box("minf", info));
      } else {
        append(p, whole(src_, b));
      }
    }
    return box("mdia", p);
  }

  Bytes buildStbl(const Box& stbl, Track& t) {
    Bytes p;
    std::size_t grouping = 0;
    for (const Box& b : boxes(src_, stbl.payload, stbl.end, budget_)) {
      const std::string& type = b.type;
      if (type == "stts") {
        append(p, runsBox("stts", 0, cut(t.stts, t.first, t.last)));
      } else if (type == "ctts") {
        append(p, cttsBox(t));
      } else if (type == "cslg") {
        if (t.has_ctts) append(p, cslgBox(t));
      } else if (type == "stss") {
        append(p, numbersBox("stss", cutNumbers(t.stss, t.first, t.last)));
      } else if (type == "stps") {
        append(p, numbersBox("stps", cutNumbers(t.stps, t.first, t.last)));
      } else if (type == "stsz" || type == "stz2") {
        Bytes s;
        versionFlags(s, 0, 0);
        appendBe32(s, t.sample_size);
        appendBe32(s, static_cast<std::uint32_t>(t.last - t.first));
        if (t.sample_size == 0) {
          for (std::uint64_t i = t.first; i < t.last; ++i) appendBe32(s, t.sizes[i]);
        }
        append(p, box("stsz", s));
      } else if (type == "stsc") {
        append(p, stscBox(t));
      } else if (type == "stco" || type == "co64") {
        append(p, offsetsBox(t));
      } else if (type == "sdtp") {
        if (t.sdtp.size() >= 4 + t.last) {
          Bytes s(t.sdtp.begin(), t.sdtp.begin() + 4);
          s.insert(s.end(), t.sdtp.begin() + static_cast<std::ptrdiff_t>(4 + t.first),
                   t.sdtp.begin() + static_cast<std::ptrdiff_t>(4 + t.last));
          append(p, box("sdtp", s));
        }
      } else if (type == "sbgp") {
        const Grouping& g = t.groupings[grouping++];
        Bytes s = g.head;
        const auto runs = cut(g.runs, t.first, t.last);
        appendBe32(s, static_cast<std::uint32_t>(runs.size()));
        for (const Run& r : runs) {
          appendBe32(s, r.count);
          appendBe32(s, r.value);
        }
        append(p, box("sbgp", s));
      } else if (type == "subs" || type == "saiz" || type == "saio" || type == "stsh" || type == "padb" ||
                 type == "stdp") {
        // Per-sample tables this does not cut: left out.
      } else {
        append(p, whole(src_, b));
      }
    }
    return box("stbl", p);
  }

  Bytes cttsBox(const Track& t) {
    std::vector<Run> runs = cut(t.ctts, t.first, t.last);
    bool negative = false;
    for (Run& r : runs) {
      const std::int64_t v = static_cast<std::int32_t>(r.value) + t.shift;
      if (v > std::numeric_limits<std::int32_t>::max()) corrupt("a composition offset out of range");
      r.value = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
      negative = negative || v < 0;
    }
    return runsBox("ctts", negative ? 1 : t.ctts_version, runs);
  }

  // The composition-to-decode box, measured again over the samples kept.
  Bytes cslgBox(const Track& t) {
    std::int64_t least = std::numeric_limits<std::int64_t>::max();
    std::int64_t greatest = std::numeric_limits<std::int64_t>::min();
    std::int64_t start = std::numeric_limits<std::int64_t>::max();
    std::int64_t end = std::numeric_limits<std::int64_t>::min();
    const auto stts = cut(t.stts, t.first, t.last);
    const auto ctts = cut(t.ctts, t.first, t.last);
    std::size_t sr = 0, cr = 0;
    std::uint64_t sleft = stts.empty() ? 0 : stts[0].count;
    std::uint64_t cleft = ctts.empty() ? 0 : ctts[0].count;
    std::int64_t dts = 0;
    for (std::uint64_t i = t.first; i < t.last; ++i) {
      while (sleft == 0 && sr + 1 < stts.size()) sleft = stts[++sr].count;
      while (cleft == 0 && cr + 1 < ctts.size()) cleft = ctts[++cr].count;
      const std::int64_t d = stts.empty() ? 0 : stts[sr].value;
      const std::int64_t offset = (cleft > 0 ? static_cast<std::int32_t>(ctts[cr].value) : 0) + t.shift;
      least = std::min(least, offset);
      greatest = std::max(greatest, offset);
      start = std::min(start, dts + offset);
      end = std::max(end, dts + offset + d);
      dts += d;
      if (sleft > 0) --sleft;
      if (cleft > 0) --cleft;
    }
    const std::int64_t shift = least < 0 ? -least : 0;
    const std::int64_t values[5] = {shift, least, greatest, start, end};
    bool wide = false;
    for (const std::int64_t v : values) {
      wide = wide || v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max();
    }
    Bytes p;
    versionFlags(p, wide ? 1 : 0, 0);
    for (const std::int64_t v : values) {
      if (wide) {
        appendBe64(p, static_cast<std::uint64_t>(v));
      } else {
        appendBe32(p, static_cast<std::uint32_t>(static_cast<std::int32_t>(v)));
      }
    }
    return box("cslg", p);
  }

  Bytes stscBox(const Track& t) {
    Bytes entries;
    std::uint32_t count = 0;
    std::uint32_t lastSamples = 0;
    std::uint32_t lastDescription = 0;
    for (std::size_t j = 0; j < t.out_chunks.size(); ++j) {
      const Chunk& c = plan_[t.out_chunks[j]];
      if (j > 0 && c.samples == lastSamples && c.description == lastDescription) continue;
      appendBe32(entries, static_cast<std::uint32_t>(j + 1));
      appendBe32(entries, c.samples);
      appendBe32(entries, c.description);
      lastSamples = c.samples;
      lastDescription = c.description;
      ++count;
    }
    Bytes p;
    versionFlags(p, 0, 0);
    appendBe32(p, count);
    append(p, entries);
    return box("stsc", p);
  }

  Bytes offsetsBox(const Track& t) {
    bool wide = false;
    for (const std::size_t i : t.out_chunks) wide = wide || plan_[i].out > 0xffffffffu;
    Bytes p;
    versionFlags(p, 0, 0);
    appendBe32(p, static_cast<std::uint32_t>(t.out_chunks.size()));
    for (const std::size_t i : t.out_chunks) {
      if (wide) {
        appendBe64(p, plan_[i].out);
      } else {
        appendBe32(p, static_cast<std::uint32_t>(plan_[i].out));
      }
    }
    return box(wide ? "co64" : "stco", p);
  }

  // ---- tags ----

  // A meta box as the tags allow: whole, not at all, or with every key and
  // item that could name a place taken out. One that cannot be read is left
  // out rather than guessed at.
  Bytes filterMeta(const Box& meta) {
    if (trim_.tags == MovieTags::all) return whole(src_, meta);
    if (trim_.tags == MovieTags::none) return {};
    try {
      return metaWithoutPlaces(meta);
    } catch (const Error& e) {
      if (e.code() == ErrorCode::io) throw;
      warn("a meta box left out of a trimmed movie: " + std::string(e.what()));
      return {};
    }
  }

  Bytes metaWithoutPlaces(const Box& meta) {
    // QuickTime's meta holds its boxes directly, an ISO one after a version
    // and flags.
    std::uint64_t first = meta.payload;
    Bytes p;
    if (meta.size() >= 8) {
      std::uint8_t peek[8];
      src_.read(meta.payload, peek, 8);
      if (std::memcmp(peek + 4, "hdlr", 4) != 0) {
        first += 4;
        append(p, peek, 4);
      }
    }
    if (first > meta.end) corrupt("truncated meta box");
    const auto children = boxes(src_, first, meta.end, budget_);
    // QuickTime keys: the kept ones renumbered, and the items with them.
    std::vector<std::uint32_t> renumbered;
    const Box* keys = find(children, "keys");
    Bytes keysBox;
    if (keys) {
      Reader r(load(src_, *keys));
      if (!r.has(8)) corrupt("truncated keys");
      Bytes k(r.data().begin(), r.data().begin() + 4);
      r.skip(4);
      const auto count = r.read(4);
      Bytes entries;
      std::uint32_t kept = 0;
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto size = r.read(4);
        if (size < 8 || size - 4 > r.left()) corrupt("a key runs past its box");
        const std::size_t at = r.pos();
        r.skip(static_cast<std::size_t>(size - 8) + 4);
        const std::string name = toText(r.data().data() + at + 4, static_cast<std::size_t>(size - 8));
        if (namesPlace(name)) {
          renumbered.push_back(0);
          continue;
        }
        renumbered.push_back(++kept);
        appendBe32(entries, static_cast<std::uint32_t>(size));
        append(entries, r.data().data() + at, static_cast<std::size_t>(size - 4));
      }
      appendBe32(k, kept);
      append(k, entries);
      keysBox = box("keys", k);
    }
    for (const Box& b : children) {
      if (b.type == "keys") {
        append(p, keysBox);
      } else if (b.type == "ilst") {
        Bytes items;
        for (const Box& item : boxes(src_, b.payload, b.end, budget_)) {
          Bytes d = whole(src_, item);
          if (keys) {
            const std::uint32_t index = getBe32(d.data() + 4);
            if (index == 0 || index > renumbered.size() || renumbered[index - 1] == 0) continue;
            putBe(d, 4, renumbered[index - 1], 4);
          } else if (item.type == "\xa9xyz" || (item.type == "----" && dashItemNamesPlace(item))) {
            continue;
          }
          append(items, d);
        }
        append(p, box("ilst", items));
      } else if (b.type == "xml " || b.type == "XMP_" || b.type == "uuid") {
        // XMP, which may hold a place.
      } else {
        append(p, whole(src_, b));
      }
    }
    return box("meta", p);
  }

  bool dashItemNamesPlace(const Box& item) {
    for (const Box& part : boxes(src_, item.payload, item.end, budget_)) {
      if (part.type != "name" || part.size() < 4) continue;
      const Bytes d = load(src_, part);
      return namesPlace(toText(d.data() + 4, d.size() - 4));
    }
    return false;
  }

  // User data as the tags allow. Without a location, only what is known to
  // be plain text stays: QuickTime's © items but ©xyz, 3GPP's text boxes but
  // loci, and meta boxes filtered.
  Bytes filterUdta(const Box& udta) {
    if (trim_.tags == MovieTags::all) return whole(src_, udta);
    if (trim_.tags == MovieTags::none) return {};
    static const char* const kText[] = {"name", "titl", "auth", "perf", "gnre", "dscp", "cprt",
                                        "albm", "yrrc", "kywd", "clsf", "rtng", "hnti", "hinf"};
    Bytes p;
    try {
      for (const Box& b : boxes(src_, udta.payload, udta.end, budget_)) {
        bool text = static_cast<unsigned char>(b.type[0]) == 0xa9 && b.type != "\xa9xyz";
        for (const char* k : kText) text = text || b.type == k;
        if (b.type == "meta") {
          append(p, filterMeta(b));
        } else if (text) {
          append(p, whole(src_, b));
        }
      }
    } catch (const Error& e) {
      if (e.code() == ErrorCode::io) throw;
      warn("user data left out of a trimmed movie: " + std::string(e.what()));
      return {};
    }
    if (p.empty()) return {};
    return box("udta", p);
  }

  const InputSource& src_;
  const MovieTrim& trim_;
  std::size_t budget_ = kMaxBoxes;
  std::optional<Box> ftyp_;
  std::optional<Box> moov_;
  std::vector<Box> topKept_;
  bool fragmented_ = false;
  std::vector<Box> moovChildren_;
  std::uint32_t movieScale_ = 0;
  std::uint64_t movieDuration_ = 0;
  std::optional<std::uint64_t> startFrame_;  // the picture track's media time at the start
  std::vector<Track> tracks_;
  std::vector<Chunk> plan_;
  std::vector<std::size_t> order_;  // plan_ by input offset: the order written
};

}  // namespace
}  // namespace detail

TrimmedMovie trimMovie(const InputSource& in, OutputSink& out, const MovieTrim& trim) {
  return detail::Trimmer(in, trim).write(out);
}

TrimmedMovie trimMovie(const std::filesystem::path& in, const std::filesystem::path& out, const MovieTrim& trim) {
  // Written beside the file, then renamed over it, so a failure leaves it as
  // it was.
  static std::atomic<unsigned> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path tmp = out;
  tmp += ".lumenlib-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".tmp";
  try {
    TrimmedMovie result;
    {
      const FileSource source(in);
      FileSink sink(tmp);
      result = trimMovie(source, sink, trim);
      sink.close();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, out, ec);
    if (ec) throw Error(ErrorCode::io, "cannot replace " + detail::pathText(out) + ": " + ec.message());
    return result;
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);
    throw;
  }
}

}  // namespace lumenlib

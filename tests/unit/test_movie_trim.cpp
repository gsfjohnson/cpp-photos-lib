// trimMovie over the fixtures with real sample tables (bframes.mp4,
// edited.mov: tests/data/make_movies.py), each copy read back box by box.
#include "testing.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

using namespace lumenlib;

namespace {

// ---- a copy, read back -------------------------------------------------------------

std::uint32_t be32(const Bytes& d, std::size_t at) {
  return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) | (std::uint32_t{d[at + 2]} << 8) | d[at + 3];
}

std::uint64_t be64(const Bytes& d, std::size_t at) {
  return (std::uint64_t{be32(d, at)} << 32) | be32(d, at + 4);
}

struct Node {
  std::string type;
  std::size_t payload;
  std::size_t end;
};

std::vector<Node> children(const Bytes& d, std::size_t begin, std::size_t end) {
  std::vector<Node> out;
  std::size_t at = begin;
  while (at + 8 <= end) {
    std::uint64_t size = be32(d, at);
    std::size_t header = 8;
    if (size == 1) {
      size = be64(d, at + 8);
      header = 16;
    }
    if (size < header || at + size > end) throw testing::Failure{"a damaged box in a copy"};
    out.push_back({std::string(d.begin() + static_cast<std::ptrdiff_t>(at + 4),
                               d.begin() + static_cast<std::ptrdiff_t>(at + 8)),
                   at + header, static_cast<std::size_t>(at + size)});
    at += static_cast<std::size_t>(size);
  }
  return out;
}

const Node* child(const std::vector<Node>& list, const std::string& type) {
  for (const Node& n : list) {
    if (n.type == type) return &n;
  }
  return nullptr;
}

// The node at a path of box types below `from`, or null.
std::optional<Node> at(const Bytes& d, const Node& from, std::initializer_list<const char*> path) {
  Node node = from;
  for (const char* type : path) {
    const auto list = children(d, node.payload, node.end);
    const Node* next = child(list, type);
    if (!next) return std::nullopt;
    node = *next;
  }
  return node;
}

struct Movie {
  Bytes data;
  Node moov;
  std::vector<Node> tracks;
};

Movie parse(Bytes data) {
  Movie m;
  m.data = std::move(data);
  const auto top = children(m.data, 0, m.data.size());
  const Node* moov = child(top, "moov");
  if (!moov) throw testing::Failure{"a copy with no movie box"};
  m.moov = *moov;
  for (const Node& n : children(m.data, moov->payload, moov->end)) {
    if (n.type == "trak") m.tracks.push_back(n);
  }
  return m;
}

std::uint32_t trackId(const Movie& m, const Node& trak) {
  const Node tkhd = *at(m.data, trak, {"tkhd"});
  return be32(m.data, tkhd.payload + (m.data[tkhd.payload] == 1 ? 20 : 12));
}

const Node* track(const Movie& m, std::uint32_t id) {
  for (const Node& t : m.tracks) {
    if (trackId(m, t) == id) return &t;
  }
  return nullptr;
}

std::optional<Node> table(const Movie& m, const Node& trak, const char* type) {
  return at(m.data, trak, {"mdia", "minf", "stbl", type});
}

// (count, value) pairs of an stts, ctts or sbgp body from `from`.
std::vector<std::pair<std::uint32_t, std::uint32_t>> runsAt(const Bytes& d, std::size_t from) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
  const std::uint32_t n = be32(d, from);
  for (std::uint32_t i = 0; i < n; ++i) out.emplace_back(be32(d, from + 4 + 8 * i), be32(d, from + 8 + 8 * i));
  return out;
}

std::vector<std::uint32_t> numbers(const Movie& m, const Node& trak, const char* type) {
  std::vector<std::uint32_t> out;
  const auto node = table(m, trak, type);
  if (!node) return out;
  const std::uint32_t n = be32(m.data, node->payload + 4);
  for (std::uint32_t i = 0; i < n; ++i) out.push_back(be32(m.data, node->payload + 8 + 4 * i));
  return out;
}

struct EditEntry {
  std::uint64_t duration;
  std::int64_t media_time;
  bool operator==(const EditEntry& o) const { return duration == o.duration && media_time == o.media_time; }
};

std::vector<EditEntry> edits(const Movie& m, const Node& trak) {
  std::vector<EditEntry> out;
  const auto elst = at(m.data, trak, {"edts", "elst"});
  if (!elst) return out;
  const bool wide = m.data[elst->payload] == 1;
  const std::uint32_t n = be32(m.data, elst->payload + 4);
  std::size_t p = elst->payload + 8;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (wide) {
      out.push_back({be64(m.data, p), static_cast<std::int64_t>(be64(m.data, p + 8))});
      p += 20;
    } else {
      out.push_back({be32(m.data, p), static_cast<std::int32_t>(be32(m.data, p + 4))});
      p += 12;
    }
  }
  return out;
}

// Every sample of a track, by its tables, in decode order.
std::vector<Bytes> samples(const Movie& m, const Node& trak) {
  const Bytes& d = m.data;
  std::vector<std::uint32_t> sizes;
  if (const auto stsz = table(m, trak, "stsz")) {
    const std::uint32_t fixed = be32(d, stsz->payload + 4);
    const std::uint32_t n = be32(d, stsz->payload + 8);
    for (std::uint32_t i = 0; i < n; ++i) sizes.push_back(fixed != 0 ? fixed : be32(d, stsz->payload + 12 + 4 * i));
  } else {
    const auto stz2 = *table(m, trak, "stz2");
    const std::uint32_t n = be32(d, stz2.payload + 8);
    for (std::uint32_t i = 0; i < n; ++i) sizes.push_back((d[stz2.payload + 12 + 2 * i] << 8) | d[stz2.payload + 13 + 2 * i]);
  }
  std::vector<std::uint64_t> chunks;
  if (const auto stco = table(m, trak, "stco")) {
    for (const std::uint32_t o : numbers(m, trak, "stco")) chunks.push_back(o);
  } else {
    const auto co64 = *table(m, trak, "co64");
    const std::uint32_t n = be32(d, co64.payload + 4);
    for (std::uint32_t i = 0; i < n; ++i) chunks.push_back(be64(d, co64.payload + 8 + 8 * i));
  }
  const auto stsc = *table(m, trak, "stsc");
  const std::uint32_t runs = be32(d, stsc.payload + 4);
  std::vector<Bytes> out;
  std::size_t sample = 0;
  for (std::uint32_t r = 0; r < runs; ++r) {
    const std::uint32_t first = be32(d, stsc.payload + 8 + 12 * r);
    const std::uint32_t per = be32(d, stsc.payload + 12 + 12 * r);
    const std::uint32_t next = r + 1 < runs ? be32(d, stsc.payload + 8 + 12 * (r + 1))
                                            : static_cast<std::uint32_t>(chunks.size() + 1);
    for (std::uint32_t c = first; c < next; ++c) {
      std::uint64_t offset = chunks[c - 1];
      for (std::uint32_t k = 0; k < per && sample < sizes.size(); ++k, ++sample) {
        if (offset + sizes[sample] > d.size()) throw testing::Failure{"a sample outside the copy"};
        out.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(offset),
                         d.begin() + static_cast<std::ptrdiff_t>(offset + sizes[sample]));
        offset += sizes[sample];
      }
    }
  }
  return out;
}

std::uint64_t mediaDuration(const Movie& m, const Node& trak) {
  const Node mdhd = *at(m.data, trak, {"mdia", "mdhd"});
  return m.data[mdhd.payload] == 1 ? be64(m.data, mdhd.payload + 24) : be32(m.data, mdhd.payload + 16);
}

bool holds(const Bytes& d, const std::string& text) {
  return std::search(d.begin(), d.end(), text.begin(), text.end()) != d.end();
}

Bytes trimmed(const std::string& name, std::uint64_t start_ms, std::uint64_t end_ms,
              MovieTags tags = MovieTags::all, TrimmedMovie* result = nullptr) {
  const MemorySource source(testing::readData(name));
  MemorySink sink;
  MovieTrim trim;
  trim.start_ms = start_ms;
  trim.end_ms = end_ms;
  trim.tags = tags;
  const TrimmedMovie made = trimMovie(source, sink, trim);
  if (result) *result = made;
  return sink.release();
}

// The copy's samples are the original's [first, first + n).
void checkSamples(const Movie& copy, const Movie& original, std::uint32_t id, std::size_t first, std::size_t n) {
  const auto kept = samples(copy, *track(copy, id));
  const auto all = samples(original, *track(original, id));
  CHECK_EQ(kept.size(), n);
  for (std::size_t i = 0; i < kept.size(); ++i) {
    CHECK(kept[i] == all[first + i]);
    CHECK_EQ(static_cast<std::uint32_t>((kept[i][0] << 8) | kept[i][1]), id);
    CHECK_EQ(static_cast<std::size_t>((kept[i][2] << 8) | kept[i][3]), first + i);
  }
}

}  // namespace

// The start falls between key frames, in a stream with B-frames: the copy
// decodes from the sync sample before it, keeps the P-frame the last B-frame
// shown needs, and its edit list starts on the very frame. The range is whole
// frames: 500 ms is inside the frame shown from 480, 1300 inside the one
// shown until 1320.
TEST(movie_trim_bframes) {
  TrimmedMovie made;
  const Movie copy = parse(trimmed("bframes.mp4", 500, 1300, MovieTags::all, &made));
  const Movie original = parse(testing::readData("bframes.mp4"));
  CHECK_EQ(made.duration_ms, std::uint64_t{840});
  CHECK_EQ(copy.tracks.size(), std::size_t{2});

  const Node& video = *track(copy, 1);
  checkSamples(copy, original, 1, 10, 24);
  CHECK(edits(copy, video) == (std::vector<EditEntry>{{840, 2048}}));
  CHECK(numbers(copy, video, "stss") == (std::vector<std::uint32_t>{1, 11, 21}));
  CHECK_EQ(mediaDuration(copy, video), std::uint64_t{24 * 512});
  const auto stts = runsAt(copy.data, table(copy, video, "stts")->payload + 4);
  CHECK(stts == (std::vector<std::pair<std::uint32_t, std::uint32_t>>{{24, 512}}));
  // Composition offsets as they were for samples 10 to 33.
  const auto ctts = runsAt(copy.data, table(copy, video, "ctts")->payload + 4);
  std::vector<std::uint32_t> offsets;
  for (const auto& [n, v] : ctts) offsets.insert(offsets.end(), n, v);
  const int display[10] = {0, 3, 1, 2, 6, 4, 5, 9, 7, 8};
  CHECK_EQ(offsets.size(), std::size_t{24});
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    const std::size_t s = 10 + i;
    CHECK_EQ(offsets[i], static_cast<std::uint32_t>((display[s % 10] + (s / 10) * 10 + 2) * 512 - s * 512));
  }
  const auto sdtp = *table(copy, video, "sdtp");
  CHECK_EQ(sdtp.end - sdtp.payload, std::size_t{4 + 24});
  CHECK_EQ(copy.data[sdtp.payload + 4], std::uint8_t{10});
  const auto sbgp = runsAt(copy.data, table(copy, video, "sbgp")->payload + 8);
  CHECK(sbgp == (std::vector<std::pair<std::uint32_t, std::uint32_t>>{{24, 0}}));

  // Sound from one sample before the first heard, its priming composed in.
  const Node& audio = *track(copy, 2);
  checkSamples(copy, original, 2, 22, 41);
  CHECK(edits(copy, audio) == (std::vector<EditEntry>{{840, 1536}}));

  // Samples lie in one mdat after the movie box, interleaved as they were.
  const auto top = children(copy.data, 0, copy.data.size());
  CHECK_EQ(top.size(), std::size_t{3});
  CHECK_EQ(top[0].type, std::string("ftyp"));
  CHECK_EQ(top[1].type, std::string("moov"));
  CHECK_EQ(top[2].type, std::string("mdat"));

  // The reader agrees, and every tag went along.
  const MovieInfo info = readMovie(MemorySource(copy.data));
  CHECK_EQ(info.brand, std::string("isom"));
  CHECK_EQ(info.duration_ms, std::uint64_t{840});
  CHECK_EQ(info.tracks[0].samples, std::uint64_t{24});
  CHECK_EQ(info.tracks[0].duration_ms, std::uint64_t{24 * 40});
  const MovieInfo before = readMovie(MemorySource(testing::readData("bframes.mp4")));
  CHECK(info.created == before.created);
  CHECK_EQ(info.items.size(), before.items.size());
  CHECK(info.item("com.apple.quicktime.location.ISO6709") != nullptr);
  CHECK(holds(copy.data, "loci"));
}

TEST(movie_trim_to_the_end_and_from_the_start) {
  const Movie original = parse(testing::readData("bframes.mp4"));
  TrimmedMovie made;
  const Movie tail = parse(trimmed("bframes.mp4", 1500, 0, MovieTags::all, &made));
  CHECK_EQ(made.duration_ms, std::uint64_t{520});
  // Shown from display frame 37 (1480 ms); decoded from the sync sample at 30.
  checkSamples(tail, original, 1, 30, 20);
  CHECK(edits(tail, *track(tail, 1)) == (std::vector<EditEntry>{{520, 1024 + 18944 - 15360}}));

  const Movie head = parse(trimmed("bframes.mp4", 0, 400, MovieTags::all, &made));
  CHECK_EQ(made.duration_ms, std::uint64_t{400});
  // Display frames 0-9 are GOP 0; 10 is not shown, so GOP 1 is not needed.
  checkSamples(head, original, 1, 0, 10);
  CHECK(edits(head, *track(head, 1)) == (std::vector<EditEntry>{{400, 1024}}));
  checkSamples(head, original, 2, 0, 20);
  CHECK(edits(head, *track(head, 2)) == (std::vector<EditEntry>{{400, 1024}}));

  // The whole movie, copied, is the movie.
  const Movie whole = parse(trimmed("bframes.mp4", 0, 0, MovieTags::all, &made));
  CHECK_EQ(made.duration_ms, std::uint64_t{2000});
  checkSamples(whole, original, 1, 0, 50);
  checkSamples(whole, original, 2, 0, 95);
}

// An edit list already there (an empty edit, then media played out of
// order) is composed with the trim's; every track gets its own.
TEST(movie_trim_composes_an_edit_list) {
  TrimmedMovie made;
  const Movie copy = parse(trimmed("edited.mov", 250, 1700, MovieTags::all, &made));
  const Movie original = parse(testing::readData("edited.mov"));
  CHECK_EQ(made.duration_ms, std::uint64_t{1450});
  CHECK_EQ(copy.tracks.size(), std::size_t{3});
  const Node& video = *track(copy, 1);
  checkSamples(copy, original, 1, 15, 30);
  CHECK(edits(copy, video) == (std::vector<EditEntry>{{150, -1}, {600, 0}, {120, 300}}));
  CHECK(numbers(copy, video, "stss") == (std::vector<std::uint32_t>{1, 16}));
  CHECK(table(copy, video, "co64").has_value() == false);  // small offsets: 32 bits
  checkSamples(copy, original, 2, 0, 1);
  CHECK(edits(copy, *track(copy, 2)) == (std::vector<EditEntry>{{870, 150}}));
  checkSamples(copy, original, 3, 0, 2);  // stz2 read, stsz written
  CHECK(edits(copy, *track(copy, 3)) == (std::vector<EditEntry>{{870, 150}}));
  CHECK(at(copy.data, video, {"tref", "chap"}).has_value());
  const MovieInfo info = readMovie(MemorySource(copy.data));
  CHECK_EQ(info.brand, std::string("qt  "));
  CHECK_EQ(info.duration_ms, std::uint64_t{1450});

  // Trimmed again, the edits compose once more.
  const MemorySource first(copy.data);
  MemorySink sink;
  MovieTrim again;
  again.start_ms = 100;
  again.end_ms = 750;
  made = trimMovie(first, sink, again);
  CHECK_EQ(made.duration_ms, std::uint64_t{650});
  const Movie twice = parse(sink.release());
  CHECK(edits(twice, *track(twice, 1)) == (std::vector<EditEntry>{{90, -1}, {300, 0}}));
  checkSamples(twice, original, 1, 15, 15);
}

// A range inside frames widens to whole ones, on the picture track's own
// times even where the movie's ticks are coarser than its frames.
TEST(movie_trim_whole_frames) {
  TrimmedMovie made;
  // 30 fps at 600 ticks a second, in a movie of 600: frames are 20 ticks.
  const Movie copy = parse(trimmed("edited.mov", 510, 1190, MovieTags::all, &made));
  // 510 ms is 306 ticks, in the second edit (media 300 at 300): media 306,
  // the frame from 300; 1190 ms is 714 ticks, media 714, the frame to 720.
  CHECK(edits(copy, *track(copy, 1)) == (std::vector<EditEntry>{{420, 0}}));
  CHECK_EQ(made.duration_ms, std::uint64_t{700});
  // bframes: 25 fps at 12800, the movie at 1000: a frame is 40 ticks.
  const Movie mp4 = parse(trimmed("bframes.mp4", 1, 39, MovieTags::all, &made));
  CHECK_EQ(made.duration_ms, std::uint64_t{40});
  CHECK(edits(mp4, *track(mp4, 1)) == (std::vector<EditEntry>{{40, 1024}}));
}

// Without a location: no key, item, box or track that could say where.
TEST(movie_trim_without_location) {
  const Movie mp4 = parse(trimmed("bframes.mp4", 500, 1300, MovieTags::noLocation));
  for (const char* place : {"location", "ISO6709", "+59.9139", "loci", "Oslo"}) CHECK(!holds(mp4.data, place));
  const MovieInfo info = readMovie(MemorySource(mp4.data));
  CHECK_EQ(*info.item("com.apple.quicktime.make"), std::string("Apple"));
  CHECK_EQ(*info.item("com.apple.quicktime.creationdate"), std::string("2025-03-01T08:15:00+0100"));
  CHECK_EQ(*info.item("\xc2\xa9nam"), std::string("Fjord"));
  CHECK(info.item("\xc2\xa9xyz") == nullptr);
  CHECK(info.created.has_value());
  CHECK_EQ(info.tracks.size(), std::size_t{2});

  const Movie mov = parse(trimmed("edited.mov", 250, 1750, MovieTags::noLocation));
  for (const char* place : {"location", "ISO6709", "-33.8688"}) CHECK(!holds(mov.data, place));
  // The timed metadata names a location and the chapters are text: both go,
  // and the reference to the chapters with them.
  CHECK_EQ(mov.tracks.size(), std::size_t{1});
  CHECK(!at(mov.data, mov.tracks[0], {"tref"}).has_value());
  const MovieInfo movInfo = readMovie(MemorySource(mov.data));
  CHECK_EQ(*movInfo.item("com.apple.quicktime.model"), std::string("iPhone 12"));
  CHECK_EQ(*movInfo.item("\xc2\xa9" "day"), std::string("2021-03-04T05:06:07Z"));
  CHECK_EQ(movInfo.items.size(), std::size_t{2});
}

TEST(movie_trim_without_tags) {
  for (const char* name : {"bframes.mp4", "edited.mov"}) {
    const Movie copy = parse(trimmed(name, 250, 1250, MovieTags::none));
    const MovieInfo info = readMovie(MemorySource(copy.data));
    CHECK(info.items.empty());
    CHECK(!info.created.has_value());
    CHECK(!info.modified.has_value());
    CHECK(!child(children(copy.data, copy.moov.payload, copy.moov.end), "udta"));
    CHECK(!child(children(copy.data, copy.moov.payload, copy.moov.end), "meta"));
    for (const MovieTrack& t : info.tracks) CHECK(t.handler == "vide" || t.handler == "soun");
  }
}

TEST(movie_trim_refusals) {
  CHECK_THROWS(trimmed("fragmented.mp4", 0, 1000), ErrorCode::unsupportedOperation);
  CHECK_THROWS(trimmed("iphone.mov", 0, 1000), ErrorCode::invalidArgument);  // no samples
  CHECK_THROWS(trimmed("bframes.mp4", 2500, 0), ErrorCode::invalidArgument);
  CHECK_THROWS(trimmed("bframes.mp4", 1000, 1000), ErrorCode::invalidArgument);
  CHECK_THROWS(trimmed("photo.heic", 0, 1000), ErrorCode::unsupportedFormat);
  CHECK_THROWS(trimmed("photo.jpg", 0, 1000), ErrorCode::unsupportedFormat);

  // A sample past the end of the file: the whole movie needs the last ones.
  Bytes cut = testing::readData("bframes.mp4");
  cut.resize(cut.size() - 100);
  const MemorySource source(cut);
  MemorySink sink;
  const MovieTrim trim;
  CHECK_THROWS(trimMovie(source, sink, trim), ErrorCode::corruptData);
}

TEST(movie_trim_progress_and_stopping) {
  const MemorySource source(testing::readData("bframes.mp4"));
  MovieTrim trim;
  trim.start_ms = 200;
  std::uint64_t lastDone = 0;
  std::uint64_t seenTotal = 0;
  int calls = 0;
  trim.progress = [&](std::uint64_t done, std::uint64_t total) {
    CHECK(done >= lastDone);
    lastDone = done;
    seenTotal = total;
    ++calls;
    return true;
  };
  MemorySink sink;
  trimMovie(source, sink, trim);
  CHECK(calls > 2);
  CHECK_EQ(lastDone, seenTotal);

  trim.progress = [&](std::uint64_t done, std::uint64_t) { return done == 0; };
  MemorySink stopped;
  CHECK_THROWS(trimMovie(source, stopped, trim), ErrorCode::cancelled);
}

TEST(movie_trim_between_files) {
  const testing::TempDir dir("lumenlib-movie-trim");
  const auto in = dir.path / "clip.mov";
  {
    FileSink sink(in);
    const Bytes data = testing::readData("edited.mov");
    sink.write(data.data(), data.size());
    sink.close();
  }
  MovieTrim trim;
  trim.start_ms = 500;
  trim.end_ms = 1500;
  const auto out = dir.path / "trimmed.mov";
  CHECK_EQ(trimMovie(in, out, trim).duration_ms, std::uint64_t{1000});
  CHECK_EQ(readMovie(out).duration_ms, std::uint64_t{1000});
  // Over itself, and nothing left beside it.
  CHECK_EQ(trimMovie(out, out, trim).duration_ms, std::uint64_t{500});
  CHECK_EQ(readMovie(out).duration_ms, std::uint64_t{500});
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir.path)) {
    (void)entry;
    ++files;
  }
  CHECK_EQ(files, std::size_t{2});
  // A failure leaves the file there as it was.
  trim.start_ms = 5000;
  trim.end_ms = 0;
  CHECK_THROWS(trimMovie(in, out, trim), ErrorCode::invalidArgument);
  CHECK_EQ(readMovie(out).duration_ms, std::uint64_t{500});
}

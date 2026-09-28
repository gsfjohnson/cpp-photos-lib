// readMovie over the movie fixtures (tests/data/make_movies.py) and movies
// built here box by box.
#include "testing.hpp"

#include <algorithm>
#include <cstring>

using namespace lumenlib;

namespace {

MovieInfo movie(const std::string& name) {
  const MemorySource source(testing::readData(name));
  return readMovie(source);
}

MovieInfo movieOf(const Bytes& data) {
  const MemorySource source(data);
  return readMovie(source);
}

std::string itemOf(const MovieInfo& info, const std::string& name) {
  const std::string* value = info.item(name);
  return value ? *value : "(absent)";
}

// 2023-12-24T21:05:09Z and 2024-06-01T12:30:15Z, in seconds since 1904.
constexpr std::uint64_t kIphoneCreated = 1703451909 + 2082844800ull;
constexpr std::uint64_t kAndroidCreated = 1717245015 + 2082844800ull;

}  // namespace

TEST(movie_iphone_mov) {
  const MovieInfo info = movie("iphone.mov");
  CHECK_EQ(info.brand, std::string("qt  "));
  CHECK(info.created.has_value());
  CHECK_EQ(*info.created, kIphoneCreated);
  CHECK_EQ(movieTimeToUnix(*info.created), std::int64_t{1703451909});
  CHECK_EQ(info.timescale, 600u);
  CHECK_EQ(info.duration_ms, std::uint64_t{1500});
  CHECK(!info.fragmented);
  CHECK_EQ(info.tracks.size(), std::size_t{3});

  const MovieTrack* video = info.firstTrack("vide");
  CHECK(video != nullptr);
  CHECK_EQ(video->id, 1u);
  CHECK(video->enabled);
  CHECK_EQ(video->codec, std::string("hvc1"));
  CHECK_EQ(video->width, 1920u);
  CHECK_EQ(video->height, 1080u);
  CHECK_EQ(video->par_h, 1u);
  CHECK_EQ(video->par_v, 1u);
  const std::int32_t turned[9] = {0, 0x10000, 0, -0x10000, 0, 0, 1080 << 16, 0, 0x40000000};
  CHECK(std::memcmp(video->matrix, turned, sizeof turned) == 0);
  CHECK_EQ(video->timescale, 600u);
  CHECK_EQ(video->duration_ms, std::uint64_t{1500});
  CHECK_EQ(video->samples, std::uint64_t{45});
  CHECK_NEAR(video->min_sample_ms, 1000.0 / 30, 1e-9);

  const MovieTrack* audio = info.firstTrack("soun");
  CHECK(audio != nullptr);
  CHECK_EQ(audio->codec, std::string("mp4a"));
  CHECK_EQ(audio->width, 0u);
  CHECK_EQ(audio->samples, std::uint64_t{65});
  // The short last packet is left out of the shortest.
  CHECK_NEAR(audio->min_sample_ms, 1024 * 1000.0 / 44100, 1e-9);
  const MovieTrack* timed = info.firstTrack("meta");
  CHECK(timed != nullptr);
  CHECK_EQ(timed->codec, std::string("mebx"));
  CHECK(info.firstTrack("text") == nullptr);

  CHECK_EQ(itemOf(info, "com.apple.quicktime.make"), std::string("Apple"));
  CHECK_EQ(itemOf(info, "com.apple.quicktime.model"), std::string("iPhone 15 Pro"));
  CHECK_EQ(itemOf(info, "com.apple.quicktime.creationdate"), std::string("2023-12-24T18:05:09-0300"));
  CHECK_EQ(itemOf(info, "com.apple.quicktime.location.ISO6709"), std::string("-22.9068-043.1729+010.000/"));
  CHECK_EQ(itemOf(info, "com.apple.quicktime.location.accuracy.horizontal"), std::string("14.5"));
  CHECK_EQ(itemOf(info, "com.apple.quicktime.live-photo.auto"), std::string("1"));
  CHECK_EQ(itemOf(info, "com.apple.quicktime.full-frame-rate-playback-intent"), std::string("-1"));
  CHECK_EQ(info.items.size(), std::size_t{8});
  CHECK_EQ(info.items.front().name, std::string("com.apple.quicktime.location.accuracy.horizontal"));
  CHECK(info.items.front().kind == MovieItemKind::key);
}

TEST(movie_android_mp4) {
  const MovieInfo info = movie("android.mp4");
  CHECK_EQ(info.brand, std::string("mp42"));
  CHECK_EQ(*info.created, kAndroidCreated);
  CHECK_EQ(info.duration_ms, std::uint64_t{2000});
  const MovieTrack* video = info.firstTrack("vide");
  CHECK(video != nullptr);
  CHECK_EQ(video->codec, std::string("avc1"));
  CHECK_EQ(video->width, 1280u);
  CHECK_EQ(video->height, 720u);
  CHECK_EQ(video->samples, std::uint64_t{30});
  CHECK_NEAR(video->min_sample_ms, 1000.0 / 30, 1e-9);  // not the short last frame
  CHECK_EQ(video->duration_ms, std::uint64_t{980});
  CHECK_EQ(info.firstTrack("soun")->codec, std::string("mp4a"));
  // moov/meta's keys come before udta's items.
  CHECK_EQ(info.items.size(), std::size_t{5});
  CHECK_EQ(info.items[0].name, std::string("com.android.version"));
  CHECK(info.items[0].kind == MovieItemKind::key);
  CHECK_EQ(itemOf(info, "com.android.manufacturer"), std::string("Google"));
  CHECK_EQ(itemOf(info, "com.android.model"), std::string("Pixel 8"));
  CHECK_EQ(itemOf(info, "com.android.capture.fps"), std::string("30"));
  CHECK_EQ(info.items[4].name, std::string("\xc2\xa9xyz"));
  CHECK(info.items[4].kind == MovieItemKind::userData);
  CHECK_EQ(itemOf(info, "\xc2\xa9xyz"), std::string("+37.7858-122.4064/"));
}

TEST(movie_ffmpeg_use_metadata_tags) {
  const MovieInfo info = movie("ffmpeg.mp4");
  CHECK_EQ(info.brand, std::string("isom"));
  CHECK_EQ(*info.created, kAndroidCreated);
  CHECK_EQ(info.duration_ms, std::uint64_t{1000});
  const MovieTrack* video = info.firstTrack("vide");
  CHECK_EQ(video->width, 160u);
  CHECK_NEAR(1000.0 / video->min_sample_ms, 25.0, 1e-9);
  CHECK_EQ(itemOf(info, "com.apple.quicktime.make"), std::string("Apple"));
  CHECK_EQ(itemOf(info, "location"), std::string("+48.8583+002.2945/"));
  CHECK_EQ(itemOf(info, "encoder"), std::string("Lavf61.7.100"));
  CHECK_EQ(itemOf(info, "major_brand"), std::string("isom"));
}

TEST(movie_mirrored_and_itunes_items) {
  const MovieInfo info = movie("mirrored.mp4");
  CHECK(!info.created.has_value());  // 0: never set
  CHECK(!info.modified.has_value());
  const MovieTrack* video = info.firstTrack("vide");
  CHECK_EQ(video->matrix[0], -0x10000);
  CHECK_EQ(video->matrix[4], 0x10000);
  CHECK_NEAR(1000.0 / video->min_sample_ms, 29.97003, 1e-4);
  CHECK_EQ(itemOf(info, "\xc2\xa9too"), std::string("Lavf61.7.100"));
  CHECK_EQ(itemOf(info, "\xc2\xa9" "day"), std::string("2022-08-14T09:10:11.000000Z"));
  CHECK_EQ(itemOf(info, "\xc2\xa9nam"), std::string("Fjord \xe2\x80\x94 \xf0\x9f\x8c\x8a"));
  CHECK_EQ(itemOf(info, "iTunSMPB"), std::string(" 00000000 00000840"));
  CHECK(info.item("covr") == nullptr);  // a picture is not text
  for (const MovieItem& item : info.items) CHECK(item.kind == MovieItemKind::itunes);
}

TEST(movie_anamorphic_64_bit_encrypted) {
  const MovieInfo info = movie("anamorphic.mov");
  CHECK_EQ(movieTimeToUnix(*info.created), std::int64_t{1614834367});
  CHECK_EQ(info.duration_ms, std::uint64_t{2000});
  const MovieTrack* video = info.firstTrack("vide");
  CHECK_EQ(video->codec, std::string("avc1"));  // the frma's, not "encv"
  CHECK_EQ(video->width, 720u);
  CHECK_EQ(video->height, 576u);
  CHECK_EQ(video->par_h, 64u);
  CHECK_EQ(video->par_v, 45u);
  CHECK_EQ(video->duration_ms, std::uint64_t{2000});
  CHECK_NEAR(video->min_sample_ms, 40.0, 1e-9);
  CHECK(info.items.empty());
}

TEST(movie_fragmented) {
  const MovieInfo info = movie("fragmented.mp4");
  CHECK(info.fragmented);
  CHECK_EQ(info.duration_ms, std::uint64_t{4000});  // mehd's
  const MovieTrack* video = info.firstTrack("vide");
  CHECK_EQ(video->samples, std::uint64_t{0});
  CHECK_EQ(video->min_sample_ms, 0.0);
  CHECK_EQ(video->width, 640u);
}

TEST(movie_from_a_path) {
  const MovieInfo info = readMovie(testing::dataPath("iphone.mov"));
  CHECK_EQ(info.duration_ms, std::uint64_t{1500});
  CHECK_THROWS(readMovie(testing::dataPath("missing.mov")), ErrorCode::io);
}

TEST(movie_not_a_movie) {
  // A photo, a WebM, an AVI, nothing at all.
  CHECK_THROWS(movie("photo.jpg"), ErrorCode::unsupportedFormat);
  CHECK_THROWS(movieOf({0x1a, 0x45, 0xdf, 0xa3, 0x9f, 0x42, 0x86, 0x81, 0x01}), ErrorCode::unsupportedFormat);
  CHECK_THROWS(movieOf(testing::bytesOf(std::string("RIFF\x10\0\0\0AVI LIST", 16))), ErrorCode::unsupportedFormat);
  CHECK_THROWS(movieOf({}), ErrorCode::unsupportedFormat);
  // An ISO file with no movie in it: a HEIF photo.
  CHECK_THROWS(movie("photo.heic"), ErrorCode::unsupportedFormat);
}

TEST(movie_damaged) {
  using testing::box;
  using testing::concat;
  const Bytes ftyp = box("ftyp", testing::bytesOf(std::string("isom\0\0\0\0isom", 12)));
  // An ftyp over bytes that are not boxes.
  Bytes junk = ftyp;
  for (int i = 0; i < 64; ++i) junk.push_back(static_cast<std::uint8_t>(i * 37 + 11));
  CHECK_THROWS(movieOf(junk), ErrorCode::corruptData);
  // A movie box cut short.
  const Bytes whole = testing::readData("android.mp4");
  CHECK_THROWS(movieOf(Bytes(whole.begin(), whole.begin() + 400)), ErrorCode::corruptData);
  // A movie box with no header.
  CHECK_THROWS(movieOf(concat({ftyp, box("moov", box("free", {}))})), ErrorCode::corruptData);
}

TEST(movie_lenient_top_level) {
  // Cut in the data after a whole movie box: what it holds still reads.
  const Bytes whole = testing::readData("android.mp4");
  const Bytes cut(whole.begin(), whole.end() - 10);
  CHECK_EQ(movieOf(cut).duration_ms, std::uint64_t{2000});
}

TEST(movie_damaged_tags_lose_only_themselves) {
  // iphone.mov with its keys' count made huge: the tracks still read.
  Bytes data = testing::readData("iphone.mov");
  const std::string keys = "keys";
  const auto at = std::search(data.begin(), data.end(), keys.begin(), keys.end());
  CHECK(at != data.end());
  const auto count = at + 8;  // past the type, the version and the flags
  count[0] = 0x7f;
  const MovieInfo info = movieOf(data);
  CHECK_EQ(info.tracks.size(), std::size_t{3});
  CHECK(info.items.empty());
}

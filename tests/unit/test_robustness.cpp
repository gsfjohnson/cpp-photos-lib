// Damaged files must fail with lumenlib::Error, never crash, hang or throw
// anything else. Each fixture is truncated and randomly mutated (with a fixed
// seed, so failures reproduce); run under AddressSanitizer in CI.
#include "testing.hpp"

#include <algorithm>
#include <random>

using namespace lumenlib;

namespace {

const char* const kFixtures[] = {"photo.jpg",     "motorola.jpg", "multi.mpo",  "photo.png",  "photo.webp",
                                 "lossless.webp", "photo.tif",    "photo.heic", "photo.avif", "photo.xmp"};

// Reads, and when possible writes, the data. Only lumenlib::Error may escape.
void exercise(Bytes data) {
  try {
    auto file = ImageFile::open(std::move(data));
    file->load();
    (void)readPhotoInfo(*file);
    for (const auto& e : file->exif()) (void)e.describe();
    (void)lensDescription(file->exif());
    if (file->canWrite(MetadataKind::exif) || file->canWrite(MetadataKind::xmp)) {
      file->xmp().setText("xmp:Rating", "3");
      if (file->canWrite(MetadataKind::exif)) file->exif().remove("gps.GPSLatitude");
      MemorySink sink;
      file->saveTo(sink);
    }
  } catch (const Error&) {
  }
}

}  // namespace

TEST(robustness_truncation) {
  for (const char* name : kFixtures) {
    const Bytes original = testing::readData(name);
    for (std::size_t n = 0; n < original.size(); n += std::max<std::size_t>(1, original.size() / 97)) {
      exercise(Bytes(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(n)));
    }
  }
}

TEST(robustness_mutation) {
  std::mt19937 rng(12345);
  for (const char* name : kFixtures) {
    const Bytes original = testing::readData(name);
    for (int round = 0; round < 300; ++round) {
      Bytes data = original;
      const int edits = 1 + static_cast<int>(rng() % 8);
      for (int e = 0; e < edits; ++e) {
        const std::size_t at = rng() % data.size();
        switch (rng() % 4) {
          case 0:
            data[at] = static_cast<std::uint8_t>(rng());
            break;
          case 1:
            data[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            break;
          case 2:
            data[at] = 0xff;
            break;
          default:
            data[at] = 0;
            break;
        }
      }
      exercise(std::move(data));
    }
  }
  CHECK(true);
}

namespace {

const char* const kMovies[] = {"iphone.mov",     "android.mp4",    "ffmpeg.mp4", "mirrored.mp4",
                               "anamorphic.mov", "fragmented.mp4", "bframes.mp4", "edited.mov"};

// Reads the movie and trims it three ways. Only lumenlib::Error may escape.
void exerciseMovie(const Bytes& data) {
  const MemorySource source(data);
  try {
    const MovieInfo info = readMovie(source);
    (void)info.firstTrack("vide");
    (void)info.item("com.apple.quicktime.make");
  } catch (const Error&) {
  }
  for (const MovieTags tags : {MovieTags::all, MovieTags::noLocation, MovieTags::none}) {
    try {
      MovieTrim trim;
      trim.start_ms = tags == MovieTags::all ? 0 : 450;
      trim.end_ms = tags == MovieTags::none ? 0 : 1300;
      trim.tags = tags;
      MemorySink sink;
      trimMovie(source, sink, trim);
      // What it writes, it reads.
      const MemorySource copy(sink.release());
      (void)readMovie(copy);
    } catch (const Error&) {
    }
  }
}

}  // namespace

TEST(robustness_movie_truncation) {
  for (const char* name : kMovies) {
    const Bytes original = testing::readData(name);
    for (std::size_t n = 0; n < original.size(); ++n) {
      exerciseMovie(Bytes(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(n)));
    }
  }
  CHECK(true);
}

TEST(robustness_movie_mutation) {
  std::mt19937 rng(61);
  for (const char* name : kMovies) {
    const Bytes original = testing::readData(name);
    for (int round = 0; round < 2000; ++round) {
      Bytes data = original;
      const int edits = 1 + static_cast<int>(rng() % 6);
      for (int e = 0; e < edits; ++e) {
        const std::size_t at = rng() % data.size();
        switch (rng() % 4) {
          case 0:
            data[at] = static_cast<std::uint8_t>(rng());
            break;
          case 1:
            data[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            break;
          case 2:
            data[at] = 0xff;
            break;
          default:
            data[at] = 0;
            break;
        }
      }
      exerciseMovie(data);
    }
  }
  CHECK(true);
}

// A minimal test harness, so the tests build wherever the library does.
#pragma once

#include <photos/photos.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct Test {
  const char* name;
  void (*fn)();
};

inline std::vector<Test>& registry() {
  static std::vector<Test> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure {
  std::string message;
};

inline int& checks() {
  static int n = 0;
  return n;
}

template <typename T>
std::string show(const T& v) {
  std::ostringstream os;
  if constexpr (std::is_same_v<T, std::uint8_t>) {
    os << static_cast<int>(v);
  } else {
    os << v;
  }
  return os.str();
}

// Non-ASCII values the test images hold, spelled out in UTF-8.
inline const std::string kKobenhavn =
    "K\xc3\xb8"
    "benhavn";                                         // Kobenhavn with o-slash
inline const std::string kAlesund = "\xc3\x85lesund";  // Alesund with A-ring

inline std::filesystem::path dataPath(const std::string& name) {
  return std::filesystem::path(PHOTOS_TEST_DATA_DIR) / name;
}

inline photos::Bytes readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Failure{"cannot read " + path.string()};
  return photos::Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline photos::Bytes readData(const std::string& name) { return readFile(dataPath(name)); }

// Opens and reads a fixture from memory.
inline std::unique_ptr<photos::Image> load(const std::string& name) {
  auto image = photos::Image::open(readData(name));
  image->readMetadata();
  return image;
}

// Writes the image to memory and reads the result back.
inline std::unique_ptr<photos::Image> roundTrip(const photos::Image& image) {
  photos::MemorySink sink;
  image.writeMetadata(sink);
  auto again = photos::Image::open(sink.release());
  again->readMetadata();
  return again;
}

// The TIFF block the library writes for the Exif data (taken from the APP1
// segment of a minimal JPEG, so only the public API is used).
inline photos::Bytes tiffOf(const photos::ExifData& exif) {
  const photos::Bytes bare = {0xff, 0xd8, 0xff, 0xda, 0x00, 0x02, 0x00, 0xff, 0xd9};
  auto image = photos::Image::open(bare);
  image->readMetadata();
  image->exifData() = exif;
  photos::MemorySink sink;
  image->writeMetadata(sink);
  const photos::Bytes& j = sink.data();
  if (j.size() < 12 || j[2] != 0xff || j[3] != 0xe1) return {};
  const std::size_t n = static_cast<std::size_t>(j[4] << 8 | j[5]) - 8;
  return photos::Bytes(j.begin() + 12, j.begin() + 12 + static_cast<std::ptrdiff_t>(n));
}

// An ISO BMFF box.
inline photos::Bytes box(const std::string& type, const photos::Bytes& payload) {
  const std::size_t n = payload.size() + 8;
  photos::Bytes b = {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
                     static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)};
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

inline photos::Bytes concat(std::initializer_list<photos::Bytes> parts) {
  photos::Bytes out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

inline photos::Bytes bytesOf(const std::string& s) { return photos::Bytes(s.begin(), s.end()); }

}  // namespace testing

#define TEST(name)                                                \
  static void name();                                             \
  static const testing::Registrar name##_registrar(#name, &name); \
  static void name()

#define CHECK(cond)                                                                                           \
  do {                                                                                                        \
    ++testing::checks();                                                                                      \
    if (!(cond)) throw testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #cond}; \
  } while (0)

#define CHECK_EQ(a, b)                                                                                         \
  do {                                                                                                         \
    ++testing::checks();                                                                                       \
    const auto va_ = (a);                                                                                      \
    const auto vb_ = (b);                                                                                      \
    if (!(va_ == vb_)) {                                                                                       \
      throw testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #a " == " #b " (" + \
                             testing::show(va_) + " vs " + testing::show(vb_) + ")"};                          \
    }                                                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                 \
  do {                                                                                                        \
    ++testing::checks();                                                                                      \
    const double va_ = (a), vb_ = (b);                                                                        \
    if (!(va_ - vb_ <= (eps) && vb_ - va_ <= (eps))) {                                                        \
      throw testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #a " ~ " #b " (" + \
                             testing::show(va_) + " vs " + testing::show(vb_) + ")"};                         \
    }                                                                                                         \
  } while (0)

#define CHECK_THROWS(expr, errorCode)                                                                               \
  do {                                                                                                              \
    ++testing::checks();                                                                                            \
    bool thrown_ = false;                                                                                           \
    try {                                                                                                           \
      (void)(expr);                                                                                                 \
    } catch (const photos::Error& e_) {                                                                             \
      thrown_ = e_.code() == (errorCode);                                                                           \
      if (!thrown_) {                                                                                               \
        throw testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +                             \
                               ": " #expr " threw the wrong error: " + e_.what()};                                  \
      }                                                                                                             \
    }                                                                                                               \
    if (!thrown_) {                                                                                                 \
      throw testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expr " did not throw"}; \
    }                                                                                                               \
  } while (0)

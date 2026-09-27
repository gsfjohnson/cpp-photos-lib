// A minimal test harness, so the tests build wherever the library does.
#pragma once

#include <lumenlib/lumenlib.hpp>

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
  return std::filesystem::path(LUMENLIB_TEST_DATA_DIR) / name;
}

inline lumenlib::Bytes readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Failure{"cannot read " + path.string()};
  return lumenlib::Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline lumenlib::Bytes readData(const std::string& name) { return readFile(dataPath(name)); }

// Opens and reads a fixture from memory.
inline std::unique_ptr<lumenlib::ImageFile> load(const std::string& name) {
  auto file = lumenlib::ImageFile::open(readData(name));
  file->load();
  return file;
}

// Writes the file to memory and reads the result back.
inline std::unique_ptr<lumenlib::ImageFile> roundTrip(const lumenlib::ImageFile& file) {
  lumenlib::MemorySink sink;
  file.saveTo(sink);
  auto again = lumenlib::ImageFile::open(sink.release());
  again->load();
  return again;
}

// The TIFF block the library writes for the Exif data.
inline lumenlib::Bytes tiffOf(const lumenlib::ExifMetadata& exif) { return exif.encode(); }

// A scratch directory of the test's own, removed with everything in it.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string& name) : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

// An ISO BMFF box.
inline lumenlib::Bytes box(const std::string& type, const lumenlib::Bytes& payload) {
  const std::size_t n = payload.size() + 8;
  lumenlib::Bytes b = {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
                       static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)};
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

inline lumenlib::Bytes concat(std::initializer_list<lumenlib::Bytes> parts) {
  lumenlib::Bytes out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

inline lumenlib::Bytes bytesOf(const std::string& s) { return lumenlib::Bytes(s.begin(), s.end()); }

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
    } catch (const lumenlib::Error& e_) {                                                                           \
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

// Real camera files, when LUMENLIB_SAMPLES names a folder of them (they are
// too large for the tree): every file must read, its maker note decode when
// it has a known one, and a TIFF-based raw must survive an edit with its
// image data, maker note and later IFDs untouched. Each file's camera and
// lens are printed for a person to check.
#include "testing.hpp"

#include <cstdlib>
#include <cstring>

using namespace lumenlib;

namespace {

// Every byte range the file's IFD0 strips and tiles cover, concatenated.
Bytes imageData(const ImageFile& file) {
  Bytes out;
  for (const auto& [o, c] : {std::make_pair("ifd0.StripOffsets", "ifd0.StripByteCounts"),
                             std::make_pair("ifd0.TileOffsets", "ifd0.TileByteCounts")}) {
    const auto* offsets = file.exif().find(o);
    const auto* counts = file.exif().find(c);
    if (!offsets || !counts) continue;
    for (std::size_t i = 0; i < offsets->count() && i < counts->count(); ++i) {
      const auto at = static_cast<std::uint64_t>(offsets->asInt(i));
      const auto n = static_cast<std::size_t>(counts->asInt(i));
      if (!file.source().contains(at, n)) continue;
      const Bytes b = file.source().readBytes(at, n);
      out.insert(out.end(), b.begin(), b.end());
    }
  }
  return out;
}

}  // namespace

TEST(samples_read_and_rewrite) {
  const char* dir = std::getenv("LUMENLIB_SAMPLES");
  if (!dir || !*dir) {
    std::cout << "  (LUMENLIB_SAMPLES not set: real camera files skipped)\n";
    return;
  }
  std::size_t seen = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(dir))) {
    if (!entry.is_regular_file()) continue;
    const auto path = entry.path();
    FileFormat format = FileFormat::unknown;
    try {
      format = detectFormat(path);
    } catch (const Error&) {
    }
    if (format == FileFormat::unknown) continue;
    ++seen;
    const Bytes original = testing::readFile(path);
    auto file = ImageFile::open(Bytes(original));
    file->load();
    const PhotoInfo info = readPhotoInfo(*file);
    const MakerNote* note = file->exif().makerNote();
    std::cout << "  " << path.filename().string() << ": " << formatName(format) << ", " << info.cameraMake << " "
              << info.cameraModel << ", lens \"" << info.lensModel << "\", maker note "
              << (note ? makerNoteFormatName(note->format()) : "-") << " (" << (note ? note->entries().size() : 0)
              << " entries)\n";
    CHECK(!info.cameraMake.empty());
    // The makers whose notes this library reads.
    for (const char* maker : {"Canon", "NIKON", "SONY", "OLYMPUS", "Panasonic", "PENTAX", "FUJIFILM"}) {
      if (info.cameraMake.compare(0, std::strlen(maker), maker) == 0 && file->exif().contains("exif.MakerNote")) {
        CHECK(note != nullptr);
      }
    }
    if (!file->canWrite(MetadataKind::exif)) continue;

    const Bytes pixels = imageData(*file);
    file->exif().setText("ifd0.Artist", "lumenlib test");
    file->exif().removeIf([](const ExifEntry& e) { return e.ifd() == Ifd::gps; });
    file->xmp().setText("xmp:Rating", "3");
    MemorySink sink;
    file->saveTo(sink);
    auto again = ImageFile::open(sink.release());
    again->load();
    CHECK_EQ(again->exif().find("ifd0.Artist")->text(), "lumenlib test");
    CHECK_EQ(*again->xmp().text("xmp:Rating"), "3");
    CHECK_EQ(readPhotoInfo(*again).cameraModel, info.cameraModel);
    CHECK_EQ(readPhotoInfo(*again).lensModel, info.lensModel);
    CHECK(imageData(*again) == pixels);
    if (note) {
      CHECK(again->exif().makerNote() != nullptr);
      CHECK_EQ(again->exif().makerNote()->entries().size(), note->entries().size());
    }
    // Everything the file had after IFD0 is still where it was.
    CHECK(again->exif().thumbnail() == file->exif().thumbnail());
  }
  CHECK(seen > 0);
}

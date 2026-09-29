// Feeds arbitrary bytes to ImageFile: detection, every reader, the maker
// note decoders, the PhotoInfo layer and, where the format is writable, the
// writer; and readHeifImage. Only lumenlib::Error may come out.
#include <lumenlib/lumenlib.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    const lumenlib::MemorySource source(lumenlib::Bytes(data, data + size));
    (void)lumenlib::readHeifImage(source);
  } catch (const lumenlib::Error&) {
  }
  try {
    auto file = lumenlib::ImageFile::open(lumenlib::Bytes(data, data + size));
    file->load();
    (void)lumenlib::readPhotoInfo(*file);
    for (const auto& e : file->exif()) (void)e.describe();
    if (file->canWrite(lumenlib::MetadataKind::xmp)) {
      file->xmp().setText("xmp:Rating", "3");
      file->exif().remove("gps.GPSLatitude");
      lumenlib::MemorySink sink;
      file->saveTo(sink);
      auto again = lumenlib::ImageFile::open(sink.release());
      again->load();
    }
  } catch (const lumenlib::Error&) {
  }
  return 0;
}

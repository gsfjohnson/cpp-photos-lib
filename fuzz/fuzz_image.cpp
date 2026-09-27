// Feeds arbitrary bytes to Image: detection, every reader, the PhotoInfo
// layer and, where the format is writable, the writer. Only photos::Error may
// come out.
#include <photos/photos.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    auto image = photos::Image::open(photos::Bytes(data, data + size));
    image->readMetadata();
    (void)photos::readPhotoInfo(*image);
    if (image->canWrite(photos::MetadataKind::xmp)) {
      image->xmpData()["Xmp.xmp.Rating"] = "3";
      photos::MemorySink sink;
      image->writeMetadata(sink);
      auto again = photos::Image::open(sink.release());
      again->readMetadata();
    }
  } catch (const photos::Error&) {
  }
  return 0;
}

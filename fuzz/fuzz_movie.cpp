// Feeds arbitrary bytes to readMovie. Only lumenlib::Error may come out.
#include <lumenlib/lumenlib.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    const lumenlib::MemorySource source(lumenlib::Bytes(data, data + size));
    const lumenlib::MovieInfo info = lumenlib::readMovie(source);
    (void)info.firstTrack("vide");
    (void)info.item("com.apple.quicktime.location.ISO6709");
  } catch (const lumenlib::Error&) {
  }
  return 0;
}

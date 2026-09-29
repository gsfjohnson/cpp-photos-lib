// Feeds arbitrary bytes to readMovie and trimMovie. Only lumenlib::Error may
// come out.
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
  // And trims it, the tags chosen by the data's length.
  try {
    const lumenlib::MemorySource source(lumenlib::Bytes(data, data + size));
    lumenlib::MovieTrim trim;
    trim.start_ms = size % 700;
    trim.end_ms = size % 3 == 0 ? 0 : 800 + size % 900;
    trim.tags = static_cast<lumenlib::MovieTags>(size % 3);
    lumenlib::MemorySink sink;
    lumenlib::trimMovie(source, sink, trim);
    const lumenlib::MemorySource copy(sink.release());
    (void)lumenlib::readMovie(copy);
  } catch (const lumenlib::Error&) {
  }
  return 0;
}

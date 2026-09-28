// Links every library in a lumen-ios-deps slice the way Lumen uses it: exiv2
// XMP and a PNG in memory, a WebP encode (with sharp YUV) that is demuxed and
// muxed back, an ONNX Runtime session with the Core ML provider, and lumenlib
// adding XMP and an ICC profile (deflated with zlib) to a PNG in memory. Run
// on a device or simulator it prints each library's version and what it did;
// the build only links it.
#include <coreml_provider_factory.h>
#include <exiv2/exiv2.hpp>
#include <lumenlib/lumenlib.hpp>
#include <onnxruntime_cxx_api.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

bool exiv2Round() {
  Exiv2::XmpParser::initialize();
  Exiv2::XmpData xmp;
  xmp["Xmp.dc.subject"] = "lumen";
  std::string packet;
  if (Exiv2::XmpParser::encode(packet, xmp) != 0) {
    return false;
  }
  Exiv2::XmpData decoded;
  if (Exiv2::XmpParser::decode(decoded, packet) != 0) {  // expat
    return false;
  }
  try {
    auto png = Exiv2::ImageFactory::create(Exiv2::ImageType::png);  // zlib
    png->setXmpData(decoded);
    png->writeMetadata();
    std::printf("exiv2 %s: %zu-byte PNG with XMP\n", Exiv2::versionString().c_str(),
                static_cast<size_t>(png->io().size()));
  } catch (const Exiv2::Error& e) {
    std::printf("exiv2: %s\n", e.what());
    return false;
  }
  Exiv2::XmpParser::terminate();
  return true;
}

bool webpRound() {
  constexpr int kSide = 16;
  std::array<uint8_t, kSide * kSide * 3> rgb{};
  for (size_t i = 0; i < rgb.size(); ++i) {
    rgb[i] = static_cast<uint8_t>(i * 7);
  }
  WebPConfig config;
  WebPPicture picture;
  if (!WebPConfigInit(&config) || !WebPPictureInit(&picture)) {
    return false;
  }
  config.use_sharp_yuv = 1;  // libsharpyuv
  picture.use_argb = 1;
  picture.width = kSide;
  picture.height = kSide;
  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  picture.writer = WebPMemoryWrite;
  picture.custom_ptr = &writer;
  bool ok = WebPPictureImportRGB(&picture, rgb.data(), kSide * 3) && WebPEncode(&config, &picture);
  WebPPictureFree(&picture);

  int width = 0, height = 0;
  const WebPData data{writer.mem, writer.size};
  ok = ok && WebPGetInfo(data.bytes, data.size, &width, &height) && width == kSide;
  if (WebPDemuxer* demux = WebPDemux(&data)) {
    WebPDemuxDelete(demux);
  } else {
    ok = false;
  }
  if (WebPMux* mux = WebPMuxCreate(&data, 0)) {
    WebPMuxDelete(mux);
  } else {
    ok = false;
  }
  const int v = WebPGetEncoderVersion();
  std::printf("libwebp %d.%d.%d: %zu-byte %dx%d WebP\n", v >> 16, (v >> 8) & 0xff, v & 0xff, writer.size,
              width, height);
  WebPMemoryWriterClear(&writer);
  return ok;
}

bool onnxRound() {
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "lumen-ios-deps");
  Ort::SessionOptions options;
  if (OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_CoreML(options, 0)) {
    std::printf("onnxruntime: Core ML: %s\n", Ort::GetApi().GetErrorMessage(status));
    Ort::GetApi().ReleaseStatus(status);
    return false;
  }
  // Not a model: the session loader rejects it, which is the expected outcome.
  constexpr std::array<uint8_t, 4> kNotAModel{0x08, 0x07, 0x12, 0x00};
  try {
    Ort::Session session(env, kNotAModel.data(), kNotAModel.size(), options);
    return false;
  } catch (const Ort::Exception& e) {
    std::printf("onnxruntime %s: rejected a non-model (%s)\n", Ort::GetVersionString().c_str(), e.what());
  }
  return true;
}

bool lumenlibRound() {
  // A 1x1 RGBA PNG.
  const lumenlib::Bytes png{
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
      0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
      0x42, 0x60, 0x82};
  try {
    auto file = lumenlib::ImageFile::open(png);
    file->load();
    file->xmp().setItems("dc:subject", {"lumen"});
    file->setIccProfile(lumenlib::Bytes(512, 0x2a));  // an iCCP chunk: deflate
    file->save();
    auto again = lumenlib::ImageFile::open(*file->buffer());
    again->load();  // inflate
    std::printf("lumenlib %s: %zu-byte PNG with XMP and a %zu-byte ICC profile\n", LUMENLIB_VERSION_STRING,
                file->buffer()->size(), again->iccProfile().size());
    return again->xmp().text("dc:subject") == "lumen" && again->iccProfile().size() == 512;
  } catch (const lumenlib::Error& e) {
    std::printf("lumenlib: %s\n", e.what());
    return false;
  }
}

}  // namespace

int main() {
  const bool ok = exiv2Round() & webpRound() & onnxRound() & lumenlibRound();
  std::printf("%s\n", ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}

// photos-meta: print and edit photo metadata from the command line.
//
//   photos-meta [-p a|e|i|x|c|s|X] file...     print (default: a)
//   photos-meta -M "set KEY VALUE" [-M ...] file...
//   photos-meta -M "del KEY" file...
//
// Print modes: a all, e Exif, i IPTC, x XMP, c comment, s summary (what a
// photo album shows), X the raw XMP packet. The listing is laid out like
// `exiv2 -pa`: key, type, count, value.
#include <photos/photos.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: photos-meta [-p a|e|i|x|c|s|X] [-M \"set KEY VALUE\" | -M \"del KEY\"]... file...\n"
               "photos %s\n",
               PHOTOS_VERSION_STRING);
}

void line(const std::string& key, const char* type, std::size_t count, const std::string& value) {
  std::printf("%-44s %-10s %4zu  %s\n", key.c_str(), type, count, value.c_str());
}

std::string printable(const photos::ExifDatum& d) {
  // Long binary values are summarised, as exiv2 does for maker notes.
  if ((d.typeId() == photos::TypeId::undefined || d.typeId() == photos::TypeId::unsignedByte) && d.count() > 64) {
    return "(Binary value suppressed)";
  }
  return d.toString();
}

void printExif(const photos::Image& image) {
  for (const auto& d : image.exifData()) line(d.key(), photos::typeName(d.typeId()), d.count(), printable(d));
  if (!image.exifData().thumbnail().empty()) {
    std::printf("%-44s %-10s %4zu  %s\n", "(thumbnail)", "JPEG", image.exifData().thumbnail().size(), "bytes");
  }
}

void printIptc(const photos::Image& image) {
  for (const auto& d : image.iptcData()) line(d.key(), "String", d.toString().size(), d.toString());
}

void printXmp(const photos::Image& image) {
  for (const auto& d : image.xmpData()) {
    const auto& v = d.value();
    const std::size_t count = v.isArray()                                     ? v.items().size()
                              : v.kind() == photos::XmpValue::Kind::langAlt   ? v.languages().size()
                              : v.kind() == photos::XmpValue::Kind::structure ? 0
                                                                              : v.text().size();
    line(d.key(), photos::toString(d.kind()), count, d.toString());
  }
}

template <typename T>
void field(const char* name, const std::optional<T>& v) {
  if (v) std::cout << "  " << name << ": " << *v << "\n";
}

void field(const char* name, const std::string& v) {
  if (!v.empty()) std::cout << "  " << name << ": " << v << "\n";
}

void printSummary(const photos::Image& image) {
  const auto info = photos::readPhotoInfo(image);
  std::cout << "  type: " << photos::toString(image.type()) << "\n";
  if (info.width) std::cout << "  size: " << info.width << " x " << info.height << "\n";
  if (info.dateTaken) std::cout << "  date taken: " << info.dateTaken->toIso8601() << "\n";
  field("orientation", info.orientation);
  field("camera", (info.cameraMake + " " + info.cameraModel).substr(info.cameraMake.empty() ? 1 : 0));
  field("lens", info.lensModel);
  field("exposure time", info.exposureTime);
  field("f-number", info.fNumber);
  field("focal length", info.focalLength);
  field("ISO", info.iso);
  if (info.gps) {
    std::printf("  GPS: %.6f, %.6f", info.gps->latitude, info.gps->longitude);
    if (info.gps->altitude) std::printf(", %.1f m", *info.gps->altitude);
    std::printf("\n");
  }
  field("title", info.title);
  field("description", info.description);
  if (!info.keywords.empty()) {
    std::cout << "  keywords:";
    for (const auto& k : info.keywords) std::cout << " [" << k << "]";
    std::cout << "\n";
  }
  field("creator", info.creator);
  field("copyright", info.copyright);
  field("rating", info.rating);
}

void modify(photos::Image& image, const std::string& command) {
  const auto space = command.find(' ');
  const std::string verb = command.substr(0, space);
  std::string rest = space == std::string::npos ? std::string() : command.substr(space + 1);
  const auto space2 = rest.find(' ');
  const std::string key = rest.substr(0, space2);
  const std::string value = space2 == std::string::npos ? std::string() : rest.substr(space2 + 1);
  if (key.empty() || (verb != "set" && verb != "del")) {
    throw photos::Error(photos::ErrorCode::invalidArgument, "bad command '" + command + "'");
  }
  const std::string family = key.substr(0, key.find('.'));
  if (verb == "del") {
    if (family == "Exif")
      image.exifData().erase(key);
    else if (family == "Iptc")
      image.iptcData().erase(key);
    else if (family == "Xmp")
      image.xmpData().erase(key);
    else
      throw photos::Error(photos::ErrorCode::invalidArgument, "unknown key family in '" + key + "'");
    return;
  }
  if (family == "Exif")
    image.exifData()[key] = value;
  else if (family == "Iptc")
    image.iptcData()[key] = value;
  else if (family == "Xmp")
    image.xmpData()[key] = value;
  else
    throw photos::Error(photos::ErrorCode::invalidArgument, "unknown key family in '" + key + "'");
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "a";
  std::vector<std::string> commands;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "-p" || arg == "-M") && i + 1 < argc) {
      (arg == "-p" ? mode : commands.emplace_back()) = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      usage();
      return 2;
    } else {
      files.push_back(arg);
    }
  }
  if (files.empty()) {
    usage();
    return 2;
  }

  int status = 0;
  for (const auto& file : files) {
    try {
      auto image = photos::Image::open(file);
      image->readMetadata();
      if (!commands.empty()) {
        for (const auto& c : commands) modify(*image, c);
        image->writeMetadata();
        continue;
      }
      if (files.size() > 1) std::printf("%s:\n", file.c_str());
      if (mode == "a" || mode == "e") printExif(*image);
      if (mode == "a" || mode == "i") printIptc(*image);
      if (mode == "a" || mode == "x") printXmp(*image);
      if ((mode == "a" || mode == "c") && !image->comment().empty()) {
        line("(comment)", "String", image->comment().size(), image->comment());
      }
      if (mode == "s") printSummary(*image);
      if (mode == "X") std::fwrite(image->xmpPacket().data(), 1, image->xmpPacket().size(), stdout);
    } catch (const photos::Error& e) {
      std::fprintf(stderr, "photos-meta: %s: %s\n", file.c_str(), e.what());
      status = 1;
    }
  }
  return status;
}

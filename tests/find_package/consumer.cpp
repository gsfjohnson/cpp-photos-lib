#include <photos/photos.hpp>

#include <iostream>

int main(int argc, char** argv) {
  std::cout << "photos " << PHOTOS_VERSION_STRING << "\n";
  if (argc < 2) return 0;
  try {
    auto image = photos::Image::open(argv[1]);
    image->readMetadata();
    const auto info = photos::readPhotoInfo(*image);
    std::cout << photos::toString(image->type()) << " " << info.cameraMake << " " << info.cameraModel << "\n";
  } catch (const photos::Error& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}

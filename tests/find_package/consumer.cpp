#include <lumenlib/lumenlib.hpp>

#include <iostream>

int main(int argc, char** argv) {
  std::cout << "lumenlib " << LUMENLIB_VERSION_STRING << "\n";
  if (argc < 2) return 0;
  try {
    auto file = lumenlib::ImageFile::open(argv[1]);
    file->load();
    const auto info = lumenlib::readPhotoInfo(*file);
    std::cout << lumenlib::formatName(file->format()) << " " << info.cameraMake << " " << info.cameraModel << "\n";
  } catch (const lumenlib::Error& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}

#include "testing.hpp"

#include <cstring>

int main(int argc, char** argv) {
  int failed = 0, run = 0;
  for (const auto& t : testing::registry()) {
    if (argc > 1 && std::strstr(t.name, argv[1]) == nullptr) continue;
    ++run;
    try {
      t.fn();
    } catch (const testing::Failure& f) {
      ++failed;
      std::cout << "FAIL " << t.name << "\n  " << f.message << "\n";
    } catch (const std::exception& e) {
      ++failed;
      std::cout << "FAIL " << t.name << "\n  unexpected exception: " << e.what() << "\n";
    }
  }
  std::cout << run - failed << "/" << run << " tests passed (" << testing::checks() << " checks)\n";
  return failed ? 1 : 0;
}

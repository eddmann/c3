#include "c3/about.hpp"

namespace c3 {

std::string engine_name() {
  return "c3";
}

std::string engine_author() {
  return "Edd Mann";
}

std::string about_message() {
  return engine_name() + " - C++ Chess Cortex (author: " + engine_author() + ")";
}

void print_about(std::ostream& os) {
  os << about_message() << '\n';
}

} // namespace c3

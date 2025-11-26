#pragma once

#include <ostream>
#include <string>

namespace c3 {

std::string engine_name();
std::string engine_author();
std::string about_message();
void print_about(std::ostream& os);

} // namespace c3

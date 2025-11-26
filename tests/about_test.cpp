#include <gtest/gtest.h>

#include <sstream>

#include "c3/about.hpp"

TEST(About, MessageIncludesNameAndAuthor) {
  const auto message = c3::about_message();
  EXPECT_NE(message.find("c3"), std::string::npos);
  EXPECT_NE(message.find("Edd Mann"), std::string::npos);
}

TEST(About, PrintWritesMessageWithNewline) {
  std::ostringstream buffer;
  c3::print_about(buffer);
  EXPECT_EQ(buffer.str(), c3::about_message() + '\n');
}

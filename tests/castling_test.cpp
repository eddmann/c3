#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>

#include "c3/castling.hpp"
#include "c3/colour.hpp"
#include "c3/square.hpp"

using namespace c3;

TEST(CastlingRights, AddACastlingRight) {
  auto rights = CastlingRights::none();

  rights.add(CastlingRight::WhiteKing);

  EXPECT_EQ(rights, CastlingRights::from({CastlingRight::WhiteKing}));
}

TEST(CastlingRights, RemoveACastlingRight) {
  auto rights = CastlingRights::all();

  rights.remove(CastlingRight::WhiteKing);

  EXPECT_EQ(rights, CastlingRights::from({CastlingRight::WhiteQueen, CastlingRight::BlackKing,
                                          CastlingRight::BlackQueen}));
}

TEST(CastlingRights, RemoveCastlingRightsForAColour) {
  auto rights = CastlingRights::all();

  rights.remove_for_colour(Colour::White);

  EXPECT_EQ(rights, CastlingRights::from({CastlingRight::BlackKing, CastlingRight::BlackQueen}));
}

TEST(CastlingRights, RemoveCastlingRightsForACornerSquare) {
  auto rights = CastlingRights::all();

  rights.remove_for_square(Square::H1);

  EXPECT_EQ(rights, CastlingRights::from({CastlingRight::WhiteQueen, CastlingRight::BlackKing,
                                          CastlingRight::BlackQueen}));
}

TEST(CastlingRights, CheckForPresenceOfACastlingRight) {
  const auto rights = CastlingRights::from({CastlingRight::WhiteKing});

  EXPECT_TRUE(rights.has(CastlingRight::WhiteKing));

  const auto not_rights = {CastlingRight::WhiteQueen, CastlingRight::BlackKing,
                           CastlingRight::BlackQueen};

  for (auto right : not_rights) {
    EXPECT_FALSE(rights.has(right));
  }
}

TEST(CastlingRights, RemoveCastlingRightsForAKingStartSquare) {
  auto rights = CastlingRights::all();

  rights.remove_for_square(Square::E1);

  EXPECT_EQ(rights, CastlingRights::from({CastlingRight::BlackKing, CastlingRight::BlackQueen}));
}

TEST(CastlingRights, RemoveCastlingRightsForEveryRelevantSquare) {
  // Each corner costs exactly one right; the rest must survive untouched, which
  // only a comparison against the whole expected set can show.
  const std::array<std::pair<Square, CastlingRights>, 4> corners = {{
      {Square::A1, CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::BlackKing,
                                         CastlingRight::BlackQueen})},
      {Square::H1, CastlingRights::from({CastlingRight::WhiteQueen, CastlingRight::BlackKing,
                                         CastlingRight::BlackQueen})},
      {Square::A8, CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::WhiteQueen,
                                         CastlingRight::BlackKing})},
      {Square::H8, CastlingRights::from({CastlingRight::WhiteKing, CastlingRight::WhiteQueen,
                                         CastlingRight::BlackQueen})},
  }};

  for (const auto& [square, surviving] : corners) {
    auto rights = CastlingRights::all();
    rights.remove_for_square(square);
    EXPECT_EQ(rights, surviving) << square;
  }
}

// Squares that take no part in castling must leave the rights untouched, so the
// caller can apply the mask to every move without first asking "is this a corner?".
TEST(CastlingRights, RemoveCastlingRightsForAnUnrelatedSquareIsANoOp) {
  for (std::uint8_t index = 0; index < 64; ++index) {
    const Square square = Square::from_index(index);
    if (square == Square::A1 || square == Square::E1 || square == Square::H1 ||
        square == Square::A8 || square == Square::E8 || square == Square::H8) {
      continue;
    }

    auto rights = CastlingRights::all();
    rights.remove_for_square(square);
    EXPECT_EQ(rights, CastlingRights::all()) << square;
  }
}

#include <gtest/gtest.h>

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

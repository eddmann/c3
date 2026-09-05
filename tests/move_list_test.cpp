#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "c3/move.hpp"
#include "c3/move_list.hpp"
#include "c3/piece.hpp"
#include "c3/square.hpp"

using namespace c3;

namespace {

// A cheap supply of moves that compare unequal to each other. Move's equality
// looks at piece/from/to/promotion/en-passant, and varying `to` alone is enough
// to tell any two of these apart.
Move nth_move(std::size_t n) {
  return Move{
      .piece = Piece::WP,
      .from = Square::from_index(static_cast<std::uint8_t>(n % 64)),
      .to = Square::from_index(static_cast<std::uint8_t>((n * 7 + 3) % 64)),
      .captured_piece = std::nullopt,
      .promotion_piece = std::nullopt,
      .is_en_passant = false,
  };
}

MoveList first_n(std::size_t n) {
  MoveList moves;
  for (std::size_t i = 0; i < n; ++i) {
    moves.push_back(nth_move(i));
  }
  return moves;
}

void expect_holds_first_n(const MoveList& moves, std::size_t n) {
  ASSERT_EQ(moves.size(), n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(moves[i], nth_move(i)) << "index " << i;
  }
}

} // namespace

// Capacity -------------------------------------------------------------------

TEST(MoveListCapacity, CoversTheBusiestPositionThatCanOccur) {
  // The whole point of a fixed capacity is that it can never be exceeded, so
  // the bound it is chosen from is part of the contract: 218 legal moves is the
  // most any legal position offers, and the generator's pseudo-legal lists peak
  // at 248 even for material no real game can reach.
  static_assert(MoveList::CAPACITY >= MAX_MOVES_IN_A_POSITION);
  static_assert(MoveList::CAPACITY >= MAX_PSEUDO_LEGAL_MOVES);
  EXPECT_EQ(MoveList::CAPACITY, 256U);
  EXPECT_EQ(MoveList().capacity(), MoveList::CAPACITY);
  EXPECT_EQ(MoveList().max_size(), MoveList::CAPACITY);
}

TEST(MoveListCapacity, StoresItsMovesInline) {
  // No pointer to the heap: the moves live in the object, which is what makes a
  // move list free to create on the search stack.
  EXPECT_GE(sizeof(MoveList), MoveList::CAPACITY * sizeof(Move));
}

TEST(MoveListCapacity, FillsEveryLastSlot) {
  MoveList moves = first_n(MoveList::CAPACITY);

  EXPECT_EQ(moves.size(), MoveList::CAPACITY);
  EXPECT_FALSE(moves.empty());
  expect_holds_first_n(moves, MoveList::CAPACITY);
}

TEST(MoveListCapacity, IsContiguous) {
  const MoveList moves = first_n(8);

  // Contiguous storage is why the search can sort a move list with pointer
  // arithmetic and why a whole list fits in a handful of cache lines.
  for (std::size_t i = 0; i < moves.size(); ++i) {
    EXPECT_EQ(moves.data() + i, &moves[i]) << "index " << i;
  }
  EXPECT_EQ(std::to_address(moves.begin()), moves.data());
}

// Construction ---------------------------------------------------------------

TEST(MoveListConstruction, DefaultIsEmpty) {
  const MoveList moves;

  EXPECT_TRUE(moves.empty());
  EXPECT_EQ(moves.size(), 0U);
  EXPECT_EQ(moves.begin(), moves.end());
  EXPECT_EQ(moves.cbegin(), moves.cend());
}

TEST(MoveListConstruction, FromInitializerList) {
  const MoveList moves = {nth_move(0), nth_move(1), nth_move(2)};

  expect_holds_first_n(moves, 3);
}

TEST(MoveListConstruction, FromIteratorRange) {
  const std::vector<Move> source = {nth_move(0), nth_move(1), nth_move(2), nth_move(3)};

  const MoveList moves(source.begin(), source.end());

  expect_holds_first_n(moves, 4);
}

TEST(MoveListConstruction, FromAPrefixOfAnotherList) {
  // This is exactly how the search truncates a principal variation that walks
  // into a draw: keep the moves up to and including the repetition.
  const MoveList source = first_n(6);

  const MoveList truncated(source.begin(), source.begin() + 3);

  expect_holds_first_n(truncated, 3);
}

TEST(MoveListConstruction, FromAnEmptyRange) {
  const MoveList source;

  const MoveList copy(source.begin(), source.end());

  EXPECT_TRUE(copy.empty());
}

// push_back / emplace_back ---------------------------------------------------

TEST(MoveListPushBack, AppendsInOrder) {
  MoveList moves;

  moves.push_back(nth_move(0));
  moves.push_back(nth_move(1));

  expect_holds_first_n(moves, 2);
  EXPECT_EQ(moves.front(), nth_move(0));
  EXPECT_EQ(moves.back(), nth_move(1));
}

TEST(MoveListPushBack, EmplaceBuildsInPlaceAndReturnsTheMove) {
  MoveList moves;

  Move& placed = moves.emplace_back(nth_move(0));

  ASSERT_EQ(moves.size(), 1U);
  EXPECT_EQ(placed, nth_move(0));
  EXPECT_EQ(&placed, moves.data());
}

TEST(MoveListPushBack, PopBackDropsTheLastMove) {
  MoveList moves = first_n(3);

  moves.pop_back();

  expect_holds_first_n(moves, 2);
}

TEST(MoveListPushBack, ClearEmptiesWithoutLosingCapacity) {
  MoveList moves = first_n(10);

  moves.clear();

  EXPECT_TRUE(moves.empty());
  EXPECT_EQ(moves.capacity(), MoveList::CAPACITY);

  moves.push_back(nth_move(0));
  expect_holds_first_n(moves, 1);
}

// insert ---------------------------------------------------------------------

TEST(MoveListInsert, AppendsARangeAtTheEnd) {
  // The search builds a principal variation this way: the move it just played,
  // followed by the whole line the child returned.
  MoveList pv;
  const MoveList child = {nth_move(1), nth_move(2)};

  pv.push_back(nth_move(0));
  pv.insert(pv.end(), child.begin(), child.end());

  expect_holds_first_n(pv, 3);
}

TEST(MoveListInsert, AppendingAnEmptyRangeChangesNothing) {
  MoveList moves = first_n(2);
  const MoveList empty;

  moves.insert(moves.end(), empty.begin(), empty.end());

  expect_holds_first_n(moves, 2);
}

TEST(MoveListInsert, ShiftsTheTailWhenInsertingInTheMiddle) {
  MoveList moves = {nth_move(0), nth_move(3)};
  const std::vector<Move> middle = {nth_move(1), nth_move(2)};

  auto* const at = moves.insert(moves.begin() + 1, middle.begin(), middle.end());

  EXPECT_EQ(at, moves.begin() + 1);
  expect_holds_first_n(moves, 4);
}

TEST(MoveListInsert, InsertsASingleMove) {
  MoveList moves = {nth_move(0), nth_move(2)};

  auto* const at = moves.insert(moves.begin() + 1, nth_move(1));

  EXPECT_EQ(*at, nth_move(1));
  expect_holds_first_n(moves, 3);
}

// erase ----------------------------------------------------------------------

TEST(MoveListErase, RemovesASingleMoveAndReturnsTheNextOne) {
  MoveList moves = {nth_move(0), nth_move(9), nth_move(1)};

  auto* const next = moves.erase(moves.begin() + 1);

  ASSERT_EQ(moves.size(), 2U);
  EXPECT_EQ(*next, nth_move(1));
  expect_holds_first_n(moves, 2);
}

TEST(MoveListErase, RemovesARangeFromTheMiddle) {
  MoveList moves = {nth_move(0), nth_move(7), nth_move(8), nth_move(1)};

  auto* const next = moves.erase(moves.begin() + 1, moves.begin() + 3);

  EXPECT_EQ(*next, nth_move(1));
  expect_holds_first_n(moves, 2);
}

TEST(MoveListErase, RemovesATailAndLeavesTheHeadIntact) {
  MoveList moves = first_n(5);

  moves.erase(moves.begin() + 2, moves.end());

  expect_holds_first_n(moves, 2);
}

TEST(MoveListErase, ErasingAnEmptyRangeChangesNothing) {
  MoveList moves = first_n(3);

  moves.erase(moves.begin() + 1, moves.begin() + 1);

  expect_holds_first_n(moves, 3);
}

TEST(MoveListErase, SupportsTheEraseRemoveIdiom) {
  // How the generator filters a pseudo-legal list down to the legal moves.
  MoveList moves = first_n(6);

  const auto removed =
      std::ranges::remove_if(moves, [](const Move& mv) { return mv.to.index() % 2 == 0; });
  moves.erase(removed.begin(), removed.end());

  EXPECT_LT(moves.size(), 6U);
  for (const auto& mv : moves) {
    EXPECT_EQ(mv.to.index() % 2, 1);
  }
}

// Iteration ------------------------------------------------------------------

TEST(MoveListIteration, VisitsEveryMoveInOrder) {
  const MoveList moves = first_n(4);

  std::size_t seen = 0;
  for (const auto& mv : moves) {
    EXPECT_EQ(mv, nth_move(seen));
    ++seen;
  }

  EXPECT_EQ(seen, 4U);
  EXPECT_EQ(static_cast<std::size_t>(std::distance(moves.begin(), moves.end())), 4U);
}

TEST(MoveListIteration, WorksWithRangeAlgorithms) {
  MoveList moves = {nth_move(3), nth_move(1), nth_move(2)};

  // The search sorts move lists in place with std::ranges::sort, so the
  // iterators have to be genuine random-access, sortable iterators.
  static_assert(std::random_access_iterator<MoveList::iterator>);
  static_assert(std::ranges::random_access_range<MoveList>);
  static_assert(std::ranges::contiguous_range<MoveList>);

  std::ranges::sort(moves,
                    [](const Move& a, const Move& b) { return a.to.index() < b.to.index(); });

  EXPECT_TRUE(std::ranges::is_sorted(
      moves, [](const Move& a, const Move& b) { return a.to.index() < b.to.index(); }));
  EXPECT_EQ(std::ranges::count_if(moves, [](const Move& mv) { return mv.piece == Piece::WP; }), 3);
}

TEST(MoveListIteration, ConstIteratorsSeeTheSameMoves) {
  const MoveList moves = first_n(3);

  EXPECT_TRUE(std::equal(moves.cbegin(), moves.cend(), moves.begin(), moves.end()));
}

// Copy and move --------------------------------------------------------------

TEST(MoveListCopy, CopiesTheMovesAndNothingElse) {
  const MoveList original = first_n(5);

  const MoveList copy = original; // NOLINT(performance-unnecessary-copy-initialization)

  expect_holds_first_n(copy, 5);
  EXPECT_NE(copy.data(), original.data()) << "a copy must own its own storage";
}

TEST(MoveListCopy, CopiesAreIndependent) {
  const MoveList original = first_n(3);
  MoveList copy = original;

  copy.push_back(nth_move(99));
  copy[0] = nth_move(42);

  EXPECT_EQ(copy.size(), 4U);
  expect_holds_first_n(original, 3);
}

TEST(MoveListCopy, AssignmentReplacesTheWholeList) {
  MoveList target = first_n(7);
  const MoveList source = first_n(2);

  target = source;

  expect_holds_first_n(target, 2);
}

TEST(MoveListCopy, RvaluesUseTheCopyBecauseThereIsNothingToSteal) {
  // MoveList declares no move operations: the storage is inside the object, so
  // a move could only copy the moves that are there, which the copy already
  // does. Rvalues bind to the copy instead, and the generic machinery the
  // search wraps a list in keeps working.
  static_assert(std::is_copy_constructible_v<MoveList>);
  static_assert(std::is_copy_assignable_v<MoveList>);
  static_assert(std::is_constructible_v<MoveList, MoveList&&>);
  static_assert(std::is_assignable_v<MoveList&, MoveList&&>);

  // Exactly how the search reports a principal variation and its score.
  std::optional<std::pair<MoveList, int>> reported;
  reported = std::make_pair(first_n(3), 42);

  ASSERT_TRUE(reported.has_value());
  expect_holds_first_n(reported->first, 3);
  EXPECT_EQ(reported->second, 42);
}

// Comparison -----------------------------------------------------------------

TEST(MoveListComparison, EqualWhenTheSameMovesAreInTheSameOrder) {
  EXPECT_EQ(first_n(3), first_n(3));
  EXPECT_NE(first_n(3), first_n(4));

  const MoveList reordered = {nth_move(1), nth_move(0)};
  EXPECT_NE(reordered, first_n(2));
}

// Type properties ------------------------------------------------------------

TEST(MoveListTraits, ExposesTheUsualContainerTypedefs) {
  static_assert(std::is_same_v<MoveList::value_type, Move>);
  static_assert(std::is_same_v<MoveList::reference, Move&>);
  static_assert(std::is_same_v<MoveList::const_reference, const Move&>);
  static_assert(std::is_same_v<MoveList::iterator, Move*>);
  static_assert(std::is_same_v<MoveList::const_iterator, const Move*>);
  SUCCEED();
}

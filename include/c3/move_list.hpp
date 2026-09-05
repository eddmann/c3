#pragma once

// =============================================================================
// MOVE LIST: Fixed-Capacity Storage for the Moves of One Position
// =============================================================================
//
// Every node the search visits generates a move list, orders it, walks it and
// throws it away. At a few million nodes per second that container is created
// and destroyed a few million times per second, so what it costs to *exist*
// matters as much as what it costs to use.
//
// std::vector is the wrong shape for that job. A vector owns a heap buffer, so
// each list costs a malloc on its first push_back and a free when the node
// returns—two trips through the allocator per node, for storage that never
// outlives the node that made it. Worse, the moves themselves live wherever the
// allocator put them, so walking a list follows a pointer off into memory that
// has nothing to do with the search stack, and every node's list may land
// somewhere different.
//
// The fix is to notice that a move list has a known maximum size. The busiest
// legal chess position anyone has constructed offers 218 moves—a board packed
// with promoted queens—and no position can offer more. So the storage can be an
// array inside the object: no allocator, no pointer chasing, the moves sitting
// contiguously in the search's own stack frame where the cache already has
// them, and destruction is free because there is nothing to give back.
//
// WHY 256 AND NOT 218
// The generator produces *pseudo-legal* moves and filters them afterwards, so
// its lists can be longer than any legal move count. Hill-climbing over piece
// placements (see MAX_PSEUDO_LEGAL_MOVES below) puts that peak at 248, for
// material—fifteen queens—that no real game can reach. Rounding to 256 keeps
// the list a power of two and leaves headroom above the worst case that can be
// constructed, let alone played.
//
// WHAT IT COSTS
// sizeof(Move) is 8, so a list is 2 KiB of stack. The search recurses at most
// MAX_DEPTH = 255 plies and holds a handful of lists per frame, and quiescence
// below that holds one; a few MiB of stack in the very deepest case, well
// inside the usual 8 MiB thread limit, and nowhere near it at the depths a real
// search reaches. That is the trade: a fixed 2 KiB per list, paid up front on
// the stack, in exchange for never touching the allocator on the hot path.
//
// PRINCIPAL VARIATIONS FIT TOO
// The search reuses this type for principal variations. A PV is built as "the
// move played here, then the child's PV", and the recursion only ever *reduces*
// depth—there are no check extensions—so a PV cannot be longer than MAX_DEPTH
// (255) moves. 256 covers that as well.
// =============================================================================

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

#include "c3/move.hpp"

namespace c3 {

// The most legal moves any legal chess position offers.
inline constexpr std::size_t MAX_MOVES_IN_A_POSITION = 218;

// The most pseudo-legal moves this generator can ever produce, found by
// hill-climbing over placements of a full sixteen-piece side. The winning
// arrangement is fifteen queens plus a king, which is unreachable in a real
// game, so this is a ceiling over positions the search can never even see.
inline constexpr std::size_t MAX_PSEUDO_LEGAL_MOVES = 248;

// A vector-shaped container whose storage is a fixed-size array inside the
// object. The interface is the part of std::vector's that the engine uses, so
// callers cannot tell the difference—except that it never allocates.
class MoveList {
public:
  using value_type = Move;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = Move&;
  using const_reference = const Move&;
  using pointer = Move*;
  using const_pointer = const Move*;
  using iterator = Move*;
  using const_iterator = const Move*;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  static constexpr size_type CAPACITY = 256;

  static_assert(CAPACITY >= MAX_PSEUDO_LEGAL_MOVES,
                "a move list must hold every move the generator can produce");

  // The empty body is deliberate and must not become `= default`.
  //
  // A defaulted default constructor is not "user-provided", and value
  // initialisation (`MoveList pv{};`, which is how the search declares its PV
  // members) zero-initialises the whole object before running such a
  // constructor—wiping 2 KiB of storage that is about to be overwritten
  // anyway. Writing the constructor out by hand makes it user-provided, so
  // `MoveList{}` costs exactly one store to the size.
  // NOLINTNEXTLINE(modernize-use-equals-default)
  MoveList() noexcept {}

  MoveList(std::initializer_list<Move> moves) { append(moves.begin(), moves.end()); }

  template <std::input_iterator InputIt> MoveList(InputIt first, InputIt last) {
    for (; first != last; ++first) {
      push_back(*first);
    }
  }

  // Copying only touches the moves that are actually there; the rest of the
  // array is raw storage with nothing in it worth reading.
  MoveList(const MoveList& other) : size_{other.size_} {
    std::uninitialized_copy_n(other.data(), other.size_, data());
  }

  MoveList& operator=(const MoveList& other) {
    if (this != &other) {
      std::uninitialized_copy_n(other.data(), other.size_, data());
      size_ = other.size_;
    }
    return *this;
  }

  // There are deliberately no move operations: the storage is part of the
  // object, so there is no buffer to steal and no pointer to null out. Moving a
  // list could only copy the moves that are in it, which is what the copy above
  // already does, so rvalues bind to it and pay nothing extra.
  //
  // Nor is there anything for the destructor to do. Move is trivially
  // destructible, so a list simply goes out of scope with the stack frame that
  // holds it—no deallocation, no loop over the elements.
  ~MoveList() = default;

  [[nodiscard]] size_type size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] static constexpr size_type capacity() noexcept { return CAPACITY; }
  [[nodiscard]] static constexpr size_type max_size() noexcept { return CAPACITY; }

  [[nodiscard]] Move* data() noexcept { return reinterpret_cast<Move*>(storage_.data()); }
  [[nodiscard]] const Move* data() const noexcept {
    return reinterpret_cast<const Move*>(storage_.data());
  }

  [[nodiscard]] iterator begin() noexcept { return data(); }
  [[nodiscard]] iterator end() noexcept { return data() + size_; }
  [[nodiscard]] const_iterator begin() const noexcept { return data(); }
  [[nodiscard]] const_iterator end() const noexcept { return data() + size_; }
  [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] const_iterator cend() const noexcept { return end(); }

  [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator{end()}; }
  [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator{begin()}; }
  [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator{end()};
  }
  [[nodiscard]] const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator{begin()};
  }

  [[nodiscard]] reference operator[](size_type index) noexcept {
    assert(index < size_ && "move list index out of range");
    return data()[index];
  }
  [[nodiscard]] const_reference operator[](size_type index) const noexcept {
    assert(index < size_ && "move list index out of range");
    return data()[index];
  }

  [[nodiscard]] reference front() noexcept { return (*this)[0]; }
  [[nodiscard]] const_reference front() const noexcept { return (*this)[0]; }
  [[nodiscard]] reference back() noexcept { return (*this)[size_ - 1]; }
  [[nodiscard]] const_reference back() const noexcept { return (*this)[size_ - 1]; }

  void clear() noexcept { size_ = 0; }

  void push_back(const Move& move) {
    assert(size_ < CAPACITY && "move list overflow: a position generated more than CAPACITY moves");
    std::construct_at(data() + size_, move);
    ++size_;
  }

  template <typename... Args> reference emplace_back(Args&&... args) {
    assert(size_ < CAPACITY && "move list overflow: a position generated more than CAPACITY moves");
    Move* const slot = std::construct_at(data() + size_, std::forward<Args>(args)...);
    ++size_;
    return *slot;
  }

  void pop_back() noexcept {
    assert(size_ > 0 && "pop_back on an empty move list");
    --size_;
  }

  iterator insert(const_iterator position, const Move& move) {
    return insert(position, &move, &move + 1);
  }

  // Inserting shifts whatever follows `position` to the right. The search only
  // ever inserts at the end—appending a child's principal variation to the move
  // that leads into it—which is the case where the shift is empty.
  template <std::forward_iterator ForwardIt>
  iterator insert(const_iterator position, ForwardIt first, ForwardIt last) {
    const difference_type offset = position - cbegin();
    const auto count = static_cast<size_type>(std::distance(first, last));

    assert(offset >= 0 && static_cast<size_type>(offset) <= size_ &&
           "insert position out of range");
    assert(size_ + count <= CAPACITY && "move list overflow: insert exceeded CAPACITY");

    iterator const at = begin() + offset;
    std::move_backward(at, end(), end() + static_cast<difference_type>(count));
    std::copy(first, last, at);
    size_ += count;

    return at;
  }

  iterator erase(const_iterator position) { return erase(position, position + 1); }

  // Erasing closes the gap by sliding the tail down, which is what the
  // erase-remove idiom needs to drop the moves a filter has rejected.
  iterator erase(const_iterator first, const_iterator last) {
    const difference_type offset = first - cbegin();
    const auto count = static_cast<size_type>(last - first);

    assert(offset >= 0 && count <= size_ && static_cast<size_type>(offset) + count <= size_ &&
           "erase range out of bounds");

    iterator const from = begin() + offset;
    std::move(from + static_cast<difference_type>(count), end(), from);
    size_ -= count;

    return from;
  }

  friend bool operator==(const MoveList& lhs, const MoveList& rhs) noexcept {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  }

private:
  template <std::forward_iterator ForwardIt> void append(ForwardIt first, ForwardIt last) {
    insert(cend(), first, last);
  }

  // Move is trivially copyable and trivially destructible, which is what lets
  // the storage stay raw: a Move object springs into existence where one is
  // constructed, and none of the unused slots costs anything to skip.
  //
  // Declaring the storage as std::array<Move, CAPACITY> would be tidier to read
  // but would defeat the whole exercise, because Move has default member
  // initialisers: every list would default-construct 256 moves—2 KiB of stores
  // per node—before the generator wrote a single real one.
  static_assert(std::is_trivially_copyable_v<Move>);
  static_assert(std::is_trivially_destructible_v<Move>);

  alignas(Move) std::array<std::byte, sizeof(Move) * CAPACITY> storage_;
  size_type size_{0};
};

static_assert(std::ranges::contiguous_range<MoveList>);
static_assert(std::random_access_iterator<MoveList::iterator>);

} // namespace c3

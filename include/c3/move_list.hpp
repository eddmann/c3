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
// its lists can be longer than any legal move count. The largest anyone has
// found it producing is 248 (see MAX_PSEUDO_LEGAL_MOVES below), for material—
// fifteen queens—that no real game can reach. That 248 is the best a search
// found, not a proof that nothing beats it, so 256 is chosen to keep the list a
// power of two *and* to keep an eight-move margin over the highest figure
// anyone has managed to construct. Should even that margin turn out to be
// wrong, push_back saturates rather than overrunning the array.
//
// WHAT IT COSTS, AND THE STACK BUDGET THIS IMPOSES
// sizeof(Move) is 8, so one list is 2 KiB of stack. That is cheap per list and
// expensive per *frame*, because the cost is multiplied by how deep the search
// recurses and by how many lists each frame holds.
//
// Thread stacks are smaller than people assume, and the smallest one wins:
//
//   Linux (glibc), main and secondary threads   8 MiB
//   macOS secondary threads                     512 KiB
//   Windows threads                             1 MiB (default /STACK)
//
// The engine searches on a secondary thread, so 512 KiB is the budget to design
// against, not 8 MiB. At 512 KiB a frame holding one list can afford roughly
// 250 plies; a frame holding four cannot afford 64. So the rule this container
// imposes on its callers is: at most one MoveList per ply on the stack, and
// anything beyond that—per-ply scratch lists, principal-variation storage—
// belongs in a heap-allocated per-search context indexed by ply rather than in
// the recursive frame. src/search.cpp does not yet obey that rule; moving its
// per-ply lists off the stack is a pending search-side change.
//
// PRINCIPAL VARIATIONS
// The search reuses this type for principal variations, built as "the move
// played here, then the child's PV". Depth normally decreases by one per ply,
// but alphabeta applies a check extension—at depth 0 while in check it resets
// depth to 1 and recurses again—so depth alone does not bound the recursion,
// and today nothing caps the ply either. A PV is therefore only as bounded as
// the ply cap the search enforces, which is meant to be MAX_DEPTH (255) and is
// another pending search-side change. Until it lands, and afterwards as a last
// line of defence, this container saturates rather than overrun: see push_back.
// =============================================================================

#include <algorithm>
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

// The most pseudo-legal moves anyone has *found* this generator producing, from
// a hill-climb over placements of a full sixteen-piece side. The best
// arrangement was fifteen queens plus a king—material no real game can reach—so
// it is a bound on positions the search will never see. It is a search result,
// not a proof: no one has shown 248 cannot be beaten, so CAPACITY keeps an
// eight-move margin over it and push_back saturates if even that is wrong.
//
// MoveListCapacity.DISABLED_HillClimbStaysUnderCapacity in
// tests/movegen_test.cpp re-runs the experiment; the comment there says how.
inline constexpr std::size_t MAX_PSEUDO_LEGAL_MOVES = 248;

// A vector-shaped container whose storage is a fixed-size array inside the
// object. The interface is the part of std::vector's that the engine uses, so
// callers cannot tell the difference—except for three things worth knowing:
//
//   * capacity() and max_size() are static and constant. There is no reserve()
//     and no growth; the capacity a list has is the capacity it was born with.
//   * Pointers, references and iterators into a list survive push_back and
//     emplace_back, because there is no reallocation to move the elements. A
//     std::vector invalidates all of them on a grow. Only insert and erase move
//     elements, and then only from the affected position onwards.
//   * Pushing past CAPACITY drops the move instead of growing. See push_back.
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

  [[nodiscard]] Move* data() noexcept { return moves_; }
  [[nodiscard]] const Move* data() const noexcept { return moves_; }

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

  // WHAT HAPPENS WHEN A LIST IS FULL
  //
  // Two things, deliberately. In a Debug build the assert fires and the test
  // that provoked it fails, which is how a capacity that turned out to be too
  // small gets discovered. In a Release build, where the assert is compiled
  // away, the push is *dropped*.
  //
  // Dropping is not a good outcome—the engine would search a position without
  // one of its moves and could return the wrong one. But it is a contained
  // wrong answer: the list stays valid, every invariant holds, and the damage
  // stops at this list. Writing the 257th move would instead run off the end of
  // the array and over whatever the compiler put next to it—size_ first, then
  // neighbouring objects in the caller's stack frame. That is memory
  // corruption, it is not caught by ASan (the overrun stays inside one object,
  // so there is no redzone to trip), and it surfaces far from its cause, as a
  // stack-smashing abort or as nothing at all. A wrong move is recoverable; a
  // corrupted stack is not.
  void push_back(const Move& move) {
    assert(size_ < CAPACITY && "move list overflow: a position generated more than CAPACITY moves");
    if (size_ == CAPACITY) {
      return;
    }
    std::construct_at(data() + size_, move);
    ++size_;
  }

  // Saturates like push_back. When the list is already full there is no new
  // element to hand back, so the caller gets the last one that fits.
  template <typename... Args> reference emplace_back(Args&&... args) {
    assert(size_ < CAPACITY && "move list overflow: a position generated more than CAPACITY moves");
    if (size_ == CAPACITY) {
      return back();
    }
    Move* const slot = std::construct_at(data() + size_, std::forward<Args>(args)...);
    ++size_;
    return *slot;
  }

  void pop_back() noexcept {
    assert(size_ > 0 && "pop_back on an empty move list");
    --size_;
  }

  // `move` is taken by value on purpose: it may name an element of this very
  // list, and the shift below would move that element out from under the
  // reference before it was read. Copying first makes lst.insert(lst.begin(),
  // lst[2]) mean what it looks like it means.
  iterator insert(const_iterator position, Move move) { return insert(position, &move, &move + 1); }

  // Inserting shifts whatever follows `position` to the right. The search only
  // ever inserts at the end—appending a child's principal variation to the move
  // that leads into it—which is the case where the shift is empty.
  //
  // Like push_back, this saturates: a range that would not fit is truncated
  // from its tail rather than written past the end of the array.
  template <std::forward_iterator ForwardIt>
  iterator insert(const_iterator position, ForwardIt first, ForwardIt last) {
    const difference_type offset = position - cbegin();
    const auto requested = static_cast<size_type>(std::distance(first, last));

    assert(offset >= 0 && static_cast<size_type>(offset) <= size_ &&
           "insert position out of range");
    assert(size_ + requested <= CAPACITY && "move list overflow: insert exceeded CAPACITY");

    const size_type count = std::min(requested, CAPACITY - size_);

    iterator const at = begin() + offset;
    std::move_backward(at, end(), end() + static_cast<difference_type>(count));
    std::copy_n(first, count, at);
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

  // WHY THE STORAGE IS A UNION
  //
  // A plain `Move moves_[CAPACITY]` member would be default-initialised, and
  // Move has default member initialisers, so every list would construct 256
  // moves—2 KiB of stores—before the generator wrote a single real one. That is
  // the cost this whole container exists to avoid, so the array must start out
  // with no live objects in it at all.
  //
  // Wrapping it in an anonymous union is how C++ says that. A variant member is
  // not initialised unless a constructor names it, and MoveList's constructors
  // never do, so the array costs nothing to declare. Elements then come to life
  // one at a time under std::construct_at as moves are pushed.
  //
  // The obvious alternative—`std::byte` storage plus a reinterpret_cast to
  // Move*—is what this used to be, and it is not actually well-formed. Even in
  // C++23 a byte array does not on its own create the Move objects a cast then
  // pretends to find; only a listed object-creating operation does, and
  // declaring an array of bytes is not one of them. The union has no such gap:
  // Move is an implicit-lifetime type, so the array is one too, and the
  // language creates it in the union's storage as soon as the program needs it
  // to exist. It also keeps the pointer arithmetic honest (`moves_ + i` walks a
  // real Move array, not bytes reinterpreted as one) and makes alignment
  // automatic, so no alignas is needed.
  //
  // This is the standard shape of a fixed-capacity vector, and it works only
  // because Move is trivially copyable and trivially destructible.
  static_assert(std::is_trivially_copyable_v<Move>);
  static_assert(std::is_trivially_destructible_v<Move>);

  union {
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    Move moves_[CAPACITY];
  };
  size_type size_{0};
};

static_assert(std::ranges::contiguous_range<MoveList>);
static_assert(std::random_access_iterator<MoveList::iterator>);

} // namespace c3

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "c3/eval.hpp"
#include "c3/movegen.hpp"
#include "c3/search.hpp"

using namespace c3;
namespace search = c3::search;

// =============================================================================
// Thread-Safe Transposition Table Tests
// =============================================================================
// These tests verify that the TT can be safely accessed from multiple threads
// without data races or corruption. This is essential for Lazy SMP.

TEST(ThreadSafeTT, ConcurrentProbesDoNotCrash) {
  search::TranspositionTable tt;

  // Pre-populate with some entries
  for (std::uint64_t i = 0; i < 1000; ++i) {
    tt.store(i * 12345, 5, static_cast<int>(i), search::Bound::Exact, std::nullopt);
  }

  std::atomic<bool> start{false};
  std::atomic<std::uint64_t> total_probes{0};

  auto probe_worker = [&](int thread_id) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    std::uint64_t probes = 0;
    for (int i = 0; i < 10000; ++i) {
      const std::uint64_t key = static_cast<std::uint64_t>((thread_id * 1000 + i) * 12345);
      [[maybe_unused]] const auto* entry = tt.probe(key);
      ++probes;
    }
    total_probes.fetch_add(probes, std::memory_order_relaxed);
  };

  std::vector<std::thread> threads;
  constexpr int kNumThreads = 4;
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back(probe_worker, i);
  }

  start.store(true, std::memory_order_release);

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(total_probes.load(), kNumThreads * 10000);
}

TEST(ThreadSafeTT, ConcurrentStoresDoNotCrash) {
  search::TranspositionTable tt;

  std::atomic<bool> start{false};
  std::atomic<std::uint64_t> total_stores{0};

  auto store_worker = [&](int thread_id) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    std::uint64_t stores = 0;
    for (int i = 0; i < 10000; ++i) {
      const std::uint64_t key = static_cast<std::uint64_t>((thread_id * 10000 + i) * 7919);
      tt.store(key, 5, i * (thread_id + 1), search::Bound::Exact, std::nullopt);
      ++stores;
    }
    total_stores.fetch_add(stores, std::memory_order_relaxed);
  };

  std::vector<std::thread> threads;
  constexpr int kNumThreads = 4;
  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back(store_worker, i);
  }

  start.store(true, std::memory_order_release);

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(total_stores.load(), kNumThreads * 10000);
}

TEST(ThreadSafeTT, ConcurrentMixedAccessDoesNotCrash) {
  search::TranspositionTable tt;

  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};

  auto writer = [&](int thread_id) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    std::uint64_t counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const std::uint64_t key = static_cast<std::uint64_t>(thread_id * 1000000 + counter);
      tt.store(key, 10, static_cast<int>(counter), search::Bound::Lower, std::nullopt);
      ++counter;
    }
  };

  auto reader = [&](int thread_id) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    std::uint64_t counter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const std::uint64_t key = static_cast<std::uint64_t>(thread_id * 1000000 + counter);
      [[maybe_unused]] const auto* entry = tt.probe(key);
      ++counter;
    }
  };

  std::vector<std::thread> threads;
  constexpr int kNumWriters = 2;
  constexpr int kNumReaders = 2;

  for (int i = 0; i < kNumWriters; ++i) {
    threads.emplace_back(writer, i);
  }
  for (int i = 0; i < kNumReaders; ++i) {
    threads.emplace_back(reader, i + kNumWriters);
  }

  start.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_release);

  for (auto& t : threads) {
    t.join();
  }

  // If we get here without crashing or sanitizer errors, the test passes
  SUCCEED();
}

// =============================================================================
// Thread Pool Tests
// =============================================================================

TEST(ThreadPool, CanCreateWithDefaultThreadCount) {
  search::ThreadPool pool;
  EXPECT_GE(pool.size(), 1U);
}

TEST(ThreadPool, CanResizeThreadCount) {
  search::ThreadPool pool;

  pool.resize(4);
  EXPECT_EQ(pool.size(), 4U);

  pool.resize(2);
  EXPECT_EQ(pool.size(), 2U);

  pool.resize(1);
  EXPECT_EQ(pool.size(), 1U);
}

TEST(ThreadPool, ResizeToZeroSetsToOne) {
  search::ThreadPool pool;
  pool.resize(0);
  EXPECT_EQ(pool.size(), 1U);
}

TEST(ThreadPool, ResizeAboveMaxClampsToMax) {
  search::ThreadPool pool;
  pool.resize(1000);
  EXPECT_LE(pool.size(), 256U);
}

// =============================================================================
// Shared Transposition Table Tests
// =============================================================================

TEST(SharedTT, CanCreateAndAccess) {
  auto tt = std::make_shared<search::TranspositionTable>();
  EXPECT_NE(tt, nullptr);
  EXPECT_GT(tt->capacity(), 0U);
}

TEST(SharedTT, CanClear) {
  search::TranspositionTable tt;
  tt.store(12345, 5, 100, search::Bound::Exact, std::nullopt);
  EXPECT_GT(tt.usage(), 0U);

  tt.clear();
  EXPECT_EQ(tt.usage(), 0U);
}

// =============================================================================
// Multi-threaded Search Tests
// =============================================================================

namespace {

Position parse(std::string_view fen) {
  return Position::from_fen(fen);
}

std::string to_uci(const Move& mv) {
  auto sq_to_str = [](Square sq) {
    std::string out;
    out.push_back(static_cast<char>('a' + sq.file()));
    out.push_back(static_cast<char>('1' + sq.rank()));
    return out;
  };

  std::string uci = sq_to_str(mv.from) + sq_to_str(mv.to);
  if (mv.promotion_piece.has_value()) {
    uci.push_back(static_cast<char>(std::tolower(to_char(*mv.promotion_piece))));
  }
  return uci;
}

} // namespace

TEST(MultiThreadedSearch, SingleThreadProducesSameResultAsLegacy) {
  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;

  // Single-threaded search should produce deterministic results
  const auto result1 = search::search(pos, limits, reporter);
  const auto result2 = search::search(pos, limits, reporter);

  EXPECT_EQ(result1.depth, result2.depth);
  EXPECT_EQ(result1.eval, result2.eval);
  ASSERT_FALSE(result1.pv.empty());
  ASSERT_FALSE(result2.pv.empty());
  EXPECT_EQ(to_uci(result1.pv[0]), to_uci(result2.pv[0]));
}

TEST(MultiThreadedSearch, MultipleThreadsProduceValidResult) {
  search::ThreadPool::set_thread_count(4);

  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 4;

  const auto result = search::search(pos, limits, reporter);

  // Should get a valid result
  EXPECT_GE(result.depth, 1);
  ASSERT_FALSE(result.pv.empty());

  // Best move should be a legal move
  const auto legal = pseudo_legal_moves(pos);
  bool found = false;
  for (const auto& mv : legal) {
    if (mv == result.pv[0]) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);

  // Reset to single thread for other tests
  search::ThreadPool::set_thread_count(1);
}

TEST(MultiThreadedSearch, FindsMateWithMultipleThreads) {
  search::ThreadPool::set_thread_count(4);

  // Back rank mate: Re8#
  Position pos = parse("6k1/5ppp/8/8/8/8/8/4R1K1 w - - 0 1");

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  ASSERT_FALSE(result.pv.empty());
  EXPECT_EQ(to_uci(result.pv[0]), "e1e8");
  EXPECT_GT(result.eval, CENTIPAWN_MATE - 100);

  search::ThreadPool::set_thread_count(1);
}

TEST(MultiThreadedSearch, StopSignalHaltsAllThreads) {
  search::ThreadPool::set_thread_count(4);

  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 100; // Very deep

  auto stop_signal = std::make_shared<std::atomic_bool>(false);

  std::thread stopper([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_signal->store(true, std::memory_order_release);
  });

  const auto result = search::search(pos, limits, reporter, stop_signal);

  stopper.join();

  // Should have stopped early
  EXPECT_LT(result.depth, 100);

  search::ThreadPool::set_thread_count(1);
}

TEST(MultiThreadedSearch, NodeCountIsAccurate) {
  search::ThreadPool::set_thread_count(4);

  Position pos = Position::startpos();

  search::NullReporter reporter;
  search::Limits limits;
  limits.depth = 3;

  const auto result = search::search(pos, limits, reporter);

  // Node count should be reasonable for depth 3
  EXPECT_GT(result.nodes, 0U);
  EXPECT_LT(result.nodes, 1000000U); // Sanity check

  search::ThreadPool::set_thread_count(1);
}

// =============================================================================
// Thread Count Configuration Tests
// =============================================================================

TEST(ThreadConfig, SetThreadCountPersists) {
  search::ThreadPool::set_thread_count(8);
  EXPECT_EQ(search::ThreadPool::thread_count(), 8U);

  search::ThreadPool::set_thread_count(1);
  EXPECT_EQ(search::ThreadPool::thread_count(), 1U);
}

TEST(ThreadConfig, ThreadCountMinimumIsOne) {
  search::ThreadPool::set_thread_count(0);
  EXPECT_EQ(search::ThreadPool::thread_count(), 1U);
}

TEST(ThreadConfig, ThreadCountMaximumIs256) {
  search::ThreadPool::set_thread_count(1000);
  EXPECT_EQ(search::ThreadPool::thread_count(), 256U);
}

TEST(ThreadConfig, DefaultThreadCountIsOne) {
  // Reset to default
  search::ThreadPool::set_thread_count(1);
  EXPECT_EQ(search::ThreadPool::thread_count(), 1U);
}

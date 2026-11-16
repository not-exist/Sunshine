/**
 * @file tests/unit/test_startup_latch.cpp
 * @brief Test video::startup_latch_t.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <thread>

// local includes
#include <src/video.h>

TEST(StartupLatchTest, WaitTimesOutBeforeReady) {
  video::startup_latch_t latch;
  EXPECT_FALSE(latch.is_ready());
  EXPECT_FALSE(latch.wait_for(std::chrono::milliseconds {1}));
}

TEST(StartupLatchTest, WaitReturnsImmediatelyAfterReady) {
  video::startup_latch_t latch;
  latch.mark_ready();
  EXPECT_TRUE(latch.is_ready());
  EXPECT_TRUE(latch.wait_for(std::chrono::milliseconds {0}));
}

TEST(StartupLatchTest, MarkReadyIsIdempotent) {
  video::startup_latch_t latch;
  latch.mark_ready();
  latch.mark_ready();
  EXPECT_TRUE(latch.is_ready());
}

TEST(StartupLatchTest, WaitForStartupReadyTimesOutBeforeReady) {
  EXPECT_FALSE(video::wait_for_startup_ready(std::chrono::milliseconds {0}));
}

TEST(StartupLatchTest, MarkReadyWakesWaiter) {
  video::startup_latch_t latch;
  std::atomic<bool> started {false};
  std::atomic<bool> result {false};
  std::jthread waiter {[&] {
    started.store(true);
    result.store(latch.wait_for(std::chrono::seconds {5}));
  }};

  while (!started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds {1});
  }
  latch.mark_ready();
  waiter.join();

  EXPECT_TRUE(result.load());
}

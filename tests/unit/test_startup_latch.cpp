/**
 * @file tests/unit/test_startup_latch.cpp
 * @brief Test the startup readiness gate used by request handlers.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <latch>

// local includes
#include <src/video.h>

TEST(StartupReadinessTest, GlobalLatchStartsNotReady) {
  EXPECT_FALSE(video::is_startup_ready());
}

TEST(StartupReadinessTest, NotReadyBeforeLatchRelease) {
  std::latch latch {1};
  safe::event_t<bool> shutdown_event;

  EXPECT_FALSE(video::is_startup_ready(latch, shutdown_event));
}

TEST(StartupReadinessTest, ReadyAfterLatchRelease) {
  std::latch latch {1};
  safe::event_t<bool> shutdown_event;

  latch.count_down();

  EXPECT_TRUE(video::is_startup_ready(latch, shutdown_event));
}

TEST(StartupReadinessTest, NotReadyWhenShutdownWasRequestedBeforeLatchRelease) {
  std::latch latch {1};
  safe::event_t<bool> shutdown_event;

  shutdown_event.raise(true);

  EXPECT_FALSE(video::is_startup_ready(latch, shutdown_event));
}

TEST(StartupReadinessTest, NotReadyWhenShutdownWasRequestedAfterLatchRelease) {
  std::latch latch {1};
  safe::event_t<bool> shutdown_event;

  latch.count_down();
  shutdown_event.raise(true);

  EXPECT_FALSE(video::is_startup_ready(latch, shutdown_event));
}

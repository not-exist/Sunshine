/**
 * @file tests/unit/test_startup_latch.cpp
 * @brief Test the startup readiness gate used by request handlers.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <memory>

// local includes
#include <src/video.h>

namespace {
  /**
   * @brief Reset the process-wide readiness state around each test.
   */
  class StartupReadinessTest: public BaseTest {
  protected:
    /**
     * @brief Clear readiness and any cached shutdown event before the test.
     */
    void SetUp() override {
      BaseTest::SetUp();
      video::set_startup_ready_for_testing(false);
    }

    /**
     * @brief Clear readiness and any cached shutdown event after the test.
     */
    void TearDown() override {
      video::set_startup_ready_for_testing(false);
      BaseTest::TearDown();
    }
  };

  TEST_F(StartupReadinessTest, GlobalStateStartsNotReady) {
    EXPECT_FALSE(video::is_startup_ready());
  }

  TEST_F(StartupReadinessTest, NotReadyBeforeStartupFinishes) {
    safe::event_t<bool> shutdown_event;

    EXPECT_FALSE(video::is_startup_ready(false, shutdown_event));
  }

  TEST_F(StartupReadinessTest, ReadyWhenStartupFinishedAndShutdownNotRequested) {
    safe::event_t<bool> shutdown_event;

    EXPECT_TRUE(video::is_startup_ready(true, shutdown_event));
  }

  TEST_F(StartupReadinessTest, NotReadyWhenShutdownRequestedAfterStartupFinished) {
    safe::event_t<bool> shutdown_event;
    shutdown_event.raise(true);

    EXPECT_FALSE(video::is_startup_ready(true, shutdown_event));
  }

  TEST_F(StartupReadinessTest, GlobalReadyWithoutShutdownEvent) {
    video::mark_startup_ready();

    EXPECT_TRUE(video::is_startup_ready());
  }

  TEST_F(StartupReadinessTest, TestingSetterCanMarkReady) {
    video::set_startup_ready_for_testing(true);

    EXPECT_TRUE(video::is_startup_ready());
  }

  TEST_F(StartupReadinessTest, GlobalReadyWhileShutdownEventIsInactive) {
    auto shutdown_event = std::make_shared<safe::event_t<bool>>();
    video::set_shutdown_event(shutdown_event);
    video::mark_startup_ready();

    EXPECT_TRUE(video::is_startup_ready());
  }

  TEST_F(StartupReadinessTest, GlobalNotReadyAfterShutdownWasRaised) {
    auto shutdown_event = std::make_shared<safe::event_t<bool>>();
    video::set_shutdown_event(shutdown_event);
    video::mark_startup_ready();
    shutdown_event->raise(true);

    EXPECT_FALSE(video::is_startup_ready());
  }
}  // namespace

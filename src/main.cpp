/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <atomic>
#include <cerrno>
#include <chrono>
#include <codecvt>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stop_token>

// platform includes
#ifdef __APPLE__
  #include "platform/macos/misc.h"

  #include <mach-o/dyld.h>
#endif
#ifdef __linux__
  #include "platform/common.h"
  #include "platform/linux/graphics.h"
  #include "platform/linux/misc.h"

  #include <sys/auxv.h>
#endif
#ifndef _WIN32
  #include <fcntl.h>
  #include <unistd.h>
#endif

// lib includes
#include <rs.h>
#ifdef _WIN32
  #include <libvirtualhid/license.hpp>
#endif

// local includes
#include "confighttp.h"
#include "display_device.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "platform/permissions.h"
#include "process.h"
#include "system_tray.h"
#include "upnp.h"
#include "video.h"

using namespace std::literals;

namespace {
  std::atomic<int> received_shutdown_signal {0};  ///< Signal number recorded by the async-signal-safe handler.
  static_assert(std::atomic<int>::is_always_lock_free, "The signal handler must only use a lock-free atomic");

#ifdef _WIN32
  HANDLE shutdown_wakeup_event = nullptr;  ///< Auto-reset event used to wake the shutdown thread.
#else
  int shutdown_wakeup_pipe[2] = {-1, -1};  ///< Self-pipe used to wake the shutdown thread without polling.
#endif

  /**
   * @brief Wake the dedicated shutdown thread from signal or request-stop context.
   */
  void wake_shutdown_thread() {
#ifdef _WIN32
    if (shutdown_wakeup_event) {
      SetEvent(shutdown_wakeup_event);
    }
#else
    // write() is async-signal-safe. A full pipe only means a wakeup is already pending.
    if (shutdown_wakeup_pipe[1] >= 0) {
      const char token = 1;
      (void) !::write(shutdown_wakeup_pipe[1], &token, sizeof(token));
    }
#endif
  }

  /**
   * @brief Record a termination signal without performing work that is unsafe in signal context.
   *
   * The signal is acted upon by a dedicated thread in main(), which performs the actual shutdown.
   * The wakeup keeps that thread event-driven instead of polling for signals.
   *
   * @param sig Native signal number being handled.
   */
  void shutdown_signal_handler(int sig) {
    // write() may clobber errno in the interrupted thread; restore it before returning.
    const int saved_errno = errno;
    received_shutdown_signal.store(sig, std::memory_order_relaxed);
    wake_shutdown_thread();
    errno = saved_errno;
  }
}  // namespace

/**
 * @brief Dispatch commands through a common signature, ignoring arguments each command does not need.
 */
std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int, char **) {
     return args::help(name);
   }},
  {"version"sv, [](const char *, int, char **) {
     return args::version();
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *, int, char **) {
     return args::restore_nvprefs_undo();
   }},
#endif
};

#ifdef _WIN32
/**
 * @brief Handle Windows session-change messages for the monitor window.
 *
 * @param hwnd Window handle receiving the Windows control event.
 * @param uMsg U msg.
 * @param wParam W param.
 * @param lParam L param.
 * @return Process or platform callback exit code.
 */
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

/**
 * @brief Handle Windows console control events for shutdown.
 *
 * @param type Protocol, message, or resource type selector.
 * @return Process or platform callback exit code.
 */
WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;  ///< Compile-time flag indicating tray support is enabled.
#else
constexpr bool tray_is_enabled = false;
#endif

/**
 * @brief Run the main event loop until Sunshine is asked to exit.
 *
 * @param shutdown_event Shutdown event.
 */
void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // Conditions that would require the main thread event loop
#ifndef _WIN32
  run_loop = tray_is_enabled && config::sunshine.system_tray;  // On Windows, tray runs in separate thread, so no main loop needed for tray
#endif

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    // Wait for shutdown
    shutdown_event->view();
    return;
  }

  // Main thread event loop
  BOOST_LOG(info) << "Starting main loop"sv;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  system_tray::run_tray_until_exit(shutdown_event);
#endif
  BOOST_LOG(info) << "Main loop has exited"sv;
}

/**
 * @brief Run the main application or worker loop.
 *
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return Process or platform callback exit code.
 */
int main(int argc, char *argv[]) {
#ifdef __linux__
  const bool privileged_execution = getauxval(AT_SECURE) != 0 || platf::has_elevated_privileges(true);
  if (privileged_execution && !platf::sanitize_process_environment()) {
    std::cerr << "Failed to sanitize the environment for privileged execution: " << std::strerror(errno) << '\n';
    return 1;
  }
#endif

#ifdef __APPLE__
  // Bundle assets are referenced relative to the executable
  // (e.g. ../Resources/assets), so anchor cwd to Contents/MacOS.
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec);
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif
#if defined(__linux__)
  // On Linux, child threads inherit capabilities from the parent at creation time.
  // We ensure the privileged worker starts while capabilities are still held,
  // and then immediately strip global privileges from the main thread and future tasks.
  // Please ensure that these calls remain ordered as early as possible in startup initialization.
  #if defined(SUNSHINE_BUILD_DRM)
  platf::kms::ensure_privileged_drm_worker_started();
  #endif
  // Check and drop capabilities but use 'false' to retain CAP_SYS_NICE for EGL high priority contexts
  if (platf::has_elevated_privileges(false)) {
    platf::drop_elevated_privileges(false);
  }

  // Next, initialize privileged EGL worker thread
  egl::ensure_privileged_egl_worker_started();
  // Finally, check and drop all capabilities via 'true', which includes CAP_SYS_NICE.
  if (platf::has_elevated_privileges(true)) {
    platf::drop_elevated_privileges(true);
  }
#endif

  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  config::log_config_settings(config::modified_config_settings, false);
  config::modified_config_settings.clear();

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

#ifdef _WIN32
  config::select_all_gamepad_drivers_if_licensed(lvh::get_license_status().license.licensed());
#endif

  // The display device guard is created after the HTTP servers start so a slow display
  // recovery does not delay the Web UI. The shutdown thread clears it through these mutexes.
  std::unique_ptr<platf::deinit_t> display_device_deinit_guard;
  std::mutex display_guard_mutex;
  // Serializes guard teardown against encoder and input probing. Probing must not run after the
  // guard is destroyed, because a reverted display yields an empty device list that bypasses the
  // no-active-device protection in video::allow_encoder_probing().
  std::mutex startup_probe_mutex;
  auto take_display_guard = [&]() {
    std::lock_guard lock {display_guard_mutex};
    return std::move(display_device_deinit_guard);
  };
  auto reset_display_guard = [&]() {
    // Only the shutdown thread calls this, so it may block. The guard is destroyed after the
    // lock is released because its destructor takes libdisplaydevice's own mutex.
    std::lock_guard probe_lock {startup_probe_mutex};
    auto guard = take_display_guard();
    guard.reset();
  };
  auto init_display_device = [&]() {
    auto guard = display_device::init(platf::appdata() / "display_device.state", config::video);
    if (!guard) {
      BOOST_LOG(error) << "Display device session failed to initialize"sv;
    }

    std::lock_guard lock {display_guard_mutex};
    display_device_deinit_guard = std::move(guard);
  };

#ifdef _WIN32
  auto apply_nvprefs_settings = [&]() {
    // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
    if (nvprefs_instance.load()) {
      // Restore global settings to the undo file left by improper termination of sunshine.exe
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      // Modify application settings for sunshine.exe
      nvprefs_instance.modify_application_profile();
      // Modify global settings, undo file is produced in the process to restore after improper termination
      nvprefs_instance.modify_global_profile();
      // Unload dynamic library to survive driver re-installation
      nvprefs_instance.unload();
    }
  };
#endif

  // This used to run unconditionally before the HTTP servers could be reached. Keep it on every
  // startup path so a previous crash cannot leave the display or NVIDIA settings broken.
  auto init_display_and_platform_settings = [&]() {
    init_display_device();
#ifdef _WIN32
    apply_nvprefs_settings();
#endif
  };

  // Run the recovery even when an exception unwinds main before the startup block reaches it
  // (for example when thread creation fails), matching the old unconditional behavior.
  bool display_recovery_started = false;
  auto initialize_display = [&]() {
    display_recovery_started = true;
    init_display_and_platform_settings();
  };
  auto display_recovery_guard = util::fail_guard([&]() {
    if (display_recovery_started) {
      return;
    }

    try {
      init_display_and_platform_settings();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Display recovery failed during shutdown: "sv << e.what();
    } catch (...) {
      BOOST_LOG(error) << "Display recovery failed during shutdown"sv;
    }
  });

#ifdef _WIN32
  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::jthread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // The shutdown thread waits for the startup block before arming the forced-shutdown timer, so a
  // slow display recovery can complete instead of being killed with the display left reverted.
  std::mutex startup_completion_mutex;
  std::condition_variable startup_completion_cv;
  std::atomic<bool> startup_completed {false};
  auto mark_startup_completed = [&]() {
    {
      std::lock_guard lock {startup_completion_mutex};
      startup_completed.store(true, std::memory_order_release);
    }
    startup_completion_cv.notify_all();
  };

  // Create signal handling after logging has been initialized. The handler itself only records
  // the signal and wakes this thread through a self-pipe or event: taking locks or destroying the
  // display guard in signal context can deadlock when the signal interrupts a thread that already
  // holds those locks, and those primitives are not async-signal-safe.
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  video::set_shutdown_event(shutdown_event);

  bool shutdown_wakeup_available = false;
#ifdef _WIN32
  shutdown_wakeup_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  shutdown_wakeup_available = shutdown_wakeup_event != nullptr;
  if (!shutdown_wakeup_available) {
    BOOST_LOG(warning) << "Failed to create the shutdown wakeup event: "sv << GetLastError();
  }
#else
  if (::pipe(shutdown_wakeup_pipe) == 0) {
    for (const int fd : shutdown_wakeup_pipe) {
      const int flags = ::fcntl(fd, F_GETFD);
      if (flags >= 0) {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
      }
    }
    // The signal handler writes to this pipe, so a full pipe must not block it.
    const int write_flags = ::fcntl(shutdown_wakeup_pipe[1], F_GETFL);
    if (write_flags >= 0) {
      ::fcntl(shutdown_wakeup_pipe[1], F_SETFL, write_flags | O_NONBLOCK);
    }
    shutdown_wakeup_available = true;
  } else {
    BOOST_LOG(warning) << "Failed to create the shutdown wakeup pipe: "sv << std::strerror(errno);
    shutdown_wakeup_pipe[0] = -1;
    shutdown_wakeup_pipe[1] = -1;
  }
#endif

  std::signal(SIGINT, shutdown_signal_handler);
  std::signal(SIGTERM, shutdown_signal_handler);

  std::jthread shutdown_thread {[&](std::stop_token stop_token) {
    platf::set_thread_name("shutdown");

    // Wake the blocking wait when a stop is requested so the thread can be joined from any exit
    // path, including early returns that only destroy the jthread.
    std::stop_callback wake_on_stop {stop_token, []() {
      wake_shutdown_thread();
    }};

    auto wait_for_wakeup = [&]() {
      if (!shutdown_wakeup_available) {
        std::this_thread::sleep_for(50ms);
        return;
      }

#ifdef _WIN32
      WaitForSingleObject(shutdown_wakeup_event, INFINITE);
#else
      char token;
      while (::read(shutdown_wakeup_pipe[0], &token, sizeof(token)) < 0 && errno == EINTR) {}
#endif
    };

    while (!stop_token.stop_requested()) {
      wait_for_wakeup();

      if (stop_token.stop_requested()) {
        return;
      }

      const int signal_number = received_shutdown_signal.exchange(0, std::memory_order_relaxed);
      if (!signal_number) {
        // Woken by request_stop() or a duplicate wakeup.
        continue;
      }

      BOOST_LOG(info) << (signal_number == SIGINT ? "Interrupt handler called"sv : "Terminate handler called"sv);

      // Break out of the main loop immediately so handlers and the startup block stop using
      // display and encoder state.
      shutdown_event->raise(true);

      // Arm the forced-shutdown timer only after startup initialization finishes, or after a
      // bounded grace period when initialization is stuck. Waking periodically lets request_stop()
      // interrupt the wait when the process is exiting through an early-return path.
      {
        std::unique_lock lock {startup_completion_mutex};
        const auto startup_deadline = std::chrono::steady_clock::now() + 10s;
        while (!startup_completed.load(std::memory_order_acquire) &&
               !stop_token.stop_requested() &&
               std::chrono::steady_clock::now() < startup_deadline) {
          startup_completion_cv.wait_for(lock, 100ms);
        }
      }

      auto task = []() {
        BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
        logging::log_flush();
        lifetime::debug_trap();
      };
      force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

      if (tray_is_enabled && config::sunshine.system_tray) {
        system_tray::end_tray();
      }

      reset_display_guard();
      return;
    }
  }};

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  // Capture the pre-request state so access granted during this launch causes
  // one clean restart after every verifiable required permission is available.
  const bool permission_restart_needed = !platf::required_permissions_granted(platf::get_permission_statuses());
#ifdef __APPLE__
  platf::request_startup_permissions(tray_is_enabled && config::sunshine.system_tray);
#endif

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

    initialize_display();

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Sunshine successfully, use the shortcut in the Start Menu. Do not run Sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    mDNS = platf::publish::start();
  });

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    initialize_display();
    return lifetime::desired_exit_code;
  }

  std::jthread httpThread {nvhttp::start};
  std::jthread configThread {confighttp::start};
  std::jthread rtspThread {rtsp_stream::start};

  BOOST_LOG(info) << "Startup: starting HTTP servers; continuing display and encoder initialization"sv;

  const auto startup_init_begin = std::chrono::steady_clock::now();

  // Keep the readiness scope small: the guard marks startup ready when this block exits,
  // including early returns and exceptions, so request handlers never stay gated forever.
  {
    auto startup_ready_guard = util::fail_guard([&]() {
      video::mark_startup_ready();
      mark_startup_completed();
      const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startup_init_begin).count();
      BOOST_LOG(info) << "Startup: streaming initialization complete in "sv << elapsed << "ms"sv;
    });

    // Perform the display recovery even when shutdown was already requested. It used to run
    // unconditionally before the Web UI could be reached, and skipping it can leave the display
    // in the state left by a previous crash.
    initialize_display();

    {
      // Hold the probe lock across the shutdown check and the probes so the shutdown thread
      // cannot release the display guard between them.
      std::lock_guard probe_lock {startup_probe_mutex};

      if (shutdown_event->peek()) {
        // Do not probe encoders or input devices while shutting down. Probing after the display
        // guard has been released bypasses the no-active-device protection and can delay exit
        // past the forced-shutdown deadline.
        auto guard = take_display_guard();
        guard.reset();
      } else {
        if (config::input.controller && input::probe_gamepads()) {
          BOOST_LOG(warning) << "No gamepad input is available"sv;
        }

        if (video::probe_encoders()) {
          BOOST_LOG(error) << "Video failed to find working encoder"sv;
        }
      }
    }

    {
      // Shutdown may have been requested while probing. Release the display guard now instead of
      // waiting for the end of main; the probe lock keeps this ordered with the shutdown thread.
      std::lock_guard probe_lock {startup_probe_mutex};
      if (shutdown_event->peek()) {
        auto guard = take_display_guard();
        guard.reset();
      }
    }
  }

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Sunshine!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Sunshine Web UI."sv;
  }
#endif

  if (!shutdown_event->peek() && tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
#if defined(_WIN32) || defined(__APPLE__)
    system_tray::prepare_tray_virtualhid_license();
    system_tray::prepare_tray_virtualhid_driver();
#endif
#ifdef _WIN32
    // TODO: Windows has a weird bug where when running as a service and on the first Windows boot,
    // the tray icon would not appear even though Sunshine is running correctly otherwise.
    // Restarting the service would allow the icon to appear normally.
    // For now we will keep the Windows tray icon on a separate thread.
    // Ideally, we would run the system tray on the main thread for all platforms.
    system_tray::init_tray_threaded();
#else
    system_tray::init_tray();
#endif
  }

#ifdef __APPLE__
  std::jthread macos_audio_permission_requester;
  if (!permission_restart_needed) {
    macos_audio_permission_requester = std::jthread([]() {
      platf::request_startup_system_audio_permission();
    });
  }
#endif

  std::jthread permission_watcher;
  if (permission_restart_needed) {
    permission_watcher = std::jthread([](std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (platf::required_permissions_granted(platf::get_permission_statuses())) {
          BOOST_LOG(info) << "Required permissions granted; restarting Sunshine"sv;
          platf::restart();
          return;
        }
        std::this_thread::sleep_for(2s);
      }
    });
  }

  // The shutdown thread may have run end_tray() before the tray was initialized, in which case no
  // later tray_exit() will wake the tray loop. Do not enter it after shutdown was requested.
  if (!shutdown_event->peek()) {
    mainThreadLoop(shutdown_event);
  }

  if (tray_is_enabled && config::sunshine.system_tray) {
    // Stop a tray that was initialized after the shutdown thread's end_tray() call. Repeating
    // end_tray() is safe and also covers a tray loop that exited on its own.
    system_tray::end_tray();
  }

  permission_watcher.request_stop();
  if (permission_watcher.joinable()) {
    permission_watcher.join();
  }
#ifdef __APPLE__
  if (macos_audio_permission_requester.joinable()) {
    macos_audio_permission_requester.join();
  }
#endif

  httpThread.join();
  configThread.join();
  rtspThread.join();

  // Stop the shutdown thread before the task pool so the forced-shutdown timer stays armed while
  // end_tray() and display-guard teardown can block.
  shutdown_thread.request_stop();
  wake_shutdown_thread();
  if (shutdown_thread.joinable()) {
    shutdown_thread.join();
  }

  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  {
    auto guard = take_display_guard();
    guard.reset();
  }

  return lifetime::desired_exit_code;
}

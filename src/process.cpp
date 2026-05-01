/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/token_functions.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "system_tray.h"
#include "utility.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/utf_utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
    std::vector<std::string> parts;
    try {
#ifdef _WIN32
      parts = boost::program_options::split_winmain(cmd);
#else
      parts = boost::program_options::split_unix(cmd);
#endif
    } catch (boost::escaped_list_error &err) {
      BOOST_LOG(error) << "Boost failed to parse command ["sv << cmd << "] because " << err.what();
      return boost::filesystem::path();
    }
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });

    if (iter == _apps.end()) {
      BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
      return 404;
    }

    if (iter->capture_mode == "vdd") {
      return execute_vdd(app_id, *iter, launch_session);
    }

    // Non-VDD streams keep Sunshine's normal single-app/desktop behavior.
    terminate();

    _app_id = app_id;
    _app = *iter;
    launch_session->show_cursor = _app.show_cursor;
    launch_session->app_streaming = _app.app_streaming;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    // Add Stream-specific environment variables
    _env["SUNSHINE_APP_ID"] = std::to_string(_app_id);
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    _env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = utf_utils::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      terminate();
    });

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait(ec);
      if (ec) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] wait failed with error code ["sv << ec << ']';
        return -1;
      }
      auto ret = child.exit_code();
      if (ret != 0) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] exited with code ["sv << ret << ']';
        return -1;
      }
    }

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (_app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << _app.cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, _app.cmd, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << _app.cmd << "]: System: "sv << ec.message();
        return -1;
      }

      _app_launch_time = std::chrono::steady_clock::now();
    }

    fg.disable();

    return 0;
  }

  int proc_t::execute_vdd(int app_id, const ctx_t &app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
#ifndef _WIN32
    BOOST_LOG(error) << "VDD app streaming is only supported on Windows"sv;
    return -1;
#else
    launch_session->show_cursor = app.show_cursor;
    launch_session->app_streaming = app.app_streaming;

    if (app.cmd.empty() && !app.attach_existing) {
      BOOST_LOG(error) << "VDD app streaming requires a launch command unless attach-existing is enabled"sv;
      return -1;
    }

    std::error_code ec;
    auto env = _env;
    env["SUNSHINE_APP_ID"] = std::to_string(app_id);
    env["SUNSHINE_APP_NAME"] = app.name;
    env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    file_t pipe;
    if (!app.output.empty() && app.output != "null"sv) {
      auto woutput = utf_utils::from_utf8(app.output);
      pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
    }

    for (auto &cmd : app.prep_cmds) {
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Executing VDD Do Cmd: ["sv << cmd.do_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, env, pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        return -1;
      }

      child.wait(ec);
      if (ec || child.exit_code() != 0) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed for VDD app launch"sv;
        return -1;
      }
    }

    for (auto &cmd : app.detached) {
      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(cmd, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Spawning VDD detached command ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(app.elevated, true, cmd, working_dir, env, pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    boost::process::v1::child process;
    boost::process::v1::group process_group;
    std::chrono::steady_clock::time_point launch_time {};
    bool placebo = app.attach_existing || app.auto_detach;

    if (!app.cmd.empty() && !app.attach_existing) {
      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(app.cmd, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Executing VDD app: ["sv << app.cmd << "] in ["sv << working_dir << ']';
      process = platf::run_command(app.elevated, true, app.cmd, working_dir, env, pipe.get(), ec, &process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << app.cmd << "]: System: "sv << ec.message();
        return -1;
      }

      launch_time = std::chrono::steady_clock::now();
    }

    std::string output_name;
    auto match_hint = app.window_match.empty() ? app.name : app.window_match;
    auto root_process_id = process ? (std::uint32_t) process.id() : 0;
    BOOST_LOG(info) << "Preparing VDD app session ["sv << app.name
                    << "] match=["sv << match_hint
                    << "] client="sv << launch_session->width << 'x' << launch_session->height
                    << " attach_existing="sv << (app.attach_existing ? "true"sv : "false"sv);

    auto cleanup = util::fail_guard([&]() {
      if (!output_name.empty()) {
        platf::release_vdd_app(output_name);
      }
      terminate_process_group(process, process_group, app.exit_timeout);
    });

    if (!platf::prepare_vdd_app(root_process_id, match_hint, launch_session->width, launch_session->height, app.window_borderless, app.window_follow, 60s, output_name)) {
      BOOST_LOG(error) << "Failed to prepare VDD app stream for ["sv << app.name << ']';
      return -1;
    }

    launch_session->output_name = output_name;
    BOOST_LOG(info) << "App session capture output = ["sv << launch_session->output_name << ']';

    auto session_app = std::make_unique<session_app_t>(session_app_t {
      app_id,
      app,
      launch_time,
      placebo,
      output_name,
      std::move(process),
      std::move(process_group),
    });

    {
      std::lock_guard lock {_session_apps->mutex};
      _session_apps->apps[launch_session->id] = std::move(session_app);
    }

    cleanup.disable();
    return 0;
#endif
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && _process.native_exit_code() == 0 &&
               std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited gracefully within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;
      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << ']';
      terminate();
    }

    std::vector<std::uint32_t> ended_session_apps;
    int active_session_app_id = 0;
    {
      std::lock_guard lock {_session_apps->mutex};
      for (auto &[launch_session_id, session_app] : _session_apps->apps) {
        bool is_running = session_app->placebo;
        if (!is_running && session_app->app.wait_all && session_app->process_group && platf::process_group_running((std::uintptr_t) session_app->process_group.native_handle())) {
          is_running = true;
        } else if (!is_running && session_app->process.running()) {
          is_running = true;
        } else if (!is_running && session_app->app.auto_detach && session_app->process.native_exit_code() == 0 &&
                   std::chrono::steady_clock::now() - session_app->launch_time < 5s) {
          BOOST_LOG(info) << "VDD app exited gracefully within 5 seconds of launch. Treating it as detached."sv;
          session_app->placebo = true;
          is_running = true;
        }

        if (is_running) {
          if (active_session_app_id == 0) {
            active_session_app_id = session_app->app_id;
          }
        } else {
          ended_session_apps.push_back(launch_session_id);
        }
      }
    }

    for (auto launch_session_id : ended_session_apps) {
      terminate_session(launch_session_id);
    }

    if (active_session_app_id > 0) {
      return active_session_app_id;
    }

    return 0;
  }

  void proc_t::terminate_session(std::uint32_t launch_session_id) {
    std::unique_ptr<session_app_t> session_app;
    {
      std::lock_guard lock {_session_apps->mutex};
      auto it = _session_apps->apps.find(launch_session_id);
      if (it == _session_apps->apps.end()) {
        return;
      }

      session_app = std::move(it->second);
      _session_apps->apps.erase(it);
    }

    BOOST_LOG(info) << "Cleaning up VDD app session ["sv << session_app->app.name
                    << "] launch_session="sv << launch_session_id;

    if (session_app->app.terminate_on_disconnect && !session_app->app.attach_existing) {
      terminate_process_group(session_app->process, session_app->process_group, session_app->app.exit_timeout);
    }

    if (!session_app->output_name.empty()) {
      platf::release_vdd_app(session_app->output_name);
    }
  }

  void proc_t::terminate() {
    std::vector<std::uint32_t> session_ids;
    {
      std::lock_guard lock {_session_apps->mutex};
      session_ids.reserve(_session_apps->apps.size());
      for (auto &[launch_session_id, session_app] : _session_apps->apps) {
        session_ids.push_back(launch_session_id);
      }
    }

    for (auto launch_session_id : session_ids) {
      terminate_session(launch_session_id);
    }

    std::error_code ec;
    placebo = false;
    terminate_process_group(_process, _process_group, _app.exit_timeout);
    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif

      display_device::revert_configuration();
    }

    _app_id = -1;
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  bool proc_t::app_allows_parallel_launch(int app_id) const {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto &app) {
      return app.id == std::to_string(app_id);
    });

    return iter != _apps.end() && iter->capture_mode == "vdd";
  }

  bool proc_t::app_window_ready(int app_id) const {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto &app) {
      return app.id == std::to_string(app_id);
    });

    if (iter == _apps.end() || iter->capture_mode != "vdd" || !iter->attach_existing) {
      return false;
    }

    {
      std::lock_guard lock {_session_apps->mutex};
      for (const auto &[_, session_app] : _session_apps->apps) {
        if (session_app->app_id == app_id) {
          return false;
        }
      }
    }

    auto match_hint = iter->window_match.empty() ? iter->name : iter->window_match;
    return platf::is_vdd_app_window_ready(match_hint);
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  const ctx_t &proc_t::get_running_app() const {
    return _app;
  }

  std::string proc_t::get_last_run_app_name() {
    return _app.name;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  /**
   * @brief Validates a path whether it is a valid PNG.
   * @param path The path to the PNG file.
   * @return true if the file has a valid PNG signature, false otherwise.
   */
  bool check_valid_png(const std::filesystem::path &path) {
    // PNG signature as defined in PNG specification
    // http://www.libpng.org/pub/png/spec/1.2/PNG-Structure.html
    static constexpr std::array<unsigned char, 8> PNG_SIGNATURE = {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }

    std::array<unsigned char, 8> header;
    file.read(reinterpret_cast<char *>(header.data()), 8);

    if (file.gcount() != 8) {
      return false;
    }

    return header == PNG_SIGNATURE;
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if the extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    if (auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path; std::filesystem::exists(full_image_path)) {
      // Validate PNG signature
      if (!check_valid_png(full_image_path)) {
        BOOST_LOG(warning) << "Invalid PNG file at path ["sv << full_image_path << ']';
        return DEFAULT_APP_IMAGE_PATH;
      }
      return full_image_path.string();
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    if (std::error_code code; !std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // Validate PNG signature
    if (!check_valid_png(app_image_path)) {
      BOOST_LOG(warning) << "Invalid PNG file at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {
    pt::ptree tree;

    try {
      pt::read_json(file_name, tree);

      auto &apps_node = tree.get_child("apps"s);
      auto &env_vars = tree.get_child("env"s);

      auto this_env = boost::this_process::environment();

      for (auto &[name, val] : env_vars) {
        this_env[name] = parse_env_val(this_env, val.get_value<std::string>());
      }

      std::set<std::string> ids;
      std::vector<proc::ctx_t> apps;
      int i = 0;
      for (auto &[_, app_node] : apps_node) {
        proc::ctx_t ctx;

        auto prep_nodes_opt = app_node.get_child_optional("prep-cmd"s);
        auto detached_nodes_opt = app_node.get_child_optional("detached"s);
        auto exclude_global_prep = app_node.get_optional<bool>("exclude-global-prep-cmd"s);
        auto output = app_node.get_optional<std::string>("output"s);
        auto name = parse_env_val(this_env, app_node.get<std::string>("name"s));
        auto cmd = app_node.get_optional<std::string>("cmd"s);
        auto image_path = app_node.get_optional<std::string>("image-path"s);
        auto working_dir = app_node.get_optional<std::string>("working-dir"s);
        auto elevated = app_node.get_optional<bool>("elevated"s);
        auto auto_detach = app_node.get_optional<bool>("auto-detach"s);
        auto wait_all = app_node.get_optional<bool>("wait-all"s);
        auto terminate_on_disconnect = app_node.get_optional<bool>("terminate-on-disconnect"s);
        auto exit_timeout = app_node.get_optional<int>("exit-timeout"s);
        auto capture_mode = app_node.get_optional<std::string>("capture-mode"s);
        auto window_match = app_node.get_optional<std::string>("window-match"s);
        auto window_resolution = app_node.get_optional<std::string>("window-resolution"s);
        auto stream_resolution = app_node.get_optional<std::string>("stream-resolution"s);
        auto client_display_mode = app_node.get_optional<std::string>("client-display-mode"s);
        auto auto_spawn_from = app_node.get_optional<std::string>("auto-spawn-from"s);
        auto client_app_window = app_node.get_optional<bool>("client-app-window"s);
        auto client_absolute_mouse = app_node.get_optional<bool>("client-absolute-mouse"s);
        auto window_borderless = app_node.get_optional<bool>("window-borderless"s);
        auto attach_existing = app_node.get_optional<bool>("attach-existing"s);
        auto window_follow = app_node.get_optional<bool>("window-follow"s);
        auto show_cursor = app_node.get_optional<bool>("show-cursor"s);

        std::vector<proc::cmd_t> prep_cmds;
        if (!exclude_global_prep.value_or(false)) {
          prep_cmds.reserve(config::sunshine.prep_cmds.size());
          for (auto &prep_cmd : config::sunshine.prep_cmds) {
            auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
            auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);

            prep_cmds.emplace_back(
              std::move(do_cmd),
              std::move(undo_cmd),
              std::move(prep_cmd.elevated)
            );
          }
        }

        if (prep_nodes_opt) {
          auto &prep_nodes = *prep_nodes_opt;

          prep_cmds.reserve(prep_cmds.size() + prep_nodes.size());
          for (auto &[_, prep_node] : prep_nodes) {
            auto do_cmd = prep_node.get_optional<std::string>("do"s);
            auto undo_cmd = prep_node.get_optional<std::string>("undo"s);
            auto elevated = prep_node.get_optional<bool>("elevated");

            prep_cmds.emplace_back(
              parse_env_val(this_env, do_cmd.value_or("")),
              parse_env_val(this_env, undo_cmd.value_or("")),
              std::move(elevated.value_or(false))
            );
          }
        }

        std::vector<std::string> detached;
        if (detached_nodes_opt) {
          auto &detached_nodes = *detached_nodes_opt;

          detached.reserve(detached_nodes.size());
          for (auto &[_, detached_val] : detached_nodes) {
            detached.emplace_back(parse_env_val(this_env, detached_val.get_value<std::string>()));
          }
        }

        if (output) {
          ctx.output = parse_env_val(this_env, *output);
        }

        if (cmd) {
          ctx.cmd = parse_env_val(this_env, *cmd);
        }

        if (working_dir) {
          ctx.working_dir = parse_env_val(this_env, *working_dir);
#ifdef _WIN32
          // The working directory, unlike the command itself, should not be quoted
          // when it contains spaces. Unlike POSIX, Windows forbids quotes in paths,
          // so we can safely strip them all out here to avoid confusing the user.
          boost::erase_all(ctx.working_dir, "\"");
#endif
        }

        if (image_path) {
          ctx.image_path = parse_env_val(this_env, *image_path);
        }

        ctx.elevated = elevated.value_or(false);
        ctx.auto_detach = auto_detach.value_or(true);
        ctx.wait_all = wait_all.value_or(true);
        ctx.exit_timeout = std::chrono::seconds {exit_timeout.value_or(5)};
        ctx.capture_mode = capture_mode.value_or("");
        ctx.window_match = window_match.value_or("");
        ctx.window_resolution = window_resolution.value_or("");
        ctx.app_streaming = ctx.capture_mode == "window" || ctx.capture_mode == "vdd";
        ctx.stream_resolution = ctx.app_streaming ? stream_resolution.value_or("") : "";
        ctx.client_display_mode = ctx.app_streaming ? client_display_mode.value_or("") : "";
        ctx.auto_spawn_from = ctx.app_streaming ? auto_spawn_from.value_or("") : "";
        ctx.client_app_window_set = ctx.app_streaming && client_app_window.has_value();
        ctx.client_app_window = ctx.app_streaming && client_app_window.value_or(false);
        ctx.client_absolute_mouse_set = ctx.app_streaming && client_absolute_mouse.has_value();
        ctx.client_absolute_mouse = ctx.app_streaming && client_absolute_mouse.value_or(false);
        ctx.terminate_on_disconnect = terminate_on_disconnect.value_or(ctx.app_streaming);
        ctx.window_borderless = window_borderless.value_or(ctx.capture_mode == "vdd");
        ctx.attach_existing = attach_existing.value_or(false);
        ctx.window_follow = window_follow.value_or(ctx.capture_mode == "vdd");
        ctx.show_cursor = show_cursor.value_or(config::video.show_cursor);

        auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        } else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        ctx.name = std::move(name);
        ctx.prep_cmds = std::move(prep_cmds);
        ctx.detached = std::move(detached);

        apps.emplace_back(std::move(ctx));
      }

      return proc::proc_t {
        std::move(this_env),
        std::move(apps)
      };
    } catch (std::exception &e) {
      BOOST_LOG(error) << e.what();
    }

    return std::nullopt;
  }

  void refresh(const std::string &file_name) {
    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc

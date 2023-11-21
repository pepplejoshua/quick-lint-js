// Copyright (C) 2020  Matthew "strager" Glazar
// See end of file for extended copyright information.

#if defined(__EMSCRIPTEN__)
// No filesystem on the web.
#else

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <quick-lint-js/cli/options.h>
#include <quick-lint-js/configuration/basic-configuration-filesystem.h>
#include <quick-lint-js/configuration/change-detecting-filesystem.h>
#include <quick-lint-js/configuration/configuration-loader.h>
#include <quick-lint-js/configuration/configuration.h>
#include <quick-lint-js/container/fixed-vector.h>
#include <quick-lint-js/container/hash-set.h>
#include <quick-lint-js/fake-configuration-filesystem.h>
#include <quick-lint-js/file-matcher.h>
#include <quick-lint-js/filesystem-test.h>
#include <quick-lint-js/io/event-loop.h>
#include <quick-lint-js/io/file-canonical.h>
#include <quick-lint-js/io/file-path.h>
#include <quick-lint-js/io/file.h>
#include <quick-lint-js/mock-inotify.h>
#include <quick-lint-js/mock-kqueue.h>
#include <quick-lint-js/mock-win32.h>
#include <quick-lint-js/permissions.h>
#include <quick-lint-js/port/thread.h>
#include <quick-lint-js/port/warning.h>
#include <quick-lint-js/port/windows-error.h>
#include <string>
#include <string_view>
#include <vector>

#if QLJS_HAVE_STD_FILESYSTEM
#include <filesystem>
#endif

#if QLJS_HAVE_POLL
#include <poll.h>
#endif

#if QLJS_HAVE_KQUEUE
#include <sys/event.h>
#endif

#if QLJS_HAVE_UNISTD_H
#include <unistd.h>
#endif

#if QLJS_HAVE_WINDOWS_H
#include <quick-lint-js/port/windows.h>
#endif

QLJS_WARNING_IGNORE_GCC("-Wmissing-field-initializers")

using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using namespace std::literals::string_view_literals;

namespace quick_lint_js {
namespace {
void move_file(const std::string& from, const std::string& to);

class Change_Detecting_Configuration_Loader {
 public:
#if QLJS_HAVE_KQUEUE
  enum Event_UData : std::uintptr_t {
    event_udata_invalid = 0,
    event_udata_fs_changed,
  };
#endif

#if defined(_WIN32)
  enum Completion_Key : ULONG_PTR {
    completion_key_invalid = 0,
    completion_key_stop,
    completion_key_fs_changed,
  };
#endif

  explicit Change_Detecting_Configuration_Loader()
      :
#if QLJS_HAVE_INOTIFY
        fs_(),
#elif QLJS_HAVE_KQUEUE
        kqueue_fd_(::kqueue()),
        fs_(this->kqueue_fd_.ref(),
            reinterpret_cast<void*>(event_udata_fs_changed)),
#elif defined(_WIN32)
        io_completion_port_(create_io_completion_port()),
        fs_(this->io_completion_port_.ref(), completion_key_fs_changed),
        io_thread_([this]() { this->run_io_thread(); }),
#else
#error "Unsupported platform"
#endif
        loader_(&this->fs_) {
  }

  ~Change_Detecting_Configuration_Loader() {
#if defined(_WIN32)
    this->stop_io_thread();
    this->io_thread_.join();
#endif
  }

  template <class... Args>
  auto watch_and_load_for_file(Args&&... args) {
#if defined(_WIN32)
    std::lock_guard<Mutex> lock(this->mutex_);
#endif
    return this->loader_.watch_and_load_for_file(std::forward<Args>(args)...);
  }

  template <class... Args>
  auto watch_and_load_config_file(Args&&... args) {
#if defined(_WIN32)
    std::lock_guard<Mutex> lock(this->mutex_);
#endif
    return this->loader_.watch_and_load_config_file(
        std::forward<Args>(args)...);
  }

  template <class... Args>
  auto unwatch_file(Args&&... args) {
#if defined(_WIN32)
    std::lock_guard<Mutex> lock(this->mutex_);
#endif
    return this->loader_.unwatch_file(std::forward<Args>(args)...);
  }

  template <class... Args>
  auto unwatch_all_files() {
#if defined(_WIN32)
    std::lock_guard<Mutex> lock(this->mutex_);
#endif
    return this->loader_.unwatch_all_files();
  }

  auto fs_take_watch_errors() {
#if defined(_WIN32)
    std::lock_guard<Mutex> lock(this->mutex_);
#endif
    return this->fs_.take_watch_errors();
  }

  Span<Configuration_Change> detect_changes_and_refresh();

 private:
  bool detect_changes();
#if defined(_WIN32)
  bool detect_changes_locked(std::unique_lock<Mutex>&);
#endif

#if defined(_WIN32)
  // On Windows, we pump events on a separate thread. This is because
  // std::rename blocks the thread waiting for the oplock to break, but we need
  // to call Change_Detecting_Filesystem_Win32::handle_event in order to break
  // the oplock and unblock std::rename.
  void run_io_thread();
  void stop_io_thread();
#endif

#if QLJS_HAVE_INOTIFY
  Change_Detecting_Filesystem_Inotify fs_;
#elif QLJS_HAVE_KQUEUE
  POSIX_FD_File kqueue_fd_;
  Change_Detecting_Filesystem_Kqueue fs_;
#elif defined(_WIN32)
  Windows_Handle_File io_completion_port_;
  Mutex mutex_;
  Condition_Variable io_thread_timed_out_;
  Condition_Variable fs_changed_;

  // Used by the test thread only:
  unsigned long long old_fs_changed_count_ = 0;

  // Protected by mutex_:
  Change_Detecting_Filesystem_Win32 fs_;
  unsigned long long io_thread_timed_out_count_ = 0;
  unsigned long long fs_changed_count_ = 0;

  std::thread io_thread_;
#endif

  // Protected by mutex_ if present:
  Configuration_Loader loader_;
  Monotonic_Allocator allocator{"Change_Detecting_Configuration_Loader"};
};

class Test_Configuration_Loader : public ::testing::Test,
                                  protected Filesystem_Test {};

TEST_F(Test_Configuration_Loader,
       file_with_no_config_file_gets_default_config) {
  // NOTE(strager): This test assumes that there is no quick-lint-js.config file
  // in /tmp or in /.
  std::string temp_dir = this->make_temporary_directory();

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  std::string js_file = temp_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = js_file.c_str(),
      .config_file = nullptr,
  });
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_EQ(*loaded_config, nullptr);
}

TEST_F(Test_Configuration_Loader, find_quick_lint_js_config_in_same_directory) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({})"_sv);

  std::string js_file = temp_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       find_config_in_same_directory_of_relative_path) {
  std::string temp_dir = this->make_temporary_directory();
  this->set_current_working_directory(temp_dir);
  std::string config_file = "quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  std::string js_file = "hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader, quick_lint_js_config_directory_fails) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  create_directory_or_exit(config_file);

  std::string js_file = temp_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());

  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_FALSE(loaded_config.ok());
  Configuration_Load_IO_Error e = loaded_config.error();
  EXPECT_EQ(e.path, canonicalize_path(config_file)->c_str());
#if QLJS_HAVE_WINDOWS_H
  EXPECT_EQ(e.io_error.error, ERROR_ACCESS_DENIED)
      << windows_error_message(e.io_error.error);
#endif
#if QLJS_HAVE_UNISTD_H
  EXPECT_EQ(e.io_error.error, EISDIR) << std::strerror(e.io_error.error);
#endif
}

TEST_F(Test_Configuration_Loader, find_config_in_parent_directory) {
  std::string temp_dir = this->make_temporary_directory();
  create_directory_or_exit(temp_dir + "/dir");
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  std::string js_file = temp_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       find_config_in_parent_directory_of_relative_path) {
  std::string temp_dir = this->make_temporary_directory();
  this->set_current_working_directory(temp_dir);
  create_directory_or_exit("dir");
  std::string config_file = "quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  std::string js_file = "dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader, find_config_in_parent_directory_of_cwd) {
  std::string temp_dir = this->make_temporary_directory();
  create_directory_or_exit(temp_dir + "/dir");
  this->set_current_working_directory(temp_dir + "/dir");
  std::string config_file = "../quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  std::string js_file = "hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader, find_config_in_ancestor_directory) {
  std::string temp_dir = this->make_temporary_directory();
  create_directory_or_exit(temp_dir + "/a");
  create_directory_or_exit(temp_dir + "/a/b");
  create_directory_or_exit(temp_dir + "/a/b/c");
  create_directory_or_exit(temp_dir + "/a/b/c/d");
  create_directory_or_exit(temp_dir + "/a/b/c/d/e");
  create_directory_or_exit(temp_dir + "/a/b/c/d/e/f");
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  std::string js_file = temp_dir + "/a/b/c/d/e/f/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       dot_dot_component_is_resolved_before_finding) {
  std::string temp_dir = this->make_temporary_directory();
  create_directory_or_exit(temp_dir + "/dir");
  create_directory_or_exit(temp_dir + "/dir/subdir");
  std::string config_file_outside_dir = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file_outside_dir, u8"{}"_sv);
  std::string config_file_inside_subdir =
      temp_dir + "/dir/subdir/quick-lint-js.config";
  write_file_or_exit(config_file_inside_subdir, u8"{}"_sv);

  // Valid search path order:
  // * $temp_dir/dir/quick-lint-js.config
  // * $temp_dir/quick-lint-js.config
  //
  // Invalid search path order:
  // * $temp_dir/dir/quick-lint-js.config
  //   (i.e. $temp_dir/dir/subdir/../quick-lint-js.config)
  // * $temp_dir/dir/subdir/quick-lint-js.config -- wrong; shouldn't be searched
  // * $temp_dir/dir/quick-lint-js.config
  // * $temp_dir/quick-lint-js.config

  std::string js_file = temp_dir + "/dir/subdir/../hello.js";
  write_file_or_exit(js_file, u8""_sv);
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file_outside_dir);
}

TEST_F(Test_Configuration_Loader, find_no_config_if_stdin) {
  std::string temp_dir = this->make_temporary_directory();
  this->set_current_working_directory(temp_dir);
  std::string config_file = "quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = "<stdin>",
      .config_file = nullptr,
      .is_stdin = true,
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_EQ(*loaded_config, nullptr)
      << "load_for_file should not search in the current working directory";
}

TEST_F(Test_Configuration_Loader,
       find_config_file_in_directory_given_missing_path_for_config_search) {
  std::string config_project_dir = this->make_temporary_directory();
  std::string config_file = config_project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  std::string js_project_dir = this->make_temporary_directory();
  std::string js_file = js_project_dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = js_file.c_str(),
      .config_file = nullptr,
      .path_for_config_search =
          (config_project_dir + "/does-not-exist.js").c_str(),
      .is_stdin = false,
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  ASSERT_NE(*loaded_config, nullptr);
  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       find_config_file_in_directory_given_path_for_config_search_for_stdin) {
  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);
  std::string js_file = project_dir + "/test.js";
  write_file_or_exit(js_file, u8"{}"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = "<stdin>",
      .config_file = nullptr,
      .path_for_config_search = js_file.c_str(),
      .is_stdin = true,
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  ASSERT_NE(*loaded_config, nullptr);
  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader, file_with_config_file_gets_loaded_config) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/config.json";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = "hello.js",
      .config_file = config_file.c_str(),
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_TRUE(
      (*loaded_config)->config.globals().find(u8"testGlobalVariable"_sv));
  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       files_with_same_config_file_get_same_loaded_config) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/config.json";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config_one = loader.load_for_file(File_To_Lint{
      .path = "one.js",
      .config_file = config_file.c_str(),
  });
  EXPECT_TRUE(loaded_config_one.ok()) << loaded_config_one.error_to_string();
  auto loaded_config_two = loader.load_for_file(File_To_Lint{
      .path = "two.js",
      .config_file = config_file.c_str(),
  });
  EXPECT_TRUE(loaded_config_two.ok()) << loaded_config_two.error_to_string();

  EXPECT_EQ(*loaded_config_one, *loaded_config_two)
      << "pointers should be the same";
}

TEST_F(Test_Configuration_Loader,
       files_with_different_config_files_get_different_loaded_config) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file_one = temp_dir + "/config-one.json";
  write_file_or_exit(config_file_one,
                     u8R"({"globals": {"testGlobalVariableOne": true}})"_sv);
  std::string config_file_two = temp_dir + "/config-two.json";
  write_file_or_exit(config_file_two,
                     u8R"({"globals": {"testGlobalVariableTwo": true}})"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config_one = loader.load_for_file(File_To_Lint{
      .path = "one.js",
      .config_file = config_file_one.c_str(),
  });
  EXPECT_TRUE(loaded_config_one.ok()) << loaded_config_one.error_to_string();
  auto loaded_config_two = loader.load_for_file(File_To_Lint{
      .path = "two.js",
      .config_file = config_file_two.c_str(),
  });
  EXPECT_TRUE(loaded_config_two.ok()) << loaded_config_two.error_to_string();

  Configuration* config_one = &(*loaded_config_one)->config;
  Configuration* config_two = &(*loaded_config_two)->config;
  EXPECT_NE(config_one, config_two) << "pointers should be different";

  EXPECT_TRUE(config_one->globals().find(u8"testGlobalVariableOne"_sv));
  EXPECT_FALSE(config_one->globals().find(u8"testGlobalVariableTwo"_sv));
  EXPECT_SAME_FILE(*(*loaded_config_one)->config_path, config_file_one);

  EXPECT_FALSE(config_two->globals().find(u8"testGlobalVariableOne"_sv));
  EXPECT_TRUE(config_two->globals().find(u8"testGlobalVariableTwo"_sv));
  EXPECT_SAME_FILE(*(*loaded_config_two)->config_path, config_file_two);
}

TEST_F(Test_Configuration_Loader, missing_config_file_fails) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/config.json";

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());

  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = "hello.js",
      .config_file = config_file.c_str(),
  });
  ASSERT_FALSE(loaded_config.ok());
  Configuration_Load_IO_Error e = loaded_config.error();
  EXPECT_EQ(e.path, canonicalize_path(config_file)->c_str());
#if QLJS_HAVE_WINDOWS_H
  EXPECT_EQ(e.io_error.error, ERROR_FILE_NOT_FOUND)
      << windows_error_message(e.io_error.error);
#endif
#if QLJS_HAVE_UNISTD_H
  EXPECT_EQ(e.io_error.error, ENOENT) << std::strerror(e.io_error.error);
#endif
}

TEST_F(Test_Configuration_Loader,
       found_quick_lint_js_config_is_loaded_only_once) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  std::string js_file_one = temp_dir + "/one.js";
  write_file_or_exit(js_file_one, u8""_sv);
  auto loaded_config_one = loader.load_for_file(File_To_Lint{
      .path = js_file_one.c_str(),
      .config_file = nullptr,
  });
  EXPECT_TRUE(loaded_config_one.ok()) << loaded_config_one.error_to_string();
  std::string js_file_two = temp_dir + "/two.js";
  write_file_or_exit(js_file_two, u8""_sv);
  auto loaded_config_two = loader.load_for_file(File_To_Lint{
      .path = js_file_two.c_str(),
      .config_file = nullptr,
  });
  EXPECT_TRUE(loaded_config_two.ok()) << loaded_config_two.error_to_string();

  EXPECT_EQ(*loaded_config_one, *loaded_config_two)
      << "pointers should be the same";
}

TEST_F(
    Test_Configuration_Loader,
    found_quick_lint_js_config_and_explicit_config_file_is_loaded_only_once) {
  {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/quick-lint-js.config";
    write_file_or_exit(config_file,
                       u8R"({"globals": {"testGlobalVariable": true}})"_sv);

    Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file_or_exit(js_file_one, u8""_sv);
    auto loaded_config_one = loader.load_for_file(File_To_Lint{
        .path = js_file_one.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(loaded_config_one.ok()) << loaded_config_one.error_to_string();
    std::string js_file_two = temp_dir + "/two.js";
    write_file_or_exit(js_file_two, u8""_sv);
    auto loaded_config_two = loader.load_for_file(File_To_Lint{
        .path = js_file_two.c_str(),
        .config_file = config_file.c_str(),
    });
    EXPECT_TRUE(loaded_config_two.ok()) << loaded_config_two.error_to_string();

    EXPECT_EQ(*loaded_config_one, *loaded_config_two)
        << "pointers should be the same";
  }

  {
    std::string temp_dir = this->make_temporary_directory();
    std::string config_file = temp_dir + "/quick-lint-js.config";
    write_file_or_exit(config_file,
                       u8R"({"globals": {"testGlobalVariable": true}})"_sv);

    Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
    std::string js_file_one = temp_dir + "/one.js";
    write_file_or_exit(js_file_one, u8""_sv);
    auto loaded_config_one = loader.load_for_file(File_To_Lint{
        .path = js_file_one.c_str(),
        .config_file = config_file.c_str(),
    });
    EXPECT_TRUE(loaded_config_one.ok()) << loaded_config_one.error_to_string();
    std::string js_file_two = temp_dir + "/two.js";
    write_file_or_exit(js_file_two, u8""_sv);
    auto loaded_config_two = loader.load_for_file(File_To_Lint{
        .path = js_file_two.c_str(),
        .config_file = nullptr,
    });
    EXPECT_TRUE(loaded_config_two.ok()) << loaded_config_two.error_to_string();

    EXPECT_EQ(*loaded_config_one, *loaded_config_two)
        << "pointers should be the same";
  }
}

TEST_F(Test_Configuration_Loader,
       finding_config_succeeds_even_if_file_is_missing) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({})"_sv);

  std::string js_file = temp_dir + "/hello.js";
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       finding_config_succeeds_even_if_directory_is_missing) {
  std::string temp_dir = this->make_temporary_directory();
  std::string config_file = temp_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({})"_sv);

  std::string js_file = temp_dir + "/dir/hello.js";
  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config = loader.load_for_file(js_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_SAME_FILE(*(*loaded_config)->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       deleting_parent_of_missing_file_is_not_detected_as_a_change) {
  std::string temp_dir = this->make_temporary_directory();
  std::string parent_dir = temp_dir + "/dir";
  create_directory_or_exit(parent_dir);

  std::string js_file = parent_dir + "/hello.js";
  Change_Detecting_Configuration_Loader loader;
  auto loaded_config = loader.watch_and_load_for_file(js_file, &js_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_EQ(::rmdir(parent_dir.c_str()), 0)
      << "failed to delete " << parent_dir << ": " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader, config_found_initially_is_unchanged) {
  {
    std::string project_dir = this->make_temporary_directory();
    std::string js_file = project_dir + "/hello.js";
    write_file_or_exit(js_file, u8""_sv);
    std::string config_file = project_dir + "/quick-lint-js.config";
    write_file_or_exit(config_file, u8"{}"_sv);

    Change_Detecting_Configuration_Loader loader;
    loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

    Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
    EXPECT_THAT(changes, IsEmpty());
  }
}

TEST_F(Test_Configuration_Loader,
       rewriting_config_completely_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  write_file_or_exit(config_file, u8R"({"globals": {"after": true}})"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       rewriting_config_partially_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  {
    FILE* file = std::fopen(config_file.c_str(), "r+");
    ASSERT_TRUE(file) << std::strerror(errno);
    ASSERT_EQ(
        std::fseek(file, narrow_cast<long>(u8R"({"globals": {")"_sv.size()),
                   SEEK_SET),
        0)
        << std::strerror(errno);
    ASSERT_EQ(std::fwrite(u8"after_", 1, 6, file), 6) << std::strerror(errno);
    ASSERT_EQ(std::fclose(file), 0) << std::strerror(errno);
  }

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       rewriting_config_back_to_original_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"a": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  write_file_or_exit(config_file, u8R"({"globals": {"b": true}})"_sv);
  write_file_or_exit(config_file, u8R"({"globals": {"a": true}})"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       renaming_file_over_config_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);
  create_directory_or_exit(project_dir + "/temp");
  std::string new_config_file = project_dir + "/temp/new-config";
  write_file_or_exit(new_config_file, u8R"({"globals": {"after": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(new_config_file, config_file);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       renaming_file_over_config_with_same_content_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);
  create_directory_or_exit(project_dir + "/temp");
  std::string new_config_file = project_dir + "/temp/new-config";
  write_file_or_exit(new_config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(new_config_file, config_file);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       moving_config_file_away_and_back_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  std::string temp_config_file = project_dir + "/temp.config";
  move_file(config_file, temp_config_file);
  move_file(temp_config_file, config_file);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader, creating_config_in_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       creating_config_in_same_dir_is_detected_if_file_doesnt_exit) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("hello.js"));
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader, creating_config_in_parent_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       creating_shadowing_config_in_child_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string outer_config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(outer_config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  std::string inner_config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(inner_config_file, u8"{}"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, inner_config_file);
}

TEST_F(Test_Configuration_Loader, deleting_config_in_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  EXPECT_EQ(std::remove(config_file.c_str()), 0)
      << "failed to delete " << config_file << ": " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
}

TEST_F(Test_Configuration_Loader,
       deleting_shadowing_config_in_child_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string outer_config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(outer_config_file, u8"{}"_sv);
  std::string inner_config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(inner_config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  EXPECT_EQ(std::remove(inner_config_file.c_str()), 0)
      << "failed to delete " << inner_config_file << ": "
      << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, outer_config_file);
}

TEST_F(Test_Configuration_Loader, moving_config_away_in_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(config_file, (project_dir + "/moved.config"));

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
}

TEST_F(Test_Configuration_Loader,
       moving_shadowing_config_away_in_child_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string outer_config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(outer_config_file, u8"{}"_sv);
  std::string inner_config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(inner_config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(inner_config_file, (project_dir + "/dir/moved.config"));

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, outer_config_file);
}

TEST_F(Test_Configuration_Loader, moving_config_into_same_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string temp_config_file = project_dir + "/temp.config";
  write_file_or_exit(temp_config_file, u8"{}"_sv);
  std::string renamed_config_file = project_dir + "/quick-lint-js.config";

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(temp_config_file, renamed_config_file);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, renamed_config_file);
}

TEST_F(Test_Configuration_Loader, moving_config_into_parent_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string temp_config_file = project_dir + "/temp.config";
  write_file_or_exit(temp_config_file, u8"{}"_sv);
  std::string renamed_config_file = project_dir + "/quick-lint-js.config";

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(temp_config_file, renamed_config_file);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, renamed_config_file);
}

TEST_F(Test_Configuration_Loader,
       moving_shadowing_config_into_child_dir_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/dir");
  std::string js_file = project_dir + "/dir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string outer_config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(outer_config_file, u8"{}"_sv);
  std::string temp_config_file = project_dir + "/dir/temp.config";
  write_file_or_exit(temp_config_file, u8"{}"_sv);
  std::string inner_config_file = project_dir + "/dir/quick-lint-js.config";

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file(temp_config_file, inner_config_file);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_SAME_FILE(*changes[0].watched_path, js_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, inner_config_file);
}

TEST_F(Test_Configuration_Loader,
       moving_directory_containing_file_and_config_unlinks_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/olddir");
  std::string js_file = project_dir + "/olddir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/olddir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file((project_dir + "/olddir"), (project_dir + "/newdir"));

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("hello.js"));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("olddir"));
  EXPECT_EQ(changes[0].config_file, nullptr) << "config should be removed";
}

TEST_F(Test_Configuration_Loader,
       moving_ancestor_directory_containing_file_and_config_unlinks_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/olddir");
  create_directory_or_exit(project_dir + "/olddir/subdir");
  std::string js_file = project_dir + "/olddir/subdir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/olddir/subdir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file((project_dir + "/olddir"), (project_dir + "/newdir"));

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("hello.js"));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("olddir"));
  EXPECT_EQ(changes[0].config_file, nullptr) << "config should be removed";
}

TEST_F(Test_Configuration_Loader,
       moving_directory_containing_file_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/olddir");
  std::string js_file = project_dir + "/olddir/hello.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file((project_dir + "/olddir"), (project_dir + "/newdir"));

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader, moving_file_keeps_config) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/oldfile.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  move_file((project_dir + "/oldfile.js"), (project_dir + "/newfile.js"));

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       creating_directory_of_watched_file_and_adding_config_is_detected) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/dir/test.js";

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  create_directory_or_exit(project_dir + "/dir");
  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty())
      << "creating dir should not change associated config file";

  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}))
      << "adding config should change associated config file";
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("test.js"));
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(
    Test_Configuration_Loader,
    creating_directory_of_watched_file_and_adding_config_is_detected_batched) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/dir/test.js";

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/nullptr);

  create_directory_or_exit(project_dir + "/dir");
  std::string config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_THAT(*changes[0].watched_path, ::testing::HasSubstr("test.js"));
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
}

TEST_F(Test_Configuration_Loader,
       creating_config_in_same_dir_as_many_watched_files_is_detected) {
  std::string project_dir = this->make_temporary_directory();

  Hash_Set<std::string> js_files;
  for (int i = 0; i < 10; ++i) {
    std::string js_file = project_dir + "/hello" + std::to_string(i) + ".js";
    write_file_or_exit(js_file, u8""_sv);
    auto [_iterator, inserted] = js_files.insert(std::move(js_file));
    ASSERT_TRUE(inserted) << "duplicate js_file: " << js_file;
  }

  Change_Detecting_Configuration_Loader loader;
  for (const std::string& js_file : js_files) {
    loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  }

  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  Hash_Set<std::string> unconfigured_js_files = js_files;
  for (const Configuration_Change& change : changes) {
    SCOPED_TRACE(*change.watched_path);
    EXPECT_TRUE(js_files.contains(*change.watched_path))
        << "change should report a watched file";
    const std::string* token =
        reinterpret_cast<const std::string*>(change.token);
    EXPECT_TRUE(js_files.contains(*token))
        << "change should have a valid token";
    EXPECT_EQ(unconfigured_js_files.erase(*change.watched_path), 1)
        << "change should report no duplicate watched files";
    EXPECT_SAME_FILE(*change.config_file->config_path, config_file);
  }
  EXPECT_THAT(unconfigured_js_files, IsEmpty())
      << "all watched files should have a config";
}

TEST_F(Test_Configuration_Loader,
       moving_config_file_and_changing_content_is_detected_as_one_change) {
  std::string project_dir = this->make_temporary_directory();

  std::string outer_js_file = project_dir + "/outer.js";
  write_file_or_exit(outer_js_file, u8""_sv);
  std::string outer_config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(outer_config_file,
                     u8R"({"globals": {"before": true}})"_sv);

  create_directory_or_exit(project_dir + "/dir");
  std::string inner_js_file = project_dir + "/dir/inner.js";
  write_file_or_exit(inner_js_file, u8""_sv);
  std::string inner_config_file = project_dir + "/dir/quick-lint-js.config";
  write_file_or_exit(inner_config_file, u8R"({"globals": {"inner": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(inner_js_file, /*token=*/&inner_js_file);
  loader.watch_and_load_for_file(outer_js_file, /*token=*/&outer_js_file);

  EXPECT_EQ(std::remove(inner_config_file.c_str()), 0)
      << "failed to delete " << inner_config_file << ": "
      << std::strerror(errno);
  write_file_or_exit(outer_config_file, u8R"({"globals": {"after": true}})"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();

  std::vector<std::string> watched_paths;
  std::vector<void*> watched_tokens;
  for (const Configuration_Change& change : changes) {
    watched_paths.emplace_back(*change.watched_path);
    watched_tokens.emplace_back(change.token);
  }
  EXPECT_THAT(watched_paths, ::testing::UnorderedElementsAreArray({
                                 ::testing::HasSubstr("outer.js"),
                                 ::testing::HasSubstr("inner.js"),
                             }));
  EXPECT_THAT(watched_tokens, ::testing::UnorderedElementsAreArray({
                                  &inner_js_file,
                                  &outer_js_file,
                              }));

  for (const Configuration_Change& change : changes) {
    EXPECT_SAME_FILE(*change.config_file->config_path, outer_config_file);
  }
}

TEST_F(Test_Configuration_Loader, load_config_file_directly) {
  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Configuration_Loader loader(Basic_Configuration_Filesystem::instance());
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/nullptr);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_TRUE(
      (*loaded_config)->config.globals().find(u8"testGlobalVariable"_sv));
}

TEST_F(Test_Configuration_Loader,
       rewriting_direct_config_file_completely_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(config_file, /*token=*/&config_file);

  write_file_or_exit(config_file, u8R"({"globals": {"after": true}})"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(*changes[0].watched_path, config_file);
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
  EXPECT_FALSE(changes[0].config_file->config.globals().find(u8"before"_sv));
  EXPECT_TRUE(changes[0].config_file->config.globals().find(u8"after"_sv));
}

TEST_F(Test_Configuration_Loader,
       creating_direct_config_file_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_FALSE(loaded_config.ok());

  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(*changes[0].watched_path, config_file);
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_SAME_FILE(*changes[0].config_file->config_path, config_file);
  EXPECT_TRUE(
      changes[0].config_file->config.globals().find(u8"testGlobalVariable"_sv));
}

TEST_F(Test_Configuration_Loader,
       deleting_direct_config_file_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(config_file, /*token=*/&config_file);

  EXPECT_EQ(std::remove(config_file.c_str()), 0)
      << "failed to delete " << config_file << ": " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(*changes[0].watched_path, config_file);
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
}

TEST_F(Test_Configuration_Loader,
       unwatching_js_file_then_modifying_config_file_is_not_a_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file = project_dir + "/hello.js";
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/nullptr);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  write_file_or_exit(config_file, u8R"({"globals": {"during": true}})"_sv);
  loader.unwatch_file(js_file);
  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());

  write_file_or_exit(config_file, u8R"({"globals": {"after": true}})"_sv);
  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       unwatching_config_file_then_modifying_is_not_a_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(config_file, /*token=*/nullptr);

  write_file_or_exit(config_file, u8R"({"globals": {"during": true}})"_sv);
  loader.unwatch_file(config_file);
  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());

  write_file_or_exit(config_file, u8R"({"globals": {"after": true}})"_sv);
  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       unwatching_all_then_modifying_files_is_not_a_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string js_file_1 = project_dir + "/hello1.js";
  std::string js_file_2 = project_dir + "/hello2.js";
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8R"({"globals": {"before": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config_1 =
      loader.watch_and_load_for_file(js_file_1, /*token=*/nullptr);
  ASSERT_TRUE(loaded_config_1.ok()) << loaded_config_1.error_to_string();
  auto loaded_config_2 =
      loader.watch_and_load_for_file(js_file_2, /*token=*/nullptr);
  ASSERT_TRUE(loaded_config_1.ok()) << loaded_config_2.error_to_string();

  write_file_or_exit(config_file, u8R"({"globals": {"during": true}})"_sv);
  loader.unwatch_all_files();
  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());

  write_file_or_exit(config_file, u8R"({"globals": {"after": true}})"_sv);
  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());
}

#if QLJS_HAVE_UNISTD_H
TEST_F(Test_Configuration_Loader,
       making_config_file_unreadable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string js_file = project_dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_TRUE(
      (*loaded_config)->config.globals().find(u8"testGlobalVariable"_sv));

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &js_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
  ASSERT_NE(changes[0].error, nullptr);
  EXPECT_EQ(changes[0].error->path, canonicalize_path(config_file)->c_str());
  EXPECT_EQ(changes[0].error->io_error.error, EACCES);
}

TEST_F(Test_Configuration_Loader,
       making_direct_config_file_unreadable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_TRUE(
      (*loaded_config)->config.globals().find(u8"testGlobalVariable"_sv));

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
  ASSERT_NE(changes[0].error, nullptr);
  EXPECT_EQ(changes[0].error->path, canonicalize_path(config_file)->c_str());
  EXPECT_EQ(changes[0].error->io_error.error, EACCES);
}

#if QLJS_HAVE_KQUEUE
// TODO(strager): Fix on macOS+kqueue.
TEST_F(Test_Configuration_Loader,
       DISABLED_making_unreadable_config_file_readable_is_detected_as_change)
#else
TEST_F(Test_Configuration_Loader,
       making_unreadable_config_file_readable_is_detected_as_change)
#endif
{
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string js_file = project_dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path,
            canonicalize_path(config_file)->c_str());
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  EXPECT_EQ(::chmod(config_file.c_str(), 0600), 0)
      << "failed to make " << config_file
      << " readable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &js_file);
  EXPECT_TRUE(
      changes[0].config_file->config.globals().find(u8"testGlobalVariable"_sv));
  EXPECT_EQ(changes[0].error, nullptr);
}

#if QLJS_HAVE_KQUEUE
// TODO(strager): Fix on macOS+kqueue.
TEST_F(
    Test_Configuration_Loader,
    DISABLED_making_unreadable_direct_config_file_readable_is_detected_as_change)
#else
TEST_F(Test_Configuration_Loader,
       making_unreadable_direct_config_file_readable_is_detected_as_change)
#endif
{
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path,
            canonicalize_path(config_file)->c_str());
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  EXPECT_EQ(::chmod(config_file.c_str(), 0600), 0)
      << "failed to make " << config_file
      << " readable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_TRUE(
      changes[0].config_file->config.globals().find(u8"testGlobalVariable"_sv));
  EXPECT_EQ(changes[0].error, nullptr);
}

TEST_F(Test_Configuration_Loader,
       unreadable_config_file_is_not_detected_as_changing) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string js_file = project_dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path,
            canonicalize_path(config_file)->c_str());
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       unreadable_direct_config_file_is_not_detected_as_changing) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_THAT(loaded_config.error().path,
              canonicalize_path(config_file)->c_str());
  EXPECT_THAT(loaded_config.error().io_error.error, EACCES);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       making_config_file_unreadable_then_readable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string js_file = project_dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/&js_file);

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  [[maybe_unused]] Span<Configuration_Change> changes_1 =
      loader.detect_changes_and_refresh();

  EXPECT_EQ(::chmod(config_file.c_str(), 0644), 0)
      << "failed to make " << config_file
      << " readable: " << std::strerror(errno);

  Span<Configuration_Change> changes_2 = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes_2, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes_2[0].token, &js_file);
  EXPECT_TRUE(changes_2[0].config_file->config.globals().find(
      u8"testGlobalVariable"_sv));
  EXPECT_EQ(changes_2[0].error, nullptr);
}

TEST_F(
    Test_Configuration_Loader,
    making_direct_config_file_unreadable_then_readable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(config_file, /*token=*/&config_file);

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  [[maybe_unused]] Span<Configuration_Change> changes_1 =
      loader.detect_changes_and_refresh();

  EXPECT_EQ(::chmod(config_file.c_str(), 0644), 0)
      << "failed to make " << config_file
      << " readable: " << std::strerror(errno);

  Span<Configuration_Change> changes_2 = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes_2, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes_2[0].token, &config_file);
  EXPECT_TRUE(changes_2[0].config_file->config.globals().find(
      u8"testGlobalVariable"_sv));
  EXPECT_EQ(changes_2[0].error, nullptr);
}

#if QLJS_HAVE_KQUEUE
// TODO(strager): Fix on macOS+kqueue.
TEST_F(
    Test_Configuration_Loader,
    DISABLED_making_unreadable_config_file_readable_then_unreadable_is_detected_as_change)
#else
TEST_F(
    Test_Configuration_Loader,
    making_unreadable_config_file_readable_then_unreadable_is_detected_as_change)
#endif
{
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string js_file = project_dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  std::string config_file_canonical_path(
      canonicalize_path(config_file)->path());

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_for_file(js_file, /*token=*/&js_file);

  EXPECT_EQ(::chmod(config_file.c_str(), 0644), 0)
      << "failed to make " << config_file
      << " readable: " << std::strerror(errno);

  [[maybe_unused]] Span<Configuration_Change> changes_1 =
      loader.detect_changes_and_refresh();

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Span<Configuration_Change> changes_2 = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes_2, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes_2[0].token, &js_file);
  EXPECT_EQ(changes_2[0].config_file, nullptr);
  ASSERT_NE(changes_2[0].error, nullptr);
  EXPECT_EQ(changes_2[0].error->path, config_file_canonical_path);
  EXPECT_EQ(changes_2[0].error->io_error.error, EACCES);
}

#if QLJS_HAVE_KQUEUE
// TODO(strager): Fix on macOS+kqueue.
TEST_F(
    Test_Configuration_Loader,
    DISABLED_making_unreadable_direct_config_file_readable_then_unreadable_is_detected_as_change)
#else
TEST_F(
    Test_Configuration_Loader,
    making_unreadable_direct_config_file_readable_then_unreadable_is_detected_as_change)
#endif
{
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  std::string config_file_canonical_path(
      canonicalize_path(config_file)->path());

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(config_file, /*token=*/&config_file);

  EXPECT_EQ(::chmod(config_file.c_str(), 0644), 0)
      << "failed to make " << config_file
      << " readable: " << std::strerror(errno);

  [[maybe_unused]] Span<Configuration_Change> changes_1 =
      loader.detect_changes_and_refresh();

  EXPECT_EQ(::chmod(config_file.c_str(), 0000), 0)
      << "failed to make " << config_file
      << " unreadable: " << std::strerror(errno);

  Span<Configuration_Change> changes_2 = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes_2, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes_2[0].token, &config_file);
  EXPECT_EQ(changes_2[0].config_file, nullptr);
  ASSERT_NE(changes_2[0].error, nullptr);
  EXPECT_EQ(changes_2[0].error->path, config_file_canonical_path);
  EXPECT_EQ(changes_2[0].error->io_error.error, EACCES);
}

TEST_F(Test_Configuration_Loader,
       making_unreadable_parent_dir_readable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string dir = project_dir + "/dir";
  create_directory_or_exit(dir);
  std::string js_file = dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string js_file_canonical_path(canonicalize_path(js_file)->path());
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  EXPECT_EQ(::chmod(dir.c_str(), 0600), 0)
      << "failed to make " << dir << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path, js_file);
  EXPECT_EQ(loaded_config.error().canonicalizing_path, js_file_canonical_path);
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  EXPECT_EQ(::chmod(dir.c_str(), 0700), 0)
      << "failed to make " << dir << " readable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &js_file);
  EXPECT_TRUE(
      changes[0].config_file->config.globals().find(u8"testGlobalVariable"_sv));
  EXPECT_EQ(changes[0].error, nullptr);
}

TEST_F(
    Test_Configuration_Loader,
    making_unreadable_parent_dir_of_direct_config_file_readable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string dir = project_dir + "/dir";
  create_directory_or_exit(dir);
  std::string config_file = dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  std::string config_file_canonical_path(
      canonicalize_path(config_file)->path());
  EXPECT_EQ(::chmod(dir.c_str(), 0600), 0)
      << "failed to make " << dir << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path, config_file);
  EXPECT_EQ(loaded_config.error().canonicalizing_path,
            config_file_canonical_path);
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  EXPECT_EQ(::chmod(dir.c_str(), 0700), 0)
      << "failed to make " << dir << " readable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_TRUE(
      changes[0].config_file->config.globals().find(u8"testGlobalVariable"_sv));
  EXPECT_EQ(changes[0].error, nullptr);
}

TEST_F(Test_Configuration_Loader,
       making_parent_dir_unreadable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string dir = project_dir + "/dir";
  create_directory_or_exit(dir);
  std::string js_file = dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string js_file_canonical_path(canonicalize_path(js_file)->path());
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_TRUE(
      (*loaded_config)->config.globals().find(u8"testGlobalVariable"_sv));

  EXPECT_EQ(::chmod(dir.c_str(), 0600), 0)
      << "failed to make " << dir << " unreadable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &js_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
  ASSERT_NE(changes[0].error, nullptr);
  EXPECT_EQ(changes[0].error->path, js_file);
  EXPECT_EQ(changes[0].error->canonicalizing_path, js_file_canonical_path);
  EXPECT_EQ(changes[0].error->io_error.error, EACCES);
}

TEST_F(
    Test_Configuration_Loader,
    making_parent_dir_of_direct_config_file_unreadable_is_detected_as_change) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string dir = project_dir + "/dir";
  create_directory_or_exit(dir);
  std::string config_file = dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  std::string config_file_canonical_path(
      canonicalize_path(config_file)->path());

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_TRUE(
      (*loaded_config)->config.globals().find(u8"testGlobalVariable"_sv));

  EXPECT_EQ(::chmod(dir.c_str(), 0600), 0)
      << "failed to make " << dir << " unreadable: " << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &config_file);
  EXPECT_EQ(changes[0].config_file, nullptr);
  ASSERT_NE(changes[0].error, nullptr);
  EXPECT_EQ(changes[0].error->path, config_file);
  EXPECT_EQ(changes[0].error->canonicalizing_path, config_file_canonical_path);
  EXPECT_EQ(changes[0].error->io_error.error, EACCES);
}

TEST_F(Test_Configuration_Loader,
       unreadable_parent_dir_is_not_detected_as_changing) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();

  std::string dir = project_dir + "/dir";
  create_directory_or_exit(dir);
  std::string js_file = dir + "/test.js";
  write_file_or_exit(js_file, u8""_sv);
  std::string js_file_canonical_path(canonicalize_path(js_file)->path());
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  EXPECT_EQ(::chmod(dir.c_str(), 0600), 0)
      << "failed to make " << dir << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_for_file(js_file, /*token=*/&js_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path, js_file);
  EXPECT_EQ(loaded_config.error().canonicalizing_path, js_file_canonical_path);
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       unreadable_parent_dir_of_direct_config_is_not_detected_as_changing) {
  if (process_ignores_filesystem_permissions()) {
    GTEST_SKIP() << "cannot run test as root";
  }

  std::string project_dir = this->make_temporary_directory();
  std::string dir = project_dir + "/dir";
  create_directory_or_exit(dir);
  std::string config_file = dir + "/quick-lint-js.config";
  write_file_or_exit(config_file,
                     u8R"({"globals": {"testGlobalVariable": true}})"_sv);
  std::string config_file_canonical_path(
      canonicalize_path(config_file)->path());
  EXPECT_EQ(::chmod(dir.c_str(), 0600), 0)
      << "failed to make " << dir << " unreadable: " << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/&config_file);
  EXPECT_FALSE(loaded_config.ok());
  EXPECT_EQ(loaded_config.error().path, config_file);
  EXPECT_EQ(loaded_config.error().canonicalizing_path,
            config_file_canonical_path);
  EXPECT_EQ(loaded_config.error().io_error.error, EACCES);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  EXPECT_THAT(changes, IsEmpty());
}
#endif

// TODO(strager): Test symlinks on Windows too.
#if QLJS_HAVE_UNISTD_H
TEST_F(Test_Configuration_Loader,
       changing_direct_config_path_symlink_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  std::string before_config_file = project_dir + "/before.config";
  write_file_or_exit(before_config_file,
                     u8R"({"globals": {"before": true}})"_sv);
  std::string after_config_file = project_dir + "/after.config";
  write_file_or_exit(after_config_file, u8R"({"globals": {"after": true}})"_sv);
  std::string config_symlink = project_dir + "/quick-lint-js.config";
  ASSERT_EQ(::symlink("before.config", config_symlink.c_str()), 0)
      << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(config_symlink, /*token=*/&config_symlink);

  ASSERT_EQ(std::remove(config_symlink.c_str()), 0) << std::strerror(errno);
  ASSERT_EQ(::symlink("after.config", config_symlink.c_str()), 0)
      << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(changes[0].token, &config_symlink);
  EXPECT_EQ(*changes[0].config_file->config_path,
            canonicalize_path(after_config_file)->canonical());
  EXPECT_FALSE(changes[0].config_file->config.globals().find(u8"before"_sv));
  EXPECT_TRUE(changes[0].config_file->config.globals().find(u8"after"_sv));
  EXPECT_EQ(changes[0].error, nullptr);

  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());
}

TEST_F(Test_Configuration_Loader,
       changing_parent_directory_symlink_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/before");
  create_directory_or_exit(project_dir + "/after");
  std::string before_config_file = project_dir + "/before/quick-lint-js.config";
  write_file_or_exit(before_config_file,
                     u8R"({"globals": {"before": true}})"_sv);
  std::string after_config_file = project_dir + "/after/quick-lint-js.config";
  write_file_or_exit(after_config_file, u8R"({"globals": {"after": true}})"_sv);
  std::string subdir_symlink = project_dir + "/subdir";
  ASSERT_EQ(::symlink("before", subdir_symlink.c_str()), 0)
      << std::strerror(errno);

  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(subdir_symlink + "/quick-lint-js.config",
                                    /*token=*/nullptr);

  ASSERT_EQ(std::remove(subdir_symlink.c_str()), 0) << std::strerror(errno);
  ASSERT_EQ(::symlink("after", subdir_symlink.c_str()), 0)
      << std::strerror(errno);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(*changes[0].config_file->config_path,
            canonicalize_path(after_config_file)->canonical());
  EXPECT_FALSE(changes[0].config_file->config.globals().find(u8"before"_sv));
  EXPECT_TRUE(changes[0].config_file->config.globals().find(u8"after"_sv));
  EXPECT_EQ(changes[0].error, nullptr);

  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());
}
#endif

TEST_F(Test_Configuration_Loader,
       swapping_parent_directory_with_another_is_detected_as_change) {
  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/before");
  create_directory_or_exit(project_dir + "/after");
  write_file_or_exit(project_dir + "/before/quick-lint-js.config",
                     u8R"({"globals": {"before": true}})"_sv);
  write_file_or_exit(project_dir + "/after/quick-lint-js.config",
                     u8R"({"globals": {"after": true}})"_sv);

  std::string subdir = project_dir + "/subdir";
  move_file(project_dir + "/before", subdir);
  Change_Detecting_Configuration_Loader loader;
  loader.watch_and_load_config_file(subdir + "/quick-lint-js.config",
                                    /*token=*/nullptr);

  move_file(subdir, project_dir + "/before");
  move_file(project_dir + "/after", subdir);

  Span<Configuration_Change> changes = loader.detect_changes_and_refresh();
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_EQ(*changes[0].config_file->config_path,
            canonicalize_path(subdir + "/quick-lint-js.config")->canonical());
  EXPECT_FALSE(changes[0].config_file->config.globals().find(u8"before"_sv));
  EXPECT_TRUE(changes[0].config_file->config.globals().find(u8"after"_sv));
  EXPECT_EQ(changes[0].error, nullptr);

  EXPECT_THAT(loader.detect_changes_and_refresh(), IsEmpty());
}

#if QLJS_HAVE_INOTIFY
TEST_F(Test_Configuration_Loader,
       inotify_init_failure_is_reported_out_of_band) {
  Mock_Inotify_Init_Error_Guard guard(EMFILE);

  std::string project_dir = this->make_temporary_directory();
  std::string config_file = project_dir + "/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/nullptr);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  std::vector<Watch_IO_Error> errors = loader.fs_take_watch_errors();
  ASSERT_THAT(errors, ElementsAreArray({::testing::_}));
  const Watch_IO_Error& error = errors[0];
  EXPECT_EQ(error.io_error.error, EMFILE) << error.to_string();
  EXPECT_EQ(error.path, "") << "init error should have an empty path\n"
                            << error.to_string();
}

TEST_F(Test_Configuration_Loader,
       inotify_watch_failure_is_reported_out_of_band) {
  Mock_Inotify_Add_Watch_Error_Guard guard(ENOSPC);

  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/subdir");
  std::string config_file = project_dir + "/subdir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/nullptr);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  std::vector<Watch_IO_Error> errors = loader.fs_take_watch_errors();
  std::vector<std::string> error_paths;
  for (Watch_IO_Error& error : errors) {
    EXPECT_EQ(error.io_error.error, ENOSPC) << error.to_string();
    error_paths.push_back(error.path);
  }
  EXPECT_THAT(error_paths,
              ::testing::IsSupersetOf({
                  canonicalize_path(project_dir)->canonical(),
                  canonicalize_path(project_dir + "/subdir")->canonical(),
              }));
}
#endif

#if QLJS_HAVE_KQUEUE
TEST_F(Test_Configuration_Loader,
       kqueue_directory_open_failure_is_reported_out_of_band) {
  Mock_Kqueue_Directory_Open_Error_Guard guard(EMFILE);

  std::string project_dir = this->make_temporary_directory();
  create_directory_or_exit(project_dir + "/subdir");
  std::string config_file = project_dir + "/subdir/quick-lint-js.config";
  write_file_or_exit(config_file, u8"{}"_sv);

  Change_Detecting_Configuration_Loader loader;
  auto loaded_config =
      loader.watch_and_load_config_file(config_file, /*token=*/nullptr);
  EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  std::vector<Watch_IO_Error> errors = loader.fs_take_watch_errors();
  std::vector<std::string> error_paths;
  for (Watch_IO_Error& error : errors) {
    EXPECT_EQ(error.io_error.error, EMFILE) << error.to_string();
    error_paths.push_back(error.path);
  }
  EXPECT_THAT(error_paths,
              ::testing::IsSupersetOf({
                  canonicalize_path(project_dir)->canonical(),
                  canonicalize_path(project_dir + "/subdir")->canonical(),
              }));
}
#endif

#if defined(_WIN32)
TEST_F(Test_Configuration_Loader,
       win32_directory_oplock_ioctl_failure_is_reported_out_of_band) {
  for (auto [mocked_function_description, error_to_mock, mock_error] : {
           std::make_tuple("open", &mock_win32_force_directory_file_id_error,
                           ERROR_FILE_NOT_FOUND),
           // For directories on SMB-mounted drives,
           // GetFileInformationByHandleEx fails with ERROR_INVALID_PARAMETER.
           std::make_tuple("file id", &mock_win32_force_directory_file_id_error,
                           ERROR_INVALID_PARAMETER),
           // For directories on SMB-mounted drives, DeviceIoControl with
           // FSCTL_REQUEST_OPLOCK fails with ERROR_INVALID_FUNCTION.
           std::make_tuple("ioctl", &mock_win32_force_directory_ioctl_error,
                           ERROR_INVALID_FUNCTION),
       }) {
    SCOPED_TRACE(mocked_function_description);
    Mock_Win32_Watch_Error_Guard guard(error_to_mock, mock_error);

    std::string project_dir = this->make_temporary_directory();
    std::string config_file = project_dir + "/quick-lint-js.config";
    write_file_or_exit(config_file, u8"{}"_sv);

    Change_Detecting_Configuration_Loader loader;
    auto loaded_config =
        loader.watch_and_load_config_file(config_file, /*token=*/nullptr);
    EXPECT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

    std::vector<Watch_IO_Error> errors = loader.fs_take_watch_errors();
    std::vector<std::string> error_paths;
    for (Watch_IO_Error& error : errors) {
      EXPECT_EQ(error.io_error.error, mock_error) << error.to_string();
      error_paths.push_back(error.path);
    }
    EXPECT_THAT(error_paths, ::testing::Contains(
                                 canonicalize_path(project_dir)->canonical()));
  }
}
#endif

class Test_Configuration_Loader_Fake : public ::testing::Test {
 public:
  Monotonic_Allocator allocator{"Test_Configuration_Loader_Fake"};
};

TEST_F(Test_Configuration_Loader_Fake,
       file_with_no_config_file_gets_default_config) {
  Fake_Configuration_Filesystem fs;
  fs.create_file(fs.rooted("hello.js"), u8""_sv);

  Configuration_Loader loader(&fs);
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = fs.rooted("hello.js").c_str(),
      .config_file = nullptr,
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  EXPECT_EQ(*loaded_config, nullptr);
}

TEST_F(Test_Configuration_Loader_Fake,
       find_quick_lint_js_config_in_same_directory) {
  Fake_Configuration_Filesystem fs;
  fs.create_file(fs.rooted("hello.js"), u8""_sv);
  fs.create_file(fs.rooted("quick-lint-js.config"), u8"{}"_sv);

  Configuration_Loader loader(&fs);
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = fs.rooted("hello.js").c_str(),
      .config_file = nullptr,
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_EQ(*(*loaded_config)->config_path, fs.rooted("quick-lint-js.config"));
}

TEST_F(Test_Configuration_Loader_Fake, find_config_in_parent_directory) {
  Fake_Configuration_Filesystem fs;
  fs.create_file(fs.rooted("dir/hello.js"), u8""_sv);
  fs.create_file(fs.rooted("quick-lint-js.config"), u8"{}"_sv);

  Configuration_Loader loader(&fs);
  auto loaded_config = loader.load_for_file(File_To_Lint{
      .path = fs.rooted("dir/hello.js").c_str(),
      .config_file = nullptr,
  });
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();

  EXPECT_EQ(*(*loaded_config)->config_path, fs.rooted("quick-lint-js.config"));
}

TEST_F(Test_Configuration_Loader_Fake,
       adding_json_syntax_error_makes_config_default) {
  Fake_Configuration_Filesystem fs;
  fs.create_file(fs.rooted("hello.js"), u8""_sv);
  fs.create_file(fs.rooted("quick-lint-js.config"), u8"{}"_sv);

  Configuration_Loader loader(&fs);
  auto loaded_config =
      loader.watch_and_load_for_file(fs.rooted("hello.js").path(), nullptr);
  ASSERT_TRUE(loaded_config.ok()) << loaded_config.error_to_string();
  ASSERT_TRUE(*loaded_config);
  ASSERT_TRUE((*loaded_config)->config.globals().find(u8"console"_sv));

  fs.create_file(fs.rooted("quick-lint-js.config"), u8"{\\}"_sv);
  Span<Configuration_Change> changes = loader.refresh(&this->allocator);
  ASSERT_THAT(changes, ElementsAreArray({::testing::_}));
  EXPECT_TRUE(changes[0].config_file->config.globals().find(u8"console"_sv));
}

TEST_F(Test_Configuration_Loader_Fake,
       multiple_watches_for_same_token_are_notified_together) {
  Fake_Configuration_Filesystem fs;
  fs.create_file(fs.rooted("quick-lint-js.config"), u8"{}"_sv);
  char token_1;
  char token_2;

  Configuration_Loader loader(&fs);
  loader.watch_and_load_config_file(fs.rooted("quick-lint-js.config").path(),
                                    &token_1);
  loader.watch_and_load_config_file(fs.rooted("quick-lint-js.config").path(),
                                    &token_2);

  fs.create_file(fs.rooted("quick-lint-js.config"),
                 u8"{\"_svglobal-groups\": false}"_sv);
  Span<Configuration_Change> changes = loader.refresh(&this->allocator);
  std::vector<void*> tokens;
  for (Configuration_Change& change : changes) {
    tokens.push_back(change.token);
  }
  ASSERT_THAT(tokens,
              ::testing::UnorderedElementsAreArray({&token_1, &token_2}));
}

Span<Configuration_Change>
Change_Detecting_Configuration_Loader::detect_changes_and_refresh() {
#if defined(_WIN32)
  std::unique_lock<Mutex> lock(this->mutex_);
  bool fs_changed = this->detect_changes_locked(lock);
#else
  bool fs_changed = this->detect_changes();
#endif
  Span<Configuration_Change> config_changes =
      this->loader_.refresh(&this->allocator);
  if (fs_changed) {
    // NOTE(strager): We cannot assert that at least one change happened,
    // because filesystem notifications might be spurious.
  } else {
    EXPECT_THAT(config_changes, IsEmpty())
        << "no filesystem notifications happened, but changes were detected";
  }
  return config_changes;
}

bool Change_Detecting_Configuration_Loader::detect_changes() {
#if QLJS_HAVE_INOTIFY
  std::vector<::pollfd> pollfds{::pollfd{
      .fd = this->fs_.get_inotify_fd().value().get(),
      .events = POLLIN,
      .revents = 0,
  }};
  int poll_rc = ::poll(pollfds.data(), pollfds.size(), 0);
  if (poll_rc == -1) {
    ADD_FAILURE() << "poll failed: " << std::strerror(errno);
    return {};
  }
  this->fs_.handle_poll_event(pollfds[0].revents);
  return poll_rc != 0;
#elif QLJS_HAVE_KQUEUE
  Fixed_Vector<struct ::kevent, 20> events;
  events.resize(events.capacity());
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 0};
  int kqueue_rc = ::kevent(
      /*fd=*/this->kqueue_fd_.get(),
      /*changelist=*/nullptr,
      /*nchanges=*/0,
      /*eventlist=*/events.data(),
      /*nevents=*/narrow_cast<int>(events.size()),
      /*timeout=*/&timeout);
  if (kqueue_rc == -1) {
    ADD_FAILURE() << "kqueue failed: " << std::strerror(errno);
    return {};
  }
  events.resize(narrow_cast<Fixed_Vector_Size>(kqueue_rc));
  for (struct ::kevent& event : events) {
    EXPECT_FALSE(event.flags & EV_ERROR)
        << std::strerror(narrow_cast<int>(event.data));
    EXPECT_EQ(event.udata, reinterpret_cast<void*>(event_udata_fs_changed));
  }
  return kqueue_rc != 0;
#elif defined(_WIN32)
  std::unique_lock<Mutex> lock(this->mutex_);
  this->detect_changes_locked(lock);
#else
#error "Unsupported platform"
#endif
}

#if defined(_WIN32)
bool Change_Detecting_Configuration_Loader::detect_changes_locked(
    std::unique_lock<Mutex>& lock) {
  auto old_io_thread_timed_out_count = this->io_thread_timed_out_count_;
  this->io_thread_timed_out_.wait(lock, [&]() {
    return this->io_thread_timed_out_count_ != old_io_thread_timed_out_count;
  });

  bool fs_changed = this->old_fs_changed_count_ != this->fs_changed_count_;
  this->old_fs_changed_count_ = this->fs_changed_count_;
  return fs_changed;
}
#endif

#if defined(_WIN32)
void Change_Detecting_Configuration_Loader::run_io_thread() {
  for (;;) {
    DWORD number_of_bytes_transferred;
    ULONG_PTR completion_key = completion_key_invalid;
    OVERLAPPED* overlapped;

    std::lock_guard<Mutex> lock(this->mutex_);
    BOOL ok = ::GetQueuedCompletionStatus(
        /*CompletionPort=*/this->io_completion_port_.get(),
        /*lpNumberOfBytesTransferred=*/&number_of_bytes_transferred,
        /*lpCompletionKey=*/&completion_key, /*lpOverlapped=*/&overlapped,
        /*dwMilliseconds=*/0);
    DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();

    if (!overlapped) {
      switch (error) {
      case WAIT_TIMEOUT:
        this->io_thread_timed_out_count_ += 1;
        this->io_thread_timed_out_.notify_all();
        continue;

      default:
        QLJS_UNIMPLEMENTED();
        break;
      }
    }

    switch (completion_key) {
    case completion_key_invalid:
      QLJS_UNREACHABLE();
      break;

    case completion_key_stop:
      return;

    case completion_key_fs_changed: {
      bool fs_changed = this->fs_.handle_event(
          overlapped, number_of_bytes_transferred, error);
      if (fs_changed) {
        this->fs_changed_count_ += 1;
        this->fs_changed_.notify_all();
      }
      break;
    }

    default:
      QLJS_UNREACHABLE();
      break;
    }
  }
}

void Change_Detecting_Configuration_Loader::stop_io_thread() {
  BOOL ok = ::PostQueuedCompletionStatus(
      /*CompletionPort=*/this->io_completion_port_.get(),
      /*dwNumberOfBytesTransferred=*/0,
      /*dwCompletionKey=*/completion_key_stop,
      /*lpOverlapped=*/reinterpret_cast<OVERLAPPED*>(1));
  if (!ok) {
    QLJS_UNIMPLEMENTED();
  }
}
#endif

void move_file(const std::string& from, const std::string& to) {
  if (std::rename(from.c_str(), to.c_str()) != 0) {
    int error = errno;
#if defined(_WIN32)
    if (error == EEXIST) {
      BOOL ok = ::ReplaceFileW(
          /*lpReplacedFileName=*/std::filesystem::path(to).wstring().c_str(),
          /*lpReplacementFileName=*/
          std::filesystem::path(from).wstring().c_str(),
          /*lpBackupFileName=*/nullptr,
          /*dwReplacemeFlags=*/0,
          /*lpExclude=*/nullptr,
          /*lpReserved=*/nullptr);
      if (!ok) {
        ADD_FAILURE() << "failed to move " << from << " to " << to << ": "
                      << windows_last_error_message();
      }
      return;
    }
#endif
    ADD_FAILURE() << "failed to move " << from << " to " << to << ": "
                  << std::strerror(error);
  }
}
}
}

#endif

// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew "strager" Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.

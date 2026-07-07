// common/tmp_path.hpp — collision-free temp paths for parallel ctest (-j).
//
// A fixed name like <TempDir>/scenario.yaml is shared across every test in a
// binary AND across the binaries "ctest -j" runs concurrently, so two runners
// clobber each other's file and read back the wrong contents (phantom -j-only
// failures). These helpers key each path on pid + current gtest test name +
// a monotonic counter so no two callers ever collide.
#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ni::cmodel::testing {

namespace detail {
inline std::string unique_token(std::string_view stem) {
    static std::atomic<unsigned> counter{0};
#ifdef _WIN32
    long pid = ::_getpid();
#else
    long pid = ::getpid();
#endif
    std::string test = "no_test";
    if (auto const* info = ::testing::UnitTest::GetInstance()->current_test_info()) {
        test = info->name();
        std::replace(test.begin(), test.end(), '/', '_');  // gtest param names use '/'
    }
    return std::string(stem) + "_" + std::to_string(pid) + "_" + test + "_" +
           std::to_string(counter.fetch_add(1));
}
}  // namespace detail

// Unique file path under the system temp dir (the file itself is not created).
inline std::string unique_temp_path(std::string_view stem) {
    return (std::filesystem::temp_directory_path() / detail::unique_token(stem)).string();
}

// Unique, freshly-created directory under the system temp dir. Use when a
// scenario writes sidecar data_file/dump_file next to its YAML: bare names in
// the YAML resolve inside this dir instead of a shared <TempDir>.
inline std::string unique_temp_dir(std::string_view stem) {
    auto d = std::filesystem::temp_directory_path() / detail::unique_token(stem);
    std::filesystem::create_directories(d);
    return d.string();
}

}  // namespace ni::cmodel::testing

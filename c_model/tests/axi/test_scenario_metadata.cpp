// Tests for metadata block parsing (schema_version: 1 strict mode).
#include "axi/scenario_parser.hpp"
#include "scenario_helpers.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {

std::string write_tmp_yaml(std::string const& body) {
    auto p =
        std::filesystem::temp_directory_path() /
        ("scn_meta_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".yaml");
    std::ofstream(p) << body;
    return p.string();
}

TEST(ScenarioMetadata, parses_name_and_category) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
metadata:
  name: AX4-BAS-001_dummy
  category: basic
config:
  memory_base: 0x1000
  memory_size: 0x1000
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    auto sc = ni::cmodel::axi::load_scenario(path);
    EXPECT_EQ(sc.metadata.name, "AX4-BAS-001_dummy");
    EXPECT_EQ(sc.metadata.category, "basic");
}

TEST(ScenarioMetadata, strict_mode_rejects_missing_metadata) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    EXPECT_THROW(ni::cmodel::axi::load_scenario(path), std::runtime_error);
}

TEST(ScenarioMetadata, strict_mode_rejects_bad_name_regex) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
metadata: { name: not_axi_pattern, category: basic }
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    EXPECT_THROW(ni::cmodel::axi::load_scenario(path), std::runtime_error);
}

TEST(ScenarioMetadata, strict_mode_rejects_cat_category_mismatch) {
    auto path = write_tmp_yaml(R"(
schema_version: 1
metadata: { name: AX4-BAS-001_dummy, category: burst }
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    EXPECT_THROW(ni::cmodel::axi::load_scenario(path), std::runtime_error);
}

TEST(ScenarioMetadata, lenient_mode_unchanged) {
    // No schema_version, no metadata block — should load fine (legacy path).
    auto path = write_tmp_yaml(R"(
config: { memory_base: 0x1000, memory_size: 0x1000 }
transactions:
  - { op: write, addr: 0x1000, id: 0, len: 0, size: 5, burst: INCR, data_file: nofile }
)");
    auto sc = ni::cmodel::axi::load_scenario(path);
    EXPECT_TRUE(sc.metadata.name.empty());
}

TEST(ScenarioHelpers, helper_compiles) {
    // After commit 2 migration, kAllAxi4Scenarios has 31 AX4-* scenarios.
    using router::tests::kAllAxi4Scenarios;
    EXPECT_GT(kAllAxi4Scenarios.size(), 0u);
}

}  // namespace

// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#include "axi/scenario_parser.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

namespace axi = ni::cmodel::axi;

namespace {

// Write YAML content to a temp file; return the file path as a string.
std::string write_temp_yaml(const std::string& content) {
    auto path = std::filesystem::temp_directory_path() / "test_axi_master_inject_tmp.yaml";
    std::ofstream f(path);
    f << content;
    return path.string();
}

}  // namespace

TEST(AxiMasterInject, unknown_mode_throws_at_parse) {
    SCENARIO("YAML config.inject with unknown mode is rejected by parser allowlist");
    const std::string yaml =
        "config:\n"
        "  inject:\n"
        "    mode: bogus_mode\n"
        "    cycle: 5\n"
        "transactions:\n"
        "  - op: read\n"
        "    addr: 0x0\n"
        "    id: 0\n"
        "    len: 0\n"
        "    size: 0\n"
        "    burst: INCR\n"
        "    dump_file: /dev/null\n";
    auto path = write_temp_yaml(yaml);
    EXPECT_THROW(axi::load_scenario(path), std::runtime_error);
}

TEST(AxiMasterInject, no_inject_field_means_mode_none) {
    SCENARIO("scenario without config.inject field defaults to InjectConfig::Mode::None");
    const std::string yaml =
        "transactions:\n"
        "  - op: read\n"
        "    addr: 0x0\n"
        "    id: 0\n"
        "    len: 0\n"
        "    size: 0\n"
        "    burst: INCR\n"
        "    dump_file: /dev/null\n";
    auto path = write_temp_yaml(yaml);
    auto sc = axi::load_scenario(path);
    EXPECT_EQ(sc.config.inject.mode, axi::InjectConfig::Mode::None);
    EXPECT_EQ(sc.config.inject.cycle, 0u);
}

TEST(AxiMasterInject, aw_unstable_at_cycle_n_parsed_correctly) {
    SCENARIO("aw_unstable mode and cycle field parsed correctly into ScenarioConfig::inject");
    const std::string yaml =
        "config:\n"
        "  inject:\n"
        "    mode: aw_unstable\n"
        "    cycle: 17\n"
        "transactions:\n"
        "  - op: read\n"
        "    addr: 0x0\n"
        "    id: 0\n"
        "    len: 0\n"
        "    size: 0\n"
        "    burst: INCR\n"
        "    dump_file: /dev/null\n";
    auto path = write_temp_yaml(yaml);
    auto sc = axi::load_scenario(path);
    EXPECT_EQ(sc.config.inject.mode, axi::InjectConfig::Mode::AwUnstable);
    EXPECT_EQ(sc.config.inject.cycle, 17u);
}

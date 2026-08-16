#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using ni::cmodel::nmu::addr_trans::load_sam_table;
using ni::cmodel::nmu::addr_trans::SamEntry;
namespace axi = ni::cmodel::axi;

namespace {

const char* space_name(axi::Space space) {
    switch (space) {
        case axi::Space::Config:
            return "config";
        case axi::Space::Memory:
            return "memory";
        case axi::Space::Peripheral:
            return "peripheral";
    }
    return "?";
}

// One SAM rule as sim/configs/sam_rules.golden spells it. Twin of
// sim/tools/test_sam_config_parity.py's rule_line().
std::string rule_line(const std::string& name, const SamEntry& e) {
    std::ostringstream os;
    os << name << ' ' << space_name(e.space) << " 0x" << std::hex << e.base << " 0x" << e.size
       << " 0x" << unsigned{e.dst_id} << std::dec << ' ' << unsigned{e.port};
    return os.str();
}

std::vector<std::string> golden_lines() {
    std::ifstream in(std::string(CONFIG_DIR) + "/sam_rules.golden");
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        out.push_back(line);
    }
    return out;
}

}  // namespace

// The parity gate this stage exists for. sim/tools/address_map.py expands a
// config at generate time and this reader expands the same config at simulation
// runtime through +sam_config. If the two expansions diverge, the generated
// package and the generated stimulus agree with each other and disagree with
// the runtime address translator: every artifact looks self-consistent while
// traffic decodes to the wrong node, and a passing simulation never sees it.
//
// The C++ reader is a header with no callable interface from Python, so the two
// meet at sim/configs/sam_rules.golden -- this test holds the C++ expansion to
// it, test_sam_config_parity.py holds the Python one to it, and equality is
// transitive. Rules are compared in order, one at a time, not as sets: an
// X-fast and a Y-fast expansion of a square mesh produce the identical set of
// bases and transpose every coordinate.
//
// CONFIG_DIR is globbed rather than listed, so a new config cannot fall out of
// coverage; a config with no golden lines fails on the count below.
TEST(SamConfig, RulesMatchThePythonExpansion) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(CONFIG_DIR)) {
        if (entry.path().extension() == ".yml") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "expected the FlooNoC-shaped configs in " CONFIG_DIR;

    std::vector<std::string> rules;
    for (const auto& file : files) {
        SCOPED_TRACE(file.string());
        // Named, not a temporary in the range-init: entries() hands back a
        // reference into the table, and a temporary table dies before the loop
        // body runs.
        const auto table = load_sam_table(file.string());
        for (const auto& e : table.entries()) {
            rules.push_back(rule_line(file.stem().string(), e));
        }
    }

    const std::vector<std::string> golden = golden_lines();
    ASSERT_FALSE(golden.empty()) << "sam_rules.golden is missing or holds no rules";
    for (std::size_t i = 0; i < std::min(rules.size(), golden.size()); ++i) {
        EXPECT_EQ(rules[i], golden[i]) << "rule " << i;
    }
    EXPECT_EQ(rules.size(), golden.size());
}

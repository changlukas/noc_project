#include "axi/types.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

TEST(AxiScaffold, ConstantsFromCodegen) {
    SCENARIO("axi scaffold: DATA_BYTES/DATA_WIDTH align with codegen NOC width constants");
    EXPECT_EQ(axi::DATA_BYTES, ni::WSTRB_WIDTH);
    EXPECT_EQ(axi::DATA_WIDTH, ni::width::NOC_DATA_WIDTH);
    EXPECT_EQ(axi::DATA_BYTES * 8, axi::DATA_WIDTH);
}

TEST(AxiScaffold, BurstEnumValues) {
    SCENARIO("axi scaffold: Burst enum encodes FIXED=0, INCR=1, WRAP=2 (AXI4 IHI 0022 §A3.4.1)");
    EXPECT_EQ(static_cast<int>(axi::Burst::FIXED), 0);
    EXPECT_EQ(static_cast<int>(axi::Burst::INCR), 1);
    EXPECT_EQ(static_cast<int>(axi::Burst::WRAP), 2);
}

TEST(AxiScaffold, RespEnumValues) {
    SCENARIO("axi scaffold: Resp enum encodes OKAY=0, EXOKAY=1, SLVERR=2, DECERR=3");
    EXPECT_EQ(static_cast<int>(axi::Resp::OKAY), 0);
    EXPECT_EQ(static_cast<int>(axi::Resp::EXOKAY), 1);
    EXPECT_EQ(static_cast<int>(axi::Resp::SLVERR), 2);
    EXPECT_EQ(static_cast<int>(axi::Resp::DECERR), 3);
}

TEST(AxiScaffold, WBeatDataArrayMatchesDataBytes) {
    SCENARIO("axi scaffold: WBeat.data std::array size equals DATA_BYTES");
    axi::WBeat w{};
    EXPECT_EQ(w.data.size(), static_cast<std::size_t>(axi::DATA_BYTES));
}

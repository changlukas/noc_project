#include "ni/ni_stage.hpp"
#include <gtest/gtest.h>

using ni::cmodel::NiPath;

TEST(NiStage, EnumValuesAreDistinct) {
    EXPECT_NE(NiPath::NmuReq, NiPath::NmuRsp);
    EXPECT_NE(NiPath::NsuReq, NiPath::NsuRsp);
    EXPECT_NE(NiPath::NmuReq, NiPath::NsuReq);
    EXPECT_NE(NiPath::NmuRsp, NiPath::NsuRsp);
}

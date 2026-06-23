#include "router/pipeline_stage.hpp"
#include <gtest/gtest.h>
using ni::cmodel::router::PipelineStage;
namespace {
TEST(PipelineStage, EmptyByDefault) {
    PipelineStage<int> s;
    EXPECT_FALSE(s.full());
    EXPECT_EQ(s.occupancy(), 0u);
    EXPECT_TRUE(s.ready());
}
TEST(PipelineStage, AcceptThenTake) {
    PipelineStage<int> s;
    s.accept(7);
    EXPECT_TRUE(s.full());
    EXPECT_EQ(s.occupancy(), 1u);
    EXPECT_FALSE(s.ready());
    EXPECT_EQ(s.peek(), 7);
    EXPECT_EQ(s.take(), 7);
    EXPECT_TRUE(s.ready());
}
TEST(PipelineStage, OverwriteAsserts) {  // mirrors router.hpp:185
    PipelineStage<int> s;
    s.accept(1);
    EXPECT_DEATH(s.accept(2), "PipelineStage: overwrite");
}
}  // namespace

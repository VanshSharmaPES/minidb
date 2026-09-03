#include <gtest/gtest.h>
#include "db.h"
TEST(Smoke, BuildWorks) { EXPECT_EQ(minidb::version(), 1); }

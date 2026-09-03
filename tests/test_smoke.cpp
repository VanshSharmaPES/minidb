#include <gtest/gtest.h>
#include "db.h"

TEST(Smoke, HeaderCompiles) {
    minidb::Options opts;
    EXPECT_TRUE(opts.sync_every_write);
    EXPECT_EQ(opts.sync_interval, 100u);
}

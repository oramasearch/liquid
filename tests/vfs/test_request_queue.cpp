// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <iostream>

#include "vfs/request_queue.hpp"

TEST(VfsRequestQueue, measures_runtime_handoff_latency) {
    const auto latency = liquid::vfs_detail::measure_request_queue(100);
    EXPECT_GT(latency.count(), 0);
    std::cout << "request_queue_average_latency_ns=" << latency.count() << '\n';
}

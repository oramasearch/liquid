// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <iostream>
#include <string_view>

#include "../fuse_probe.hpp"
#include "conformance_backend.hpp"

int main(int argc, char** argv) {
    using namespace liquid::test::vfs_conformance;
    auto remaining = 1;
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument.starts_with("--backend=")) {
            const auto name = argument.substr(std::string_view{"--backend="}.size());
            selected_backend = nullptr;
            for (const auto& driver : backends()) {
                if (driver.name == name) {
                    selected_backend = &driver;
                }
            }
            if (!selected_backend) {
                std::cerr << "mount: expected a registered backend, got '" << name << "'\n";
                return 2;
            }
        } else if (argument == "--allow-unavailable") {
            allow_unavailable = true;
        } else {
            argv[remaining++] = argv[index];
        }
    }
    if (!selected_backend) {
        std::cerr << "mount: expected explicit --backend=<name>; registered:";
        for (const auto& driver : backends()) {
            std::cerr << ' ' << driver.name;
        }
        std::cerr << '\n';
        return 2;
    }
    if (selected_backend->name == "libfuse") {
        if (const auto reason = liquid::test::fuse_unavailable_reason()) {
            // A required backend that cannot mount on this host is skipped, not failed: the
            // conformance baseline is not established here, and the reason says why (#362).
            std::cout << "VFS conformance backend=libfuse skipped: " << *reason << '\n';
            return 77;
        }
    }
    argc = remaining;
    argv[argc] = nullptr;
    testing::InitGoogleTest(&argc, argv);
    std::cout << "VFS conformance backend=" << selected_backend->name
              << " backend_required=" << (allow_unavailable ? "false" : "true") << '\n';
    const auto result = RUN_ALL_TESTS();
    if (result != 0) {
        return result;
    }
    const auto* tests = testing::UnitTest::GetInstance();
    if (tests->skipped_test_count() != 0 || tests->successful_test_count() == 0) {
        std::cout << "VFS conformance baseline NOT established: incomplete operation coverage\n";
        return 77;
    }
    return 0;
}

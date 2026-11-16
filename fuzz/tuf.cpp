// SPDX-License-Identifier: MIT

// TUF metadata as liquid's TUF client reads it from a release repository (liquid #463): the root it
// pins, and the timestamp, snapshot, targets and rotated roots of an update (src/tuf.cpp). The
// client keeps its state in a repository's metadata, so the harness makes one repository in a
// temporary directory, once, and re-pins a root before each update.
//
//   0  a root: pin
//   1  length-prefixed parts: a root to pin, then timestamp, snapshot and targets for an update,
//      then the target contents to check against every target the update accepted, then any
//      rotated roots. Verified at a fixed time.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "fuzz.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/tuf.hpp"

namespace fs = std::filesystem;
namespace tuf = liquid::tuf;

namespace {

constexpr std::size_t part_limit = 8;

// 2026-01-01T00:00:00Z, inside the fixture metadata's validity.
constexpr auto verification_time = std::chrono::sys_seconds{std::chrono::seconds{1'767'225'600}};

auto copy(std::span<const std::byte> input) -> std::vector<std::byte> {
    return {input.begin(), input.end()};
}

auto subject() -> liquid::repository* {
    static auto repository = [] {
        const auto root =
            fs::temp_directory_path() / ("liquid-fuzz-tuf-" + std::to_string(::getpid()));
        std::error_code ignored;
        fs::remove_all(root, ignored);
        ::setenv("LIQUID_HOME", (root / "home").c_str(), 1);
        auto created = liquid::runtime::block_on(liquid::repository::create(root / "repository"));
        return created ? std::optional<liquid::repository>{std::move(*created)} : std::nullopt;
    }();
    return repository ? &*repository : nullptr;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 2);
    auto* repository = subject();
    if (!input || repository == nullptr) {
        return 0;
    }
    if (input->decoder == 0) {
        (void)liquid::runtime::block_on(
            tuf::client::pin(*repository, copy(input->payload), verification_time));
        return 0;
    }
    const auto split = liquid::fuzz::parts(input->payload, part_limit);
    if (split.size() < 4) {
        return 0;
    }
    const auto pinned =
        liquid::runtime::block_on(tuf::client::pin(*repository, copy(split[0]), verification_time));
    if (!pinned) {
        return 0;
    }
    auto client = liquid::runtime::block_on(tuf::client::open(*repository, verification_time));
    if (!client) {
        return 0;
    }
    auto metadata = tuf::update_metadata{
        .roots = {},
        .timestamp = copy(split[1]),
        .snapshot = copy(split[2]),
        .targets = copy(split[3]),
    };
    for (std::size_t index = 5; index < split.size(); ++index) {
        metadata.roots.push_back(copy(split[index]));
    }
    const auto updated = liquid::runtime::block_on(client->update(std::move(metadata)));
    if (!updated) {
        return 0;
    }
    const auto contents = split.size() > 4 ? split[4] : std::span<const std::byte>{};
    for (const auto& target : updated->targets) {
        (void)client->target(target.path);
        (void)client->verify_target(target.path, contents);
    }
    return 0;
}

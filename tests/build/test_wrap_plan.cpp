// SPDX-License-Identifier: MIT

// liquid#401: the wrap hook revalidates conditional inputs with BLAKE3 (never FNV-1a) and reads a
// plan only from liquid's own location, bound to the digest liquid wrote it with.
// liquid#449: the process key that names each wrapped process is BLAKE3-256 too, and a saved wrap
// state with the old 64-bit keys is ignored.

#include <gtest/gtest.h>

#ifndef _WIN32

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "build/wrap.hpp"
#include "liquid/hash.hpp"
#include "sandbox/wrap_plan.h"

extern char** environ;

namespace {

// Two 16-byte inputs with the same 64-bit FNV-1a value, 3f59d21c3718158e, found by a Pollard rho
// search. Under the old row fingerprint (size plus FNV-1a) the second matched a row recorded for
// the first.
constexpr auto recorded_input = std::string_view{"63f9f7662a36d07e"};
constexpr auto colliding_input = std::string_view{"d5b15a15b2e650b1"};

auto fnv1a(std::string_view value) -> std::uint64_t {
    auto output = std::uint64_t{1469598103934665603ULL};
    for (const auto character : value) {
        output ^= static_cast<unsigned char>(character);
        output *= 1099511628211ULL;
    }
    return output;
}

auto hex(std::string_view value) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

// The key the hook computes for a command (liquid#449).
auto process_key(std::string_view executable,
                 std::string_view directory,
                 std::string_view arguments) -> std::string {
    auto output = std::array<char, LIQUID_WRAP_KEY_HEX_SIZE + 1>{};
    liquid_wrap_process_key_hex(executable.data(),
                                executable.size(),
                                directory.data(),
                                directory.size(),
                                arguments.data(),
                                arguments.size(),
                                output.data());
    return output.data();
}

// The 64-bit key the hook computed before liquid#449: FNV-1a over executable, NUL, working
// directory, NUL, and the NUL-separated arguments.
auto old_process_key(std::string_view executable,
                     std::string_view directory,
                     std::string_view arguments) -> std::uint64_t {
    auto joined = std::string{executable};
    joined.push_back('\0');
    joined += directory;
    joined.push_back('\0');
    joined += arguments;
    return fnv1a(joined);
}

// Two well-formed keys for tests that only need distinct rows.
const auto first_key = std::string(LIQUID_WRAP_KEY_HEX_SIZE, 'a');
const auto second_key = std::string(LIQUID_WRAP_KEY_HEX_SIZE, 'b');

auto digest_of(std::string_view bytes) -> std::string {
    auto output = std::array<char, LIQUID_WRAP_DIGEST_HEX_SIZE + 1>{};
    liquid_wrap_digest_hex(bytes.data(), bytes.size(), output.data());
    return output.data();
}

auto write_file(const std::filesystem::path& path, std::string_view bytes, ::mode_t mode = 0600)
    -> void {
    std::filesystem::remove(path);
    {
        auto output = std::ofstream{path, std::ios::binary};
        output << bytes;
    }
    if (::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("chmod " + path.string());
    }
}

class scratch final {
public:
    scratch() {
        auto text = (std::filesystem::temp_directory_path() / "lq-plan-XXXXXX").string();
        if (::mkdtemp(text.data()) == nullptr) {
            throw std::runtime_error("mkdtemp");
        }
        path_ = text;
        std::filesystem::create_directories(path_ / "mount");
    }

    ~scratch() {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    scratch(const scratch&) = delete;
    auto operator=(const scratch&) -> scratch& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

    [[nodiscard]] auto mount() const -> std::filesystem::path {
        return path_ / "mount";
    }

private:
    std::filesystem::path path_;
};

// A plan with a hit for `key` (status 7) conditional on mount/input having the bytes it has now.
auto conditional_plan(const std::string& key, const std::filesystem::path& mount) -> std::string {
    auto digest = std::array<char, LIQUID_WRAP_DIGEST_HEX_SIZE + 1>{};
    auto size = std::uint64_t{};
    if (liquid_wrap_file_digest_hex((mount / "input").c_str(), digest.data(), &size) != 0) {
        throw std::runtime_error("cannot hash the conditional input");
    }
    auto output = std::ostringstream{};
    output << "H\t" << key << "\t7\n"
           << "C\t" << key << '\t' << digest.data() << '\t' << size << '\t' << hex("input") << '\n';
    return output.str();
}

} // namespace

TEST(WrapPlan, a_changed_input_with_the_same_fnv1a_value_no_longer_hits) {
    ASSERT_EQ(recorded_input.size(), colliding_input.size());
    ASSERT_EQ(fnv1a(recorded_input), fnv1a(colliding_input));
    scratch location;
    write_file(location.mount() / "input", recorded_input);
    const auto plan = conditional_plan(first_key, location.mount());
    const auto mount_path = location.mount();
    const auto* mount = mount_path.c_str();

    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), first_key.c_str(), mount), 7);
    write_file(location.mount() / "input", colliding_input);
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), first_key.c_str(), mount), -1);
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), second_key.c_str(), mount), -1);
}

TEST(WrapPlan, a_condition_in_the_old_fnv1a_form_is_a_miss) {
    scratch location;
    write_file(location.mount() / "input", recorded_input);
    auto old = std::ostringstream{};
    old << "H\t" << first_key << "\t7\nC\t" << first_key << '\t' << std::hex
        << fnv1a(recorded_input) << "\t16\t" << hex("input") << '\n';
    const auto plan = old.str();
    EXPECT_EQ(liquid_wrap_plan_status(
                  plan.data(), plan.size(), first_key.c_str(), location.mount().c_str()),
              -1);
}

TEST(WrapPlan, command_lines_whose_fnv1a_keys_collide_get_different_keys_and_do_not_hit) {
    // The colliding pair as executables: FNV-1a reaches the same state after either, so the old
    // keys of two commands that differ only there are equal, whatever follows.
    constexpr auto directory = std::string_view{"/liquid/work"};
    constexpr auto arguments = std::string_view{"cc\0-c\0main.c\0", 13};
    ASSERT_EQ(old_process_key(recorded_input, directory, arguments),
              old_process_key(colliding_input, directory, arguments));

    const auto recorded = process_key(recorded_input, directory, arguments);
    const auto colliding = process_key(colliding_input, directory, arguments);
    ASSERT_EQ(recorded.size(), std::size_t{LIQUID_WRAP_KEY_HEX_SIZE});
    EXPECT_NE(recorded, colliding);
    EXPECT_EQ(recorded, process_key(recorded_input, directory, arguments));

    // A plan that answers the recorded command does not answer the colliding one.
    const auto plan = "H\t" + recorded + "\t0\n";
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), recorded.c_str(), "/"), 0);
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), colliding.c_str(), "/"), -1);
}

TEST(WrapPlan, a_process_key_hashes_each_part_with_its_length) {
    // Moving a byte from one part to the next is a different command and a different key.
    EXPECT_NE(process_key("/bin/ab", "/liquid/work", std::string_view{"x\0", 2}),
              process_key("/bin/a", "b/liquid/work", std::string_view{"x\0", 2}));
    EXPECT_NE(process_key("/bin/cc", "/liquid/work", std::string_view{"a\0b\0", 4}),
              process_key("/bin/cc", "/liquid/work", std::string_view{"a\0b\0\0", 5}));
}

TEST(WrapPlan, a_row_whose_key_is_not_64_lowercase_hex_digits_never_hits) {
    const auto key = process_key("/bin/cc", "/liquid/work", std::string_view{"cc\0", 3});
    auto upper = key;
    for (auto& character : upper) {
        if (character >= 'a' && character <= 'f') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    auto old = std::ostringstream{};
    old << "H\t" << std::hex
        << old_process_key("/bin/cc", "/liquid/work", std::string_view{"cc\0", 3}) << "\t0\n";
    for (const auto& plan : {old.str(),
                             "H\t" + upper + "\t0\n",
                             "H\t" + key + "00\t0\n",
                             "H\t" + key.substr(0, 32) + "\t0\n"}) {
        EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), key.c_str(), "/"), -1) << plan;
    }
    // Nor is a key that is not one: the hook never looks it up.
    const auto plan = "H\t" + key + "\t0\n";
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), "2a", "/"), -1);
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), key.c_str(), "/"), 0);
}

TEST(WrapState, a_state_with_64_bit_process_keys_is_ignored_and_rebuilt) {
    scratch location;
    const auto state = location.path() / "state";
    const auto result = liquid::to_string(liquid::digest{liquid::algorithm::blake3, {}});

    // Written before liquid#449: no version line, 64-bit FNV-1a keys.
    write_file(state, "O\t" + hex("out/app") + "\nA\t3f59d21c3718158e\t" + result + "\n");
    auto old = liquid::build_detail::saved_wrap_action_keys(state);
    ASSERT_TRUE(old.has_value()) << old.error().message;
    EXPECT_TRUE(old->empty());

    // An unknown version is ignored the same way.
    write_file(state, "V\t3\nA\t" + first_key + '\t' + result + "\n");
    auto future = liquid::build_detail::saved_wrap_action_keys(state);
    ASSERT_TRUE(future.has_value()) << future.error().message;
    EXPECT_TRUE(future->empty());

    // The current version carries the BLAKE3-256 key as written.
    write_file(state, "V\t2\nA\t" + first_key + '\t' + result + "\n");
    auto current = liquid::build_detail::saved_wrap_action_keys(state);
    ASSERT_TRUE(current.has_value()) << current.error().message;
    EXPECT_EQ(*current, std::vector<std::string>{first_key});

    // A current state with an old key in it is corrupt, never read as a key.
    write_file(state, "V\t2\nA\t3f59d21c3718158e\t" + result + "\n");
    EXPECT_FALSE(liquid::build_detail::saved_wrap_action_keys(state).has_value());
}

TEST(WrapPlan, a_plan_is_read_only_when_its_bytes_hash_to_the_digest_liquid_passed) {
    scratch location;
    const auto plan = "H\t" + first_key + "\t0\n";
    const auto path = location.path() / "plan";
    write_file(path, plan);
    auto size = std::size_t{};

    auto* loaded = liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 1, &size);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(std::string(loaded, size), plan);
    std::free(loaded);

    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), nullptr, 1, &size), nullptr);
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), "not a digest", 1, &size), nullptr);
    const auto other = "H\t" + second_key + "\t0\n";
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), digest_of(other).c_str(), 1, &size), nullptr);

    // A row added after liquid hashed the plan.
    write_file(path, plan + other);
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 1, &size), nullptr);
}

TEST(WrapPlan, a_private_plan_is_required_and_a_link_is_never_followed) {
    scratch location;
    const auto plan = "H\t" + first_key + "\t0\n";
    const auto path = location.path() / "plan";
    auto size = std::size_t{};

    write_file(path, plan, 0644);
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 1, &size), nullptr);
    auto* loaded = liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 0, &size);
    EXPECT_NE(loaded, nullptr);
    std::free(loaded);

    write_file(location.path() / "elsewhere", plan);
    std::filesystem::remove(path);
    std::filesystem::create_symlink(location.path() / "elsewhere", path);
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 0, &size), nullptr);
}

#if defined(__APPLE__) && defined(LIQUID_WRAP_PROBE) && defined(LIQUID_TEST_WRAP_HOOK)

namespace {

// Runs the probe with the real hook preloaded and `variables` added to this process's
// environment. Returns its exit status.
auto run_probe(const std::map<std::string, std::string>& variables,
               const std::filesystem::path& marker) -> int {
    auto storage = std::vector<std::string>{};
    for (auto** entry = environ; *entry != nullptr; ++entry) {
        const auto text = std::string_view{*entry};
        const auto name = text.substr(0, text.find('='));
        if (!variables.contains(std::string{name}) && !name.starts_with("LIQUID_WRAP_") &&
            name != "DYLD_INSERT_LIBRARIES") {
            storage.emplace_back(text);
        }
    }
    for (const auto& [name, value] : variables) {
        storage.push_back(name + "=" + value);
    }
    storage.push_back(std::string{"DYLD_INSERT_LIBRARIES="} + LIQUID_TEST_WRAP_HOOK);
    auto environment = std::vector<char*>{};
    for (auto& entry : storage) {
        environment.push_back(entry.data());
    }
    environment.push_back(nullptr);
    auto program = std::string{LIQUID_WRAP_PROBE};
    auto argument = marker.string();
    auto arguments = std::array<char*, 3>{program.data(), argument.data(), nullptr};
    auto child = pid_t{};
    if (::posix_spawn(
            &child, program.c_str(), nullptr, nullptr, arguments.data(), environment.data()) != 0) {
        throw std::runtime_error("posix_spawn " + program);
    }
    auto status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        throw std::runtime_error("probe did not exit");
    }
    return WEXITSTATUS(status);
}

// The process key the hook computes for the probe with this marker, read from its trace.
auto probe_key(const scratch& location, const std::filesystem::path& marker) -> std::string {
    const auto trace = location.path() / "trace";
    write_file(trace, "");
    run_probe({{"LIQUID_WRAP_TRACE", trace.string()}}, marker);
    std::filesystem::remove(marker);
    auto input = std::ifstream{trace};
    auto pid = std::string{};
    auto parent = std::string{};
    auto key = std::string{};
    std::getline(input, pid, '\t');
    std::getline(input, parent, '\t');
    std::getline(input, key, '\t');
    return key;
}

} // namespace

TEST(WrapHook, macos_hook_runs_the_tool_unless_a_bound_plan_beside_the_mount_matches) {
    scratch location;
    const auto marker = location.path() / "marker";
    const auto key = probe_key(location, marker);
    write_file(location.mount() / "input", recorded_input);
    const auto plan = conditional_plan(key, location.mount());
    const auto plan_path = location.path() / "plan";
    write_file(plan_path, plan);
    auto variables = std::map<std::string, std::string>{
        {"LIQUID_WRAP_PLAN", plan_path.string()},
        {"LIQUID_WRAP_ROOT", location.mount().string()},
        {LIQUID_WRAP_PLAN_DIGEST_VARIABLE, digest_of(plan)},
    };

    // The plan liquid wrote, for the input as recorded: answered from the plan.
    EXPECT_EQ(run_probe(variables, marker), 7);
    EXPECT_FALSE(std::filesystem::exists(marker));

    // The input changed to bytes with the same size and FNV-1a value: the tool runs.
    write_file(location.mount() / "input", colliding_input);
    EXPECT_EQ(run_probe(variables, marker), 0);
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);
    write_file(location.mount() / "input", recorded_input);

    // A digest that is not the plan's: the plan is ignored.
    auto wrong_digest = variables;
    wrong_digest[LIQUID_WRAP_PLAN_DIGEST_VARIABLE] = digest_of("H\t" + second_key + "\t0\n");
    EXPECT_EQ(run_probe(wrong_digest, marker), 0);
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);

    // No digest at all: ignored.
    auto no_digest = variables;
    no_digest.erase(LIQUID_WRAP_PLAN_DIGEST_VARIABLE);
    EXPECT_EQ(run_probe(no_digest, marker), 0);
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);

    // The same, correctly bound plan planted anywhere but beside the mount: ignored.
    std::filesystem::create_directories(location.path() / "planted");
    const auto planted = location.path() / "planted" / "plan";
    write_file(planted, plan);
    auto elsewhere = variables;
    elsewhere["LIQUID_WRAP_PLAN"] = planted.string();
    EXPECT_EQ(run_probe(elsewhere, marker), 0);
    EXPECT_TRUE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);

    // A plan other users could write: ignored.
    ASSERT_EQ(::chmod(plan_path.c_str(), 0666), 0);
    EXPECT_EQ(run_probe(variables, marker), 0);
    EXPECT_TRUE(std::filesystem::exists(marker));
}

#endif

#endif

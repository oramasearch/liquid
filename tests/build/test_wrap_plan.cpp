// liquid#401: the wrap hook revalidates conditional inputs with BLAKE3 (never FNV-1a) and reads a
// plan only from liquid's own location, bound to the digest liquid wrote it with.

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
auto conditional_plan(std::uint64_t key, const std::filesystem::path& mount) -> std::string {
    auto digest = std::array<char, LIQUID_WRAP_DIGEST_HEX_SIZE + 1>{};
    auto size = std::uint64_t{};
    if (liquid_wrap_file_digest_hex((mount / "input").c_str(), digest.data(), &size) != 0) {
        throw std::runtime_error("cannot hash the conditional input");
    }
    auto output = std::ostringstream{};
    output << "H\t" << std::hex << key << "\t7\n"
           << "C\t" << std::hex << key << '\t' << digest.data() << '\t' << std::dec << size << '\t'
           << hex("input") << '\n';
    return output.str();
}

} // namespace

TEST(WrapPlan, a_changed_input_with_the_same_fnv1a_value_no_longer_hits) {
    ASSERT_EQ(recorded_input.size(), colliding_input.size());
    ASSERT_EQ(fnv1a(recorded_input), fnv1a(colliding_input));
    scratch location;
    write_file(location.mount() / "input", recorded_input);
    const auto plan = conditional_plan(0x2a, location.mount());

    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), 0x2a, location.mount().c_str()), 7);
    write_file(location.mount() / "input", colliding_input);
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), 0x2a, location.mount().c_str()),
              -1);
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), 0x2b, location.mount().c_str()),
              -1);
}

TEST(WrapPlan, a_condition_in_the_old_fnv1a_form_is_a_miss) {
    scratch location;
    write_file(location.mount() / "input", recorded_input);
    auto old = std::ostringstream{};
    old << "H\t2a\t7\nC\t2a\t" << std::hex << fnv1a(recorded_input) << "\t16\t" << hex("input")
        << '\n';
    const auto plan = old.str();
    EXPECT_EQ(liquid_wrap_plan_status(plan.data(), plan.size(), 0x2a, location.mount().c_str()),
              -1);
}

TEST(WrapPlan, a_plan_is_read_only_when_its_bytes_hash_to_the_digest_liquid_passed) {
    scratch location;
    const auto plan = std::string{"H\t2a\t0\n"};
    const auto path = location.path() / "plan";
    write_file(path, plan);
    auto size = std::size_t{};

    auto* loaded = liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 1, &size);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(std::string(loaded, size), plan);
    std::free(loaded);

    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), nullptr, 1, &size), nullptr);
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), "not a digest", 1, &size), nullptr);
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), digest_of("H\t2b\t0\n").c_str(), 1, &size),
              nullptr);

    // A row added after liquid hashed the plan.
    write_file(path, plan + "H\t2b\t0\n");
    EXPECT_EQ(liquid_wrap_plan_load(path.c_str(), digest_of(plan).c_str(), 1, &size), nullptr);
}

TEST(WrapPlan, a_private_plan_is_required_and_a_link_is_never_followed) {
    scratch location;
    const auto plan = std::string{"H\t2a\t0\n"};
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
auto probe_key(const scratch& location, const std::filesystem::path& marker) -> std::uint64_t {
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
    return std::stoull(key, nullptr, 16);
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
    wrong_digest[LIQUID_WRAP_PLAN_DIGEST_VARIABLE] = digest_of("H\t0\t0\n");
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

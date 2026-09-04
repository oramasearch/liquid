#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "build/cache.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "build/remote_blob_source.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "sandbox/environment.hpp"
#include "vfs/repository_access.hpp"

namespace {

auto timing(std::chrono::milliseconds duration) -> liquid::action_timing {
    return {.exec = duration};
}

TEST(BuildDistribution, mixed_workload_selects_local_remote_and_race) {
    auto tiny = liquid::build::choose_execution({.history = {timing(std::chrono::milliseconds{12}),
                                                             timing(std::chrono::milliseconds{14}),
                                                             timing(std::chrono::milliseconds{13})},
                                                 .input_bytes = 1024,
                                                 .remote_state_queried = true,
                                                 .remote_missing_bytes = 0,
                                                 .remote_queue_depth = 0,
                                                 .remote_cache_locality_permille = 1000});
    EXPECT_EQ(tiny.decision, liquid::execution_decision::local);
    EXPECT_EQ(tiny.inputs.size_class, liquid::action_size_class::tiny);
    EXPECT_EQ(tiny.local_estimate, std::chrono::milliseconds{13});
    EXPECT_EQ(tiny.remote_estimate, std::chrono::milliseconds{29});

    auto short_batch =
        liquid::build::choose_execution({.history = {timing(std::chrono::milliseconds{80}),
                                                     timing(std::chrono::milliseconds{100}),
                                                     timing(std::chrono::milliseconds{90})},
                                         .input_bytes = 32U * 1024U,
                                         .remote_state_queried = true,
                                         .remote_missing_bytes = 0,
                                         .remote_queue_depth = 0,
                                         .remote_cache_locality_permille = 1000,
                                         .batch_supported = true,
                                         .batch_size = 8});
    EXPECT_EQ(short_batch.decision, liquid::execution_decision::remote);
    EXPECT_EQ(short_batch.inputs.size_class, liquid::action_size_class::short_action);
    EXPECT_EQ(short_batch.local_estimate, std::chrono::milliseconds{90});
    EXPECT_EQ(short_batch.remote_estimate, std::chrono::milliseconds{87});

    auto long_action =
        liquid::build::choose_execution({.history = {timing(std::chrono::milliseconds{3200}),
                                                     timing(std::chrono::milliseconds{3400}),
                                                     timing(std::chrono::milliseconds{3600})},
                                         .input_bytes = 64U * 1024U * 1024U,
                                         .remote_state_queried = true,
                                         .remote_missing_bytes = 8U * 1024U * 1024U,
                                         .remote_queue_depth = 1,
                                         .remote_cache_locality_permille = 875});
    EXPECT_EQ(long_action.decision, liquid::execution_decision::remote);
    EXPECT_EQ(long_action.inputs.size_class, liquid::action_size_class::long_action);
    EXPECT_EQ(long_action.local_estimate, std::chrono::milliseconds{3400});
    EXPECT_EQ(long_action.remote_estimate, std::chrono::milliseconds{2700});

    auto uncertain =
        liquid::build::choose_execution({.history = {timing(std::chrono::milliseconds{400}),
                                                     timing(std::chrono::milliseconds{900}),
                                                     timing(std::chrono::milliseconds{2400})},
                                         .input_bytes = 16U * 1024U * 1024U,
                                         .remote_state_queried = true,
                                         .remote_missing_bytes = 4U * 1024U * 1024U,
                                         .remote_queue_depth = 0,
                                         .remote_cache_locality_permille = 750});
    EXPECT_EQ(uncertain.decision, liquid::execution_decision::race);
    EXPECT_EQ(uncertain.inputs.size_class, liquid::action_size_class::medium);
    EXPECT_EQ(uncertain.local_estimate, std::chrono::milliseconds{900});
    EXPECT_EQ(uncertain.remote_estimate, std::chrono::milliseconds{735});
}

TEST(BuildDistribution, unknown_and_unbatched_short_work_stays_local) {
    const auto unknown = liquid::build::choose_execution({});
    EXPECT_EQ(unknown.decision, liquid::execution_decision::local);
    EXPECT_EQ(unknown.inputs.size_class, liquid::action_size_class::unknown);

    const auto short_action =
        liquid::build::choose_execution({.history = {timing(std::chrono::milliseconds{100})},
                                         .remote_state_queried = true,
                                         .remote_cache_locality_permille = 1000});
    EXPECT_EQ(short_action.decision, liquid::execution_decision::local);
    EXPECT_EQ(short_action.inputs.size_class, liquid::action_size_class::short_action);
}

#if LIQUID_BUILD_HAS_FUSE && defined(__linux__)

class temporary_directory {
public:
    explicit temporary_directory(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                (std::string{name} + "-" + std::to_string(::getpid()) + "-" +
                 std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
        EXPECT_FALSE(code) << code.message();
    }

    ~temporary_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto host_platform() -> liquid::platform {
#if defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
}

class distributed_fixture {
public:
    distributed_fixture() : location_("liquid-distribution-source") {
        auto options = liquid::repository_options{};
        options.cache.publish = true;
        auto created = liquid::runtime::block_on(
            liquid::repository::create(location_.path() / "repository", options));
        EXPECT_TRUE(created) << created.error().message;
        repository = std::move(*created);
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("input.txt", bytes("input"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("other.txt", bytes("other"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("out/copy.txt", {})));
        EXPECT_TRUE(
            liquid::runtime::block_on(repository.write_file("bin/fixture", LIQUID_BUILD_FIXTURE)));
        EXPECT_TRUE(liquid::runtime::block_on(
            liquid::repository_detail::vfs_access::set_mode(repository, "bin/fixture", 0100755)));
        EXPECT_TRUE(liquid::runtime::block_on(repository.cut("distributed fixture")));
        auto log = liquid::runtime::block_on(repository.log());
        EXPECT_TRUE(log) << log.error().message;
        root = log->front().root;
    }

    auto input() const -> liquid::action_input {
        return {.command = {"fixture", "copy", "input.txt", "out/copy.txt"},
                .cwd = "",
                .env = {},
                .outputs = {"out/copy.txt"},
                .inputs = root,
                .toolchain = root,
                .platform = host_platform(),
                .timeout = std::chrono::seconds{3}};
    }

    liquid::repository repository;
    liquid::digest root;

private:
    temporary_directory location_;
};

liquid::runtime::task<void> materialise_remote_tree(liquid::store& store,
                                                    liquid::digest root,
                                                    std::filesystem::path destination) {
    auto record = co_await store.get(root);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto tree = liquid::objects::decode_tree(*record);
    if (!tree) {
        co_return std::unexpected(tree.error());
    }
    auto code = std::error_code{};
    std::filesystem::create_directories(destination, code);
    if (code) {
        co_return std::unexpected(
            liquid::error{liquid::errc::io, code.message(), destination.string()});
    }
    for (const auto& entry : tree->entries) {
        const auto target = destination / entry.name;
        if (entry.kind == liquid::node_kind::tree) {
            auto nested = co_await materialise_remote_tree(store, entry.ref, target);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            continue;
        }
        if (entry.kind != liquid::node_kind::blob) {
            continue;
        }
        auto blob_record = co_await store.get(entry.ref);
        if (!blob_record) {
            co_return std::unexpected(blob_record.error());
        }
        auto blob = liquid::objects::decode_blob(*blob_record);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        auto output = std::ofstream{target, std::ios::binary | std::ios::trunc};
        for (const auto& chunk : blob->chunks) {
            auto content = co_await store.get(chunk);
            if (!content) {
                co_return std::unexpected(content.error());
            }
            output.write(reinterpret_cast<const char*>(content->data()),
                         static_cast<std::streamsize>(content->size()));
        }
        output.close();
        std::filesystem::permissions(target,
                                     static_cast<std::filesystem::perms>(entry.mode & 0777U),
                                     std::filesystem::perm_options::replace,
                                     code);
        if (!output || code) {
            co_return std::unexpected(liquid::error{
                liquid::errc::io, code ? code.message() : "remote write failed", target.string()});
        }
    }
    co_return liquid::result<void>{};
}

liquid::runtime::task<liquid::digest> store_remote_blob(liquid::store& store,
                                                        std::vector<std::byte> content) {
    const auto size = content.size();
    auto chunk = co_await store.put(std::move(content));
    if (!chunk) {
        co_return std::unexpected(chunk.error());
    }
    auto encoded = liquid::objects::encode(liquid::blob{{*chunk}, size, {size}});
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await store.put(std::move(*encoded));
}

class materialising_remote_executor final : public liquid::remote_executor {
public:
    materialising_remote_executor(std::shared_ptr<liquid::test_support::on_disk_remote_source> blobs,
                                  std::filesystem::path root,
                                  std::chrono::milliseconds delay = {})
        : blobs_(std::move(blobs)), root_(std::move(root)), delay_(delay) {}

    auto blobs() noexcept -> liquid::remote_blob_source& override {
        return *blobs_;
    }

    auto instance() const -> std::string override {
        return "worker-a";
    }

    auto queue_depth() const noexcept -> std::uint64_t override {
        return 0;
    }

    auto supports_batch() const noexcept -> bool override {
        return true;
    }

    auto execute(liquid::action_input input)
        -> liquid::runtime::task<liquid::action_result> override {
        const auto stop = co_await boost::capy::this_coro::stop_token;
        auto cancelled = std::stop_callback{stop, [this] { cancelled_ = true; }};
        if (delay_.count() != 0) {
            auto [delay_error] = co_await boost::capy::delay(delay_);
            if (delay_error) {
                co_return std::unexpected(liquid::error{
                    liquid::errc::unavailable, "remote action was cancelled", "test executor"});
            }
        }
        const auto run = root_ / ("run-" + std::to_string(executions_++));
        const auto work = run / "work";
        const auto toolchain = run / "toolchain";
        auto materialised = co_await materialise_remote_tree(blobs_->store(), input.inputs, work);
        if (!materialised) {
            co_return std::unexpected(materialised.error());
        }
        materialised =
            co_await materialise_remote_tree(blobs_->store(), input.toolchain, toolchain);
        if (!materialised) {
            co_return std::unexpected(materialised.error());
        }
        materialised_toolchain_ = std::filesystem::is_regular_file(toolchain / "bin/fixture");
        auto command = std::string{"cd '"} + (work / input.cwd).string() + "' && '" +
                       (toolchain / "bin/fixture").string() +
                       "' copy input.txt out/copy.txt >/dev/null 2>/dev/null";
        const auto started = std::chrono::steady_clock::now();
        const auto status = std::system(command.c_str());
        const auto finished = std::chrono::steady_clock::now();
        if (status < 0 || !WIFEXITED(status)) {
            co_return std::unexpected(
                liquid::error{liquid::errc::io, "remote fixture failed", command});
        }
        auto output = std::ifstream{work / "out/copy.txt", std::ios::binary};
        auto content = std::vector<std::byte>{};
        for (char value; output.get(value);) {
            content.push_back(std::byte{static_cast<unsigned char>(value)});
        }
        if (!output.eof()) {
            co_return std::unexpected(liquid::error{
                liquid::errc::io, "cannot read remote fixture output", work.string()});
        }
        auto output_blob = co_await store_remote_blob(blobs_->store(), std::move(content));
        auto empty_blob = co_await store_remote_blob(blobs_->store(), {});
        if (!output_blob || !empty_blob) {
            co_return std::unexpected(!output_blob ? output_blob.error() : empty_blob.error());
        }
        co_return liquid::action_result{
            .exit = WEXITSTATUS(status),
            .outputs = {{"out/copy.txt", 0644, *output_blob}},
            .stdout_output = *empty_blob,
            .stderr_output = *empty_blob,
            .timing = {.exec = std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                                     started)},
            .worker = "test-remote",
            .observed_at =
                std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
    }

    auto execute_batch(std::vector<liquid::action_input> inputs)
        -> liquid::runtime::task<std::vector<liquid::action_result>> override {
        ++batch_calls_;
        auto output = std::vector<liquid::action_result>{};
        for (auto& input : inputs) {
            auto value = co_await execute(std::move(input));
            if (!value) {
                co_return std::unexpected(value.error());
            }
            output.push_back(std::move(*value));
        }
        co_return output;
    }

    auto materialised_toolchain() const noexcept -> bool {
        return materialised_toolchain_;
    }

    auto cancelled() const noexcept -> bool {
        return cancelled_;
    }

    auto batch_calls() const noexcept -> std::uint64_t {
        return batch_calls_;
    }

private:
    std::shared_ptr<liquid::test_support::on_disk_remote_source> blobs_;
    std::filesystem::path root_;
    std::chrono::milliseconds delay_{};
    std::atomic_uint64_t executions_{};
    std::atomic_uint64_t batch_calls_{};
    std::atomic_bool materialised_toolchain_{};
    std::atomic_bool cancelled_{};
};

auto tree_blob(liquid::repository& repository, liquid::digest root, std::string_view name)
    -> liquid::digest {
    auto raw =
        liquid::runtime::block_on(liquid::repository_detail::build_access::get(repository, root));
    EXPECT_TRUE(raw) << raw.error().message;
    auto tree = liquid::objects::decode_tree(*raw);
    EXPECT_TRUE(tree) << tree.error().message;
    const auto found = std::ranges::find(tree->entries, name, &liquid::tree_entry::name);
    EXPECT_NE(found, tree->entries.end());
    return found->ref;
}

auto seed_history(distributed_fixture& fixture,
                  std::initializer_list<std::chrono::milliseconds> durations) -> std::size_t {
    const auto input = fixture.input();
    auto sandbox = liquid::sandbox_detail::definition(input.platform);
    EXPECT_TRUE(sandbox) << sandbox.error().message;
    auto sandbox_record = liquid::objects::encode(*sandbox);
    EXPECT_TRUE(sandbox_record) << sandbox_record.error().message;
    auto sandbox_id = liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
        fixture.repository, std::move(*sandbox_record)));
    EXPECT_TRUE(sandbox_id) << sandbox_id.error().message;
    auto cache = liquid::build_detail::disk_cache{fixture.repository};
    EXPECT_TRUE(liquid::runtime::block_on(cache.open()));
    auto candidate = cache.candidate_key(input, *sandbox_id);
    EXPECT_TRUE(candidate) << candidate.error().message;
    const auto wrong_input = tree_blob(fixture.repository, fixture.root, "other.txt");
    const auto empty_path = std::filesystem::temp_directory_path() /
                            ("liquid-distribution-empty-" + std::to_string(::getpid()));
    {
        auto empty = std::ofstream{empty_path, std::ios::trunc};
    }
    auto empty_blob = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_file(fixture.repository, empty_path));
    std::filesystem::remove(empty_path);
    EXPECT_TRUE(empty_blob) << empty_blob.error().message;
    for (const auto duration : durations) {
        auto value = liquid::action_result{.stdout_output = *empty_blob,
                                           .stderr_output = *empty_blob,
                                           .manifest = {{"input.txt", wrong_input}},
                                           .timing = {.exec = duration},
                                           .worker = "history",
                                           .observed_at = std::chrono::floor<std::chrono::seconds>(
                                               std::chrono::system_clock::now())};
        auto stored =
            liquid::runtime::block_on(liquid::build_detail::testing::store_result_with_provenance(
                fixture.repository, liquid::objects::action_record{input, *sandbox_id}, value));
        EXPECT_TRUE(stored) << stored.error().message;
        EXPECT_TRUE(liquid::runtime::block_on(cache.publish(*candidate, *stored, value)));
    }
    return cache.entries().size();
}

auto output_contents(liquid::repository& repository, liquid::result_id id)
    -> std::vector<std::byte> {
    auto value = liquid::runtime::block_on(liquid::build::result(repository, id));
    EXPECT_TRUE(value) << value.error().message;
    EXPECT_EQ(value->outputs.size(), 1U);
    auto record = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(repository, value->outputs.front().blob));
    EXPECT_TRUE(record) << record.error().message;
    auto blob = liquid::objects::decode_blob(*record);
    EXPECT_TRUE(blob) << blob.error().message;
    auto output = std::vector<std::byte>{};
    for (const auto& chunk : blob->chunks) {
        auto content = liquid::runtime::block_on(
            liquid::repository_detail::build_access::get(repository, chunk));
        EXPECT_TRUE(content) << content.error().message;
        output.insert(output.end(), content->begin(), content->end());
    }
    return output;
}

TEST(BuildDistribution, remote_double_materialises_toolchain_from_store_and_matches_local) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    distributed_fixture local_fixture;
    auto local = liquid::runtime::block_on(
        liquid::build::execute(local_fixture.repository, local_fixture.input()));
    ASSERT_TRUE(local) << local.error().message << ": " << local.error().context;

    distributed_fixture remote_fixture;
    seed_history(remote_fixture,
                 {std::chrono::milliseconds{3200},
                  std::chrono::milliseconds{3400},
                  std::chrono::milliseconds{3600}});
    temporary_directory remote_location{"liquid-distribution-remote"};
    auto store = liquid::runtime::block_on(liquid::store::open(remote_location.path() / "store"));
    ASSERT_TRUE(store) << store.error().message;
    auto source =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(*store));
    materialising_remote_executor executor{source, remote_location.path() / "runs"};
    auto remote = liquid::runtime::block_on(
        liquid::build::execute(remote_fixture.repository, remote_fixture.input(), executor));
    ASSERT_TRUE(remote) << remote.error().message << ": " << remote.error().context;
    EXPECT_TRUE(executor.materialised_toolchain());
    EXPECT_EQ(output_contents(remote_fixture.repository, *remote),
              output_contents(local_fixture.repository, *local));
    auto value =
        liquid::runtime::block_on(liquid::build::result(remote_fixture.repository, *remote));
    ASSERT_TRUE(value) << value.error().message;
    ASSERT_TRUE(value->schedule);
    EXPECT_EQ(value->schedule->decision, liquid::execution_decision::remote);
    EXPECT_TRUE(value->schedule->inputs.remote_state_queried);
    EXPECT_GT(value->schedule->inputs.remote_missing_bytes, 0U);
    EXPECT_EQ(value->worker, "test-remote");
}

TEST(BuildDistribution, short_actions_share_one_remote_batch) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    distributed_fixture fixture;
    seed_history(fixture,
                 {std::chrono::milliseconds{80},
                  std::chrono::milliseconds{90},
                  std::chrono::milliseconds{100}});
    temporary_directory remote_location{"liquid-distribution-batch"};
    auto store = liquid::runtime::block_on(liquid::store::open(remote_location.path() / "store"));
    ASSERT_TRUE(store) << store.error().message;
    auto source =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(*store));
    materialising_remote_executor executor{source, remote_location.path() / "runs"};
    auto inputs = std::vector<liquid::action_input>(4, fixture.input());
    auto results = liquid::runtime::block_on(
        liquid::build::execute_batch(fixture.repository, std::move(inputs), executor));
    ASSERT_TRUE(results) << results.error().message << ": " << results.error().context;
    ASSERT_EQ(results->size(), 4U);
    EXPECT_EQ(executor.batch_calls(), 1U);
    for (const auto& id : *results) {
        auto value = liquid::runtime::block_on(liquid::build::result(fixture.repository, id));
        ASSERT_TRUE(value) << value.error().message;
        ASSERT_TRUE(value->schedule);
        EXPECT_EQ(value->schedule->decision, liquid::execution_decision::remote);
        EXPECT_EQ(value->schedule->inputs.size_class, liquid::action_size_class::short_action);
        EXPECT_EQ(value->schedule->inputs.batch_size, 4U);
    }
}

TEST(BuildDistribution, race_publishes_local_winner_and_cancels_remote_loser) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    distributed_fixture fixture;
    const auto initial_entries = seed_history(fixture,
                                              {std::chrono::milliseconds{400},
                                               std::chrono::milliseconds{900},
                                               std::chrono::milliseconds{2400}});
    temporary_directory remote_location{"liquid-distribution-race"};
    auto store = liquid::runtime::block_on(liquid::store::open(remote_location.path() / "store"));
    ASSERT_TRUE(store) << store.error().message;
    auto source =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(*store));
    materialising_remote_executor executor{
        source, remote_location.path() / "runs", std::chrono::seconds{5}};
    auto winner = liquid::runtime::block_on(
        liquid::build::execute(fixture.repository, fixture.input(), executor));
    ASSERT_TRUE(winner) << winner.error().message << ": " << winner.error().context;
    EXPECT_TRUE(executor.cancelled());
    auto value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *winner));
    ASSERT_TRUE(value) << value.error().message;
    ASSERT_TRUE(value->schedule);
    EXPECT_EQ(value->schedule->decision, liquid::execution_decision::race);
    EXPECT_NE(value->worker, "test-remote");
    EXPECT_EQ(output_contents(fixture.repository, *winner), bytes("input"));
    auto cache = liquid::build_detail::disk_cache{fixture.repository};
    ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
    EXPECT_EQ(cache.entries().size(), initial_entries + 1U);
}

#endif

} // namespace

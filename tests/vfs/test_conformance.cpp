#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

#include "../temporary_directory.hpp"
#include "conformance_backend.hpp"
#include "repository/testing.hpp"
#include "vfs/repository_access.hpp"

namespace {

using liquid::runtime::block_on;
using liquid::test::vfs_conformance::allow_unavailable;
#if !defined(_WIN32)
using liquid::test::vfs_conformance::filesystem_operations;
#endif
using liquid::test::vfs_conformance::selected_backend;
using liquid::vfs_detail::observed_operation;
using liquid::vfs_detail::observed_request;

const auto recorded_input = std::string{"recorded input\n"};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto operation_name(observed_operation operation) -> std::string_view {
    switch (operation) {
    case observed_operation::getattr:
        return "getattr";
    case observed_operation::readdir:
        return "readdir";
    case observed_operation::open:
        return "open";
    case observed_operation::read:
        return "read";
    case observed_operation::write:
        return "write";
    }
    return "unknown operation";
}

// The remote supplies deterministic logical chunks and records every requested range.
// A mount read must first fetch from here, then survive a new mount with this source offline.
class remote_source final : public liquid::remote_blob_source {
public:
    static constexpr auto chunk_size = std::size_t{4 * 1024 * 1024};
    std::vector<std::byte> content = std::vector<std::byte>(8 * chunk_size);
    liquid::blob record{};
    std::atomic_uint64_t calls{};
    std::atomic_uint64_t transferred{};
    std::atomic_bool online{true};
    std::atomic_uint64_t active{};
    std::atomic_uint64_t maximum_active{};

    void initialise() {
        for (auto index = std::size_t{}; index < content.size(); ++index) {
            content[index] = static_cast<std::byte>((index * 29 + index / chunk_size) & 0xff);
        }
        auto whole = liquid::hash(liquid::algorithm::blake3, content);
        ASSERT_TRUE(whole) << "read/hydration fixture: expected whole-content digest";
        record.content = *whole;
        record.size = content.size();
        record.remote = liquid::remote_blob_location{"conformance", "chunks"};
        for (auto offset = std::size_t{}; offset < content.size(); offset += chunk_size) {
            const auto chunk = std::span{content}.subspan(offset, chunk_size);
            auto id = liquid::hash(liquid::algorithm::blake3, chunk);
            ASSERT_TRUE(id) << "read/hydration fixture: expected chunk digest";
            record.chunks.push_back(*id);
            record.chunk_sizes.push_back(chunk_size);
        }
    }

    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string, std::vector<liquid::digest>) override {
        co_return std::vector<liquid::digest>{};
    }

    liquid::runtime::task<std::uint64_t> read_range(std::string instance,
                                                    liquid::digest id,
                                                    std::uint64_t offset,
                                                    std::span<std::byte> output) override {
        const auto pending = ++active;
        auto maximum = maximum_active.load();
        while (maximum < pending && !maximum_active.compare_exchange_weak(maximum, pending)) {
        }
        struct decrement {
            std::atomic_uint64_t& value;
            ~decrement() {
                --value;
            }
        } guard{active};
        ++calls;
        if (!online) {
            co_return std::unexpected(liquid::error{liquid::errc::unavailable, "fixture offline"});
        }
        if (instance != "chunks" ||
            (id.alg != record.content.alg || id.bytes != record.content.bytes) ||
            offset >= content.size() || output.size() > content.size() - offset) {
            co_return std::unexpected(
                liquid::error{liquid::errc::invalid_argument, "unexpected hydration range"});
        }
        co_await liquid::runtime::yield();
        std::copy_n(content.begin() + offset, output.size(), output.begin());
        transferred += output.size();
        co_return output.size();
    }

    liquid::runtime::task<void>
    write(std::string, liquid::digest, std::vector<std::byte>) override {
        co_return std::unexpected(
            liquid::error{liquid::errc::unsupported, "fixture remote is read-only"});
    }
};

#if !defined(_WIN32)
class descriptor {
public:
    explicit descriptor(std::shared_ptr<filesystem_operations> fs, int value)
        : fs_(std::move(fs)), value_(value) {}
    ~descriptor() {
        if (value_ >= 0) {
            fs_->release(value_);
        }
    }
    descriptor(const descriptor&) = delete;
    auto operator=(const descriptor&) -> descriptor& = delete;
    auto get() const -> int {
        return value_;
    }
    auto close() -> int {
        return fs_->release(std::exchange(value_, -1));
    }

private:
    std::shared_ptr<filesystem_operations> fs_;
    int value_;
};

liquid::runtime::task<std::string> read_range_async(std::shared_ptr<filesystem_operations> fs,
                                                    std::filesystem::path path,
                                                    std::uint64_t offset) {
    auto read = co_await liquid::runtime::offload(
        [fs, path = std::move(path), offset]() -> liquid::result<std::string> {
            auto file = descriptor{fs, fs->open(path.c_str(), O_RDONLY)};
            if (file.get() < 0) {
                return std::unexpected(
                    liquid::error{liquid::errc::io, "open: expected concurrent range handle"});
            }
            auto value = std::string(4096, '\0');
            const auto count = fs->pread(file.get(), value.data(), value.size(), offset);
            if (count != static_cast<ssize_t>(value.size())) {
                return std::unexpected(
                    liquid::error{liquid::errc::io, "read: expected full concurrent range"});
            }
            if (file.close() != 0) {
                return std::unexpected(
                    liquid::error{liquid::errc::io, "release: expected concurrent handle close"});
            }
            return value;
        });
    if (!read) {
        co_return std::unexpected(read.error());
    }
    co_return std::move(*read);
}

liquid::runtime::task<std::array<std::string, 2>>
read_concurrently(std::shared_ptr<filesystem_operations> fs,
                  std::filesystem::path path,
                  std::uint64_t first_offset,
                  std::uint64_t second_offset) {
    auto [error, first, second] = co_await liquid::runtime::when_all(
        liquid::runtime::as_io(read_range_async(fs, path, first_offset)),
        liquid::runtime::as_io(read_range_async(fs, path, second_offset)));
    if (error) {
        co_return std::unexpected(liquid::error{liquid::errc::io, error.message()});
    }
    if (!first) {
        co_return std::unexpected(first.error());
    }
    if (!second) {
        co_return std::unexpected(second.error());
    }
    co_return std::array{std::move(*first), std::move(*second)};
}

class VfsConformance : public testing::Test {
protected:
    std::filesystem::path root;
    liquid::repository repository;
    std::shared_ptr<remote_source> remote;
    liquid::digest input_tree;
    liquid::digest input_blob;
    std::string author;
    std::shared_ptr<liquid::vfs_detail::backend> handle;
    std::shared_ptr<filesystem_operations> fs;
    std::mutex observations_mutex;
    std::vector<observed_request> observations;
    std::unique_ptr<testing::ScopedTrace> backend_trace;
    std::optional<std::string> previous_identity_home;

    void SetUp() override {
        backend_trace = std::make_unique<testing::ScopedTrace>(
            __FILE__, __LINE__, "backend=" + std::string{selected_backend->name});
        static auto next = std::atomic_uint64_t{};
        root = liquid::test::fixture_path("vfs-conformance-" + std::to_string(next++));
        if (const auto* configured = std::getenv("LIQUID_HOME")) {
            previous_identity_home = configured;
        }
        ASSERT_EQ(::setenv("LIQUID_HOME", (root / "identity").c_str(), 1), 0)
            << "mount fixture: expected isolated signing identity";
        remote = std::make_shared<remote_source>();
        auto options = liquid::repository_options{};
        options.storage.remote_sources.emplace("conformance", remote);
        auto created = block_on(liquid::repository::create(root / "repository", options));
        ASSERT_TRUE(created) << "mount fixture: expected repository creation: "
                             << created.error().message;
        repository = std::move(*created);
        ASSERT_TRUE(block_on(repository.write("input.txt", bytes(recorded_input))))
            << "mount fixture: expected recorded input";
        ASSERT_TRUE(block_on(repository.write("nested/child.txt", bytes("child"))))
            << "mount fixture: expected nested input";
        ASSERT_TRUE(block_on(repository.write("deps/library.txt", bytes("dependency"))))
            << "mount fixture: expected reserved dependency input";
        auto tree = block_on(repository.root_digest());
        ASSERT_TRUE(tree) << "mount fixture: expected recorded input-tree digest";
        input_tree = *tree;
        auto entry =
            block_on(liquid::repository_detail::vfs_access::entry(repository, "input.txt"));
        ASSERT_TRUE(entry) << "mount fixture: expected input blob record";
        input_blob = entry->object;
        auto operations = block_on(repository.operations());
        ASSERT_TRUE(operations) << "mount fixture: expected author history";
        ASSERT_FALSE(operations->empty()) << "mount fixture: expected recorded seed writes";
        author = operations->front().by.taap_id;
        ASSERT_FALSE(author.empty()) << "mount fixture: expected a TAAP author";
    }

    void TearDown() override {
        if (handle) {
            stop();
        }
        repository = {};
        if (previous_identity_home) {
            ::setenv("LIQUID_HOME", previous_identity_home->c_str(), 1);
        } else {
            ::unsetenv("LIQUID_HOME");
        }
        auto error = std::error_code{};
        std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << "release/unmount: expected fixture cleanup: " << error.message();
    }

    auto path(std::string_view name = {}) const -> std::filesystem::path {
        return root / "mount" / name;
    }

    void
    mount(bool observed = false, bool read_only = false, std::vector<std::string> outputs = {}) {
        auto mounted = block_on(selected_backend->start(
            {.source = repository,
             .mountpoint = path(),
             .input_tree = observed ? std::optional{input_tree} : std::nullopt,
             .outputs = std::move(outputs),
             .observe =
                 [this](observed_request request) {
                     auto lock = std::scoped_lock{observations_mutex};
                     observations.push_back(std::move(request));
                 },
             .read_only = read_only}));
        if (!mounted && mounted.error().code == liquid::errc::unsupported && allow_unavailable) {
            GTEST_SKIP() << "mount: named backend unavailable: " << mounted.error().message << ": "
                         << mounted.error().context;
        }
        ASSERT_TRUE(mounted) << "mount: expected named backend to mount: "
                             << mounted.error().message << ": " << mounted.error().context;
        handle = std::move(mounted->lifecycle);
        fs = std::move(mounted->operations);
    }

    void stop() {
        auto stopped = block_on(handle->unmount());
        EXPECT_TRUE(stopped) << "release/unmount: expected clean backend shutdown: "
                             << stopped.error().message;
        handle.reset();
        fs.reset();
    }

    void expect_read(std::string_view name, std::string_view expected, std::uint64_t offset = 0) {
        auto file = descriptor{fs, fs->open(path(name).c_str(), O_RDONLY)};
        ASSERT_GE(file.get(), 0) << "open: expected readable " << name << ": "
                                 << std::strerror(errno);
        auto actual = std::string(expected.size(), '\0');
        ASSERT_EQ(fs->pread(file.get(), actual.data(), actual.size(), static_cast<off_t>(offset)),
                  static_cast<ssize_t>(expected.size()))
            << "read: expected complete requested range for " << name << ": "
            << std::strerror(errno);
        EXPECT_EQ(actual, expected) << "read: expected recorded bytes at offset " << offset;
        ASSERT_EQ(file.close(), 0) << "release: expected successful close after read";
    }

    auto names(std::string_view name = {}) -> std::set<std::string> {
        auto directory = fs->readdir(path(name));
        EXPECT_TRUE(directory.opened) << "readdir: expected readable directory " << name;
        auto result = std::set<std::string>{};
        if (!directory.opened) {
            return result;
        }
        for (const auto& entry : directory.entries) {
            EXPECT_TRUE(result.insert(entry).second)
                << "readdir: expected each directory name once: " << entry;
        }
        EXPECT_EQ(directory.read_error, 0) << "readdir: expected clean end of directory";
        EXPECT_EQ(directory.close_error, 0) << "release: expected directory close";
        return result;
    }

    void expect_observation(observed_operation operation,
                            std::string_view name,
                            std::optional<liquid::digest> content = {},
                            bool denied = false) {
        auto lock = std::scoped_lock{observations_mutex};
        const auto found = std::ranges::any_of(observations, [&](const auto& request) {
            return request.operation == operation && request.path == name &&
                   request.pid == ::getpid() && request.denied == denied &&
                   (!content || (request.content && request.content->alg == content->alg &&
                                 request.content->bytes == content->bytes));
        });
        EXPECT_TRUE(found) << "observation: expected " << operation_name(operation)
                           << " path=" << name << " caller PID=" << ::getpid()
                           << " denied=" << denied << " and recorded content identity";
    }
};

TEST_F(VfsConformance, getattr_readdir_open_read_use_recorded_input_tree) {
    ASSERT_TRUE(block_on(repository.write("input.txt", bytes("new live repository bytes"))))
        << "read fixture: expected divergent live repository";
    ASSERT_TRUE(block_on(repository.write("live-only.txt", bytes("not in recorded tree"))))
        << "readdir fixture: expected live-only entry";
    {
        auto disk = std::ofstream{root / "repository" / "input.txt"};
        disk << "unrecorded physical checkout bytes";
        ASSERT_TRUE(disk) << "read fixture: expected divergent physical checkout";
    }
    mount(true, true);
    if (!handle) {
        return;
    }
    struct stat attributes{};
    ASSERT_EQ(fs->getattr(path("input.txt").c_str(), &attributes), 0)
        << "getattr: expected recorded input to exist: " << std::strerror(errno);
    EXPECT_EQ(attributes.st_size, static_cast<off_t>(recorded_input.size()))
        << "getattr: expected st_size to equal recorded input bytes";
    EXPECT_EQ(attributes.st_mode & S_IFMT, S_IFREG) << "getattr: expected regular file type";
    EXPECT_EQ(attributes.st_mode & 0777, 0644) << "getattr: expected recorded permissions";
    EXPECT_EQ(names(), (std::set<std::string>{".", "..", "deps", "input.txt", "nested"}))
        << "readdir: expected exactly recorded children, excluding live-only entries";
    EXPECT_EQ(names("nested"), (std::set<std::string>{".", "..", "child.txt"}))
        << "readdir: expected recorded nested children";
    ASSERT_NO_FATAL_FAILURE(expect_read("input.txt", recorded_input));
    ASSERT_NO_FATAL_FAILURE(expect_read("input.txt", "input", 9));
    ASSERT_NO_FATAL_FAILURE(expect_read("nested/child.txt", "child"));
    auto file = descriptor{fs, fs->open(path("input.txt").c_str(), O_RDONLY)};
    ASSERT_GE(file.get(), 0) << "open: expected input handle for EOF checks";
    auto byte = char{};
    EXPECT_EQ(fs->pread(file.get(), &byte, 1, recorded_input.size()), 0)
        << "read: expected zero bytes at EOF";
    EXPECT_EQ(fs->pread(file.get(), &byte, 1, recorded_input.size() + 4096), 0)
        << "read: expected zero bytes beyond EOF";
    EXPECT_EQ(fs->pread(file.get(), &byte, 0, 0), 0)
        << "read: expected zero-length read to succeed";
    auto missing = descriptor{fs, fs->open(path("missing").c_str(), O_RDONLY)};
    EXPECT_EQ(missing.get(), -1) << "open: expected missing path refusal";
    EXPECT_EQ(errno, ENOENT) << "open: expected ENOENT for missing path";
    EXPECT_EQ(fs->getattr(path("missing").c_str(), &attributes), -1)
        << "getattr: expected missing path refusal";
    EXPECT_EQ(errno, ENOENT) << "getattr: expected ENOENT for missing path";
    expect_observation(observed_operation::getattr, "input.txt", input_blob);
    expect_observation(observed_operation::readdir, "nested");
    expect_observation(observed_operation::open, "input.txt", input_blob);
    expect_observation(observed_operation::read, "input.txt", input_blob);
}

TEST_F(VfsConformance, create_write_release_capture_bytes_and_attribution) {
    mount(true);
    if (!handle) {
        return;
    }
    const auto before = repository.status()->pending_drips;
    auto file =
        descriptor{fs, fs->open(path("created.txt").c_str(), O_CREAT | O_EXCL | O_RDWR, 0644)};
    ASSERT_GE(file.get(), 0) << "create: expected mutable capture mount to accept new file";
    ASSERT_EQ(fs->pwrite(file.get(), "abcdef", 6, 0), 6) << "write: expected six bytes captured";
    ASSERT_EQ(fs->pwrite(file.get(), "XY", 2, 2), 2) << "write: expected offset overwrite captured";
    ASSERT_EQ(fs->pwrite(file.get(), "!", 1, 9), 1) << "write: expected extending write captured";
    ASSERT_EQ(file.close(), 0) << "release: expected successful close after writes";
    const auto expected = std::string{"abXYef\0\0\0!", 10};
    ASSERT_NO_FATAL_FAILURE(expect_read("created.txt", expected));
    auto persisted = block_on(repository.read("created.txt"));
    ASSERT_TRUE(persisted) << "release: expected repository content after close";
    EXPECT_EQ(*persisted, bytes(expected)) << "write: expected exact bytes and zero-filled gap";
    EXPECT_GT(repository.status()->pending_drips, before)
        << "write: expected captured writes to advance drip history";
    auto operations = block_on(repository.operations());
    ASSERT_TRUE(operations) << "write: expected operation history";
    ASSERT_FALSE(operations->empty()) << "write: expected attributed write operation";
    EXPECT_EQ(operations->front().kind, "write") << "write: expected write operation kind";
    EXPECT_EQ(operations->front().from, "created.txt")
        << "write: expected captured path attribution";
    EXPECT_EQ(operations->front().by.taap_id, author) << "write: expected repository TAAP author";
    auto root_tree = block_on(repository.root());
    ASSERT_TRUE(root_tree) << "write: expected updated repository tree";
    const auto entry =
        std::ranges::find(root_tree->entries, "created.txt", &liquid::tree_entry::name);
    ASSERT_NE(entry, root_tree->entries.end()) << "write: expected created entry in repository";
    EXPECT_NE(entry->node, liquid::node_id{}) << "write: expected nonempty stable node identity";
    EXPECT_EQ(liquid::repository_detail::testing::last_operation_node(repository), entry->node)
        << "write: expected drip attributed to the created stable node";
    auto verified = liquid::repository_detail::testing::verify_history(repository);
    ASSERT_TRUE(verified) << "write: expected valid signed history: " << verified.error().message;
    EXPECT_GT(*verified, 0U) << "write: expected signed history records";
    expect_observation(observed_operation::write, "created.txt");
    auto status = handle->status();
    ASSERT_TRUE(status) << "write: expected mount counters";
    EXPECT_GE(status->writes_captured, 4U) << "write: expected create and three writes counted";
    stop();
    mount();
    if (!handle) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(expect_read("created.txt", expected));
}

TEST_F(VfsConformance, truncate_chmod_and_open_trunc_preserve_file_type_and_bytes) {
    mount(true);
    if (!handle) {
        return;
    }
    ASSERT_EQ(fs->chmod(path("input.txt").c_str(), 0700), 0)
        << "chmod: expected mutable file permission change";
    ASSERT_EQ(fs->truncate(path("input.txt").c_str(), 8), 0)
        << "truncate: expected shrink to requested length";
    ASSERT_NO_FATAL_FAILURE(expect_read("input.txt", "recorded"));
    ASSERT_EQ(fs->truncate(path("input.txt").c_str(), 11), 0)
        << "truncate: expected extension to requested length";
    ASSERT_NO_FATAL_FAILURE(expect_read("input.txt", std::string{"recorded\0\0\0", 11}));
    struct stat attributes{};
    ASSERT_EQ(fs->getattr(path("input.txt").c_str(), &attributes), 0)
        << "getattr: expected attributes after truncate and chmod";
    EXPECT_EQ(attributes.st_size, 11) << "getattr: expected fresh size after truncate";
    EXPECT_EQ(attributes.st_mode & (S_IFMT | 0777), S_IFREG | 0700)
        << "chmod: expected regular-file type and requested permissions";
    auto file = descriptor{fs, fs->open(path("input.txt").c_str(), O_WRONLY | O_TRUNC)};
    ASSERT_GE(file.get(), 0) << "open/O_TRUNC: expected successful mutable open";
    ASSERT_EQ(file.close(), 0) << "release: expected close after O_TRUNC";
    auto entry = block_on(liquid::repository_detail::vfs_access::entry(repository, "input.txt"));
    ASSERT_TRUE(entry) << "truncate: expected persisted entry";
    EXPECT_EQ(entry->size, 0U) << "open/O_TRUNC: expected persisted empty content";
    EXPECT_EQ(entry->mode, 0100700U) << "open/O_TRUNC: expected executable mode retained";
    ASSERT_EQ(fs->getattr(path("input.txt").c_str(), &attributes), 0)
        << "getattr: expected attributes after O_TRUNC";
    EXPECT_EQ(attributes.st_size, 0) << "getattr: expected zero size after O_TRUNC";
}

TEST_F(VfsConformance, rename_unlink_update_names_bytes_and_stable_identity) {
    mount(true);
    if (!handle) {
        return;
    }
    auto original = block_on(liquid::repository_detail::vfs_access::entry(repository, "input.txt"));
    ASSERT_TRUE(original) << "rename fixture: expected source stable node";
    const auto before = repository.status()->pending_drips;
    ASSERT_EQ(fs->rename(path("input.txt").c_str(), path("moved.txt").c_str()), 0)
        << "rename: expected source to move";
    EXPECT_EQ(repository.status()->pending_drips, before + 1)
        << "rename: expected one recorded move to absent destination";
    auto moved = block_on(liquid::repository_detail::vfs_access::entry(repository, "moved.txt"));
    ASSERT_TRUE(moved) << "rename: expected destination in repository";
    EXPECT_EQ(moved->node, original->node) << "rename: expected stable node identity retained";
    ASSERT_NO_FATAL_FAILURE(expect_read("moved.txt", recorded_input));
    EXPECT_EQ(names(), (std::set<std::string>{".", "..", "deps", "moved.txt", "nested"}))
        << "rename/readdir: expected destination present and source absent";
    ASSERT_EQ(fs->rename(path("moved.txt").c_str(), path("nested/child.txt").c_str()), 0)
        << "rename: expected replacement of existing file";
    ASSERT_NO_FATAL_FAILURE(expect_read("nested/child.txt", recorded_input));
    ASSERT_EQ(fs->unlink(path("nested/child.txt").c_str()), 0)
        << "unlink: expected removal of renamed file";
    EXPECT_EQ(names("nested"), (std::set<std::string>{".", ".."}))
        << "unlink/readdir: expected removed entry absent";
    struct stat attributes{};
    EXPECT_EQ(fs->getattr(path("nested/child.txt").c_str(), &attributes), -1)
        << "unlink/getattr: expected removed path absent";
    EXPECT_EQ(errno, ENOENT) << "unlink/getattr: expected ENOENT";
    auto missing = block_on(repository.read("nested/child.txt"));
    ASSERT_FALSE(missing) << "unlink: expected removal persisted in repository";
    EXPECT_EQ(missing.error().code, liquid::errc::not_found)
        << "unlink: expected repository not_found";
}

TEST_F(VfsConformance, synthetic_mtimes_distinguish_sources_outputs_and_mutations) {
    ASSERT_TRUE(block_on(repository.write("output.bin", bytes("old output"))))
        << "getattr/mtime fixture: expected declared output";
    auto tree = block_on(repository.root_digest());
    ASSERT_TRUE(tree) << "getattr/mtime fixture: expected input tree with output";
    input_tree = *tree;
    mount(true, false, {"output.bin"});
    if (!handle) {
        return;
    }
    struct stat source{};
    struct stat output{};
    ASSERT_EQ(fs->getattr(path("input.txt").c_str(), &source), 0)
        << "getattr/mtime: expected source attributes";
    ASSERT_EQ(fs->getattr(path("output.bin").c_str(), &output), 0)
        << "getattr/mtime: expected output attributes";
    EXPECT_EQ(source.st_mtime, 1'000'000'000) << "getattr/mtime: expected synthetic source epoch";
    EXPECT_EQ(output.st_mtime, 2'000'000'000) << "getattr/mtime: expected synthetic output epoch";
    ASSERT_EQ(fs->truncate(path("output.bin").c_str(), 2), 0)
        << "truncate/mtime: expected output mutation";
    struct stat changed{};
    ASSERT_EQ(fs->getattr(path("output.bin").c_str(), &changed), 0)
        << "getattr/mtime: expected fresh output attributes";
    EXPECT_GT(changed.st_mtime, output.st_mtime)
        << "getattr/mtime: expected mutation after output epoch";
    ASSERT_EQ(fs->chmod(path("output.bin").c_str(), 0600), 0)
        << "chmod/mtime: expected second output mutation";
    ASSERT_EQ(fs->getattr(path("output.bin").c_str(), &output), 0)
        << "getattr/mtime: expected second fresh output attributes";
    EXPECT_GT(output.st_mtime, changed.st_mtime)
        << "getattr/mtime: expected monotonic mutation time";
    ASSERT_EQ(fs->getattr(path("input.txt").c_str(), &source), 0)
        << "getattr/mtime: expected unchanged source attributes";
    EXPECT_EQ(source.st_mtime, 1'000'000'000) << "getattr/mtime: expected stable source epoch";
}

TEST_F(VfsConformance, readonly_observed_mount_refuses_mutations_without_capture) {
    mount(true, true);
    if (!handle) {
        return;
    }
    const auto before = repository.status()->pending_drips;
    for (const auto flags : {O_WRONLY, O_RDWR, O_WRONLY | O_TRUNC}) {
        auto file = descriptor{fs, fs->open(path("input.txt").c_str(), flags)};
        EXPECT_EQ(file.get(), -1) << "open/write: expected read-only mount refusal, flags="
                                  << flags;
        EXPECT_EQ(errno, EROFS) << "open/write: expected EROFS";
    }
    auto created = descriptor{fs, fs->open(path("refused.txt").c_str(), O_CREAT | O_WRONLY, 0644)};
    EXPECT_EQ(created.get(), -1) << "create: expected read-only mount refusal";
    EXPECT_EQ(errno, EROFS) << "create: expected EROFS";
    EXPECT_EQ(fs->truncate(path("input.txt").c_str(), 0), -1)
        << "truncate: expected read-only mount refusal";
    EXPECT_EQ(errno, EROFS) << "truncate: expected EROFS";
    EXPECT_EQ(fs->chmod(path("input.txt").c_str(), 0600), -1)
        << "chmod: expected read-only mount refusal";
    EXPECT_EQ(errno, EROFS) << "chmod: expected EROFS";
    EXPECT_EQ(fs->rename(path("input.txt").c_str(), path("renamed.txt").c_str()), -1)
        << "rename: expected read-only mount refusal";
    EXPECT_EQ(errno, EROFS) << "rename: expected EROFS";
    EXPECT_EQ(fs->unlink(path("input.txt").c_str()), -1)
        << "unlink: expected read-only mount refusal";
    EXPECT_EQ(errno, EROFS) << "unlink: expected EROFS";
    EXPECT_EQ(repository.status()->pending_drips, before)
        << "write/capture: expected refused operations to leave drip history unchanged";
    ASSERT_NO_FATAL_FAILURE(expect_read("input.txt", recorded_input));
    auto status = handle->status();
    ASSERT_TRUE(status) << "write/capture: expected mount counters";
    EXPECT_EQ(status->writes_captured, 0U) << "write/capture: expected zero successful writes";
    expect_observation(observed_operation::write, "input.txt", {}, true);
}

TEST_F(VfsConformance, mutable_mount_preserves_readonly_dependency_inputs) {
    mount(true);
    if (!handle) {
        return;
    }
    auto file = descriptor{fs, fs->open(path("deps/library.txt").c_str(), O_WRONLY)};
    EXPECT_EQ(file.get(), -1) << "open/write: expected reserved dependency subtree refusal";
    EXPECT_EQ(errno, EROFS) << "open/write: expected EROFS for dependency";
    EXPECT_EQ(fs->rename(path("input.txt").c_str(), path("deps/library.txt").c_str()), -1)
        << "rename: expected dependency destination refusal";
    EXPECT_EQ(errno, EROFS) << "rename: expected EROFS for dependency destination";
    ASSERT_NO_FATAL_FAILURE(expect_read("deps/library.txt", "dependency"));
    ASSERT_NO_FATAL_FAILURE(expect_read("input.txt", recorded_input));
}

TEST_F(VfsConformance, xattrs_get_list_set_remove_and_report_invalid_requests) {
    mount();
    if (!handle) {
        return;
    }
    const auto input = path("input.txt");
    constexpr auto pin = "user.liquid.pin";
    constexpr auto state = "user.liquid.state";
    EXPECT_EQ(fs->get_attribute(input, pin, nullptr, 0), 1)
        << "getxattr: expected size query for one-byte pin value";
    auto value = std::array<char, 64>{};
    ASSERT_EQ(fs->get_attribute(input, pin, value.data(), value.size()), 1)
        << "getxattr: expected default pin value";
    EXPECT_EQ(value[0], '0') << "getxattr: expected initially unpinned file";
    const auto count = fs->list_attributes(input, nullptr, 0);
    ASSERT_GT(count, 0) << "listxattr: expected nonempty size query";
    auto listed = std::string(static_cast<std::size_t>(count), '\0');
    ASSERT_EQ(fs->list_attributes(input, listed.data(), listed.size()), count)
        << "listxattr: expected complete NUL-separated list";
    ASSERT_EQ(listed.back(), '\0') << "listxattr: expected NUL-terminated attribute names";
    auto attribute_names = std::set<std::string>{};
    for (auto offset = std::size_t{}; offset < listed.size();) {
        const auto end = listed.find('\0', offset);
        ASSERT_NE(end, std::string::npos) << "listxattr: expected each name terminated";
        EXPECT_GT(end, offset) << "listxattr: expected nonempty attribute names";
        EXPECT_TRUE(attribute_names.insert(listed.substr(offset, end - offset)).second)
            << "listxattr: expected each attribute once";
        offset = end + 1;
    }
    EXPECT_EQ(attribute_names, (std::set<std::string>{pin, state}))
        << "listxattr: expected both documented drive attributes in any order";
    EXPECT_EQ(fs->list_attributes(input, value.data(), 1), -1)
        << "listxattr: expected short buffer refusal";
    EXPECT_EQ(errno, ERANGE) << "listxattr: expected ERANGE for short buffer";
    ASSERT_EQ(fs->set_attribute(input, pin, "1"), 0) << "setxattr: expected pin accepted";
    ASSERT_EQ(fs->get_attribute(input, pin, value.data(), value.size()), 1)
        << "getxattr: expected updated pin";
    EXPECT_EQ(value[0], '1') << "setxattr/getxattr: expected pinned value";
    constexpr auto offline = std::string_view{"available-offline"};
    ASSERT_EQ(fs->get_attribute(input, state, value.data(), value.size()), offline.size())
        << "getxattr: expected pinned state length";
    EXPECT_EQ(std::string_view(value.data(), offline.size()), offline)
        << "setxattr/getxattr: expected available-offline state";
    EXPECT_EQ(fs->get_attribute(input, state, value.data(), 1), -1)
        << "getxattr: expected short buffer refusal";
    EXPECT_EQ(errno, ERANGE) << "getxattr: expected ERANGE for short buffer";
    EXPECT_EQ(fs->set_attribute(input, pin, "invalid"), -1)
        << "setxattr: expected invalid pin value refusal";
    EXPECT_EQ(errno, EINVAL) << "setxattr: expected EINVAL for invalid pin";
    EXPECT_EQ(fs->get_attribute(input, "user.unknown", value.data(), value.size()), -1)
        << "getxattr: expected unknown attribute refusal";
#if defined(__APPLE__)
    EXPECT_EQ(errno, ENOATTR) << "getxattr: expected ENOATTR for unknown name";
#else
    EXPECT_EQ(errno, ENODATA) << "getxattr: expected ENODATA for unknown name";
#endif
    ASSERT_EQ(fs->remove_attribute(input, pin), 0) << "removexattr: expected pin reset";
    ASSERT_EQ(fs->get_attribute(input, pin, value.data(), value.size()), 1)
        << "getxattr: expected pin after reset";
    EXPECT_EQ(value[0], '0') << "removexattr: expected reset to online-only";
    ASSERT_EQ(fs->set_attribute(input, state, "available-offline"), 0)
        << "setxattr: expected state setter accepted";
    ASSERT_EQ(fs->remove_attribute(input, state), 0) << "removexattr: expected state reset";
    stop();
    mount();
    if (!handle) {
        return;
    }
    ASSERT_EQ(fs->get_attribute(input, pin, value.data(), value.size()), 1)
        << "getxattr: expected persisted pin reset after remount";
    EXPECT_EQ(value[0], '0') << "removexattr: expected reset persisted";
}

TEST_F(VfsConformance, read_hydrates_missing_range_through_queue_and_persistent_cache) {
    ASSERT_NO_FATAL_FAILURE(remote->initialise());
    ASSERT_TRUE(block_on(repository.write_remote("remote.bin", remote->record)))
        << "read/hydration fixture: expected registered remote blob";
    // Seed only the first logical chunk in the store; the requested distant range is absent.
    auto first = std::vector<std::byte>(remote_source::chunk_size);
    auto seeded = block_on(liquid::repository_detail::vfs_access::read_chunk(
        repository, remote->record.content, 0, first));
    ASSERT_TRUE(seeded) << "read/hydration fixture: expected first chunk locally present";
    ASSERT_EQ(remote->transferred.load(), remote_source::chunk_size)
        << "read/hydration fixture: expected only first chunk seeded";
    const auto before = repository.status();
    ASSERT_TRUE(before) << "read/hydration fixture: expected remote-byte status";
    EXPECT_GT(before->remote_bytes, 0U) << "read/hydration fixture: expected partially absent blob";
    EXPECT_LT(before->remote_bytes, remote->content.size())
        << "read/hydration fixture: expected partially present blob";
    const auto seeded_calls = remote->calls.load();
    mount();
    if (!handle) {
        return;
    }
    constexpr auto offset = 5 * remote_source::chunk_size + 4096;
    const auto expected =
        std::string_view{reinterpret_cast<const char*>(remote->content.data() + offset), 4096};
    constexpr auto second_offset = 6 * remote_source::chunk_size + 4096;
    auto concurrent = block_on(read_concurrently(fs, path("remote.bin"), offset, second_offset));
    ASSERT_TRUE(concurrent) << "read/queue: expected both concurrent ranges: "
                            << concurrent.error().message;
    EXPECT_EQ((*concurrent)[0], expected) << "read: expected first concurrent range bytes";
    EXPECT_EQ((*concurrent)[1],
              (std::string_view{
                  reinterpret_cast<const char*>(remote->content.data() + second_offset), 4096}))
        << "read: expected second concurrent range bytes";
    auto status = handle->status();
    ASSERT_TRUE(status) << "read/hydration: expected mount status";
    EXPECT_GT(remote->calls.load(), seeded_calls)
        << "read/hydration: expected missing range fetched";
    EXPECT_GT(status->misses_served, 0U) << "read/hydration: expected persistent cache miss";
    EXPECT_GT(status->hydrated_bytes, 0U) << "read/hydration: expected hydrated chunk bytes";
    EXPECT_LT(status->hydrated_bytes, remote->content.size())
        << "read/hydration: expected partial range hydration, not full download";
    EXPECT_EQ(status->hydrated_files, 1U) << "read/hydration: expected one hydrated file";
    EXPECT_GT(status->request_queue_latency.count(), 0)
        << "read/queue: expected completed serialized requests";
    EXPECT_EQ(remote->maximum_active.load(), 1U) << "read/queue: expected serialized remote reads";
    stop();
    const auto fetched_calls = remote->calls.load();
    remote->online = false;
    mount();
    if (!handle) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(expect_read("remote.bin", expected, offset));
    auto cached = handle->status();
    ASSERT_TRUE(cached) << "read/hydration: expected remount counters";
    EXPECT_EQ(cached->misses_served, 0U)
        << "read/hydration: expected persistent cache hit after remount";
    EXPECT_EQ(remote->calls.load(), fetched_calls)
        << "read/hydration: expected cached range without remote access";
}
#else
TEST(VfsConformance, mounted_operations_require_platform_driver) {
    if (allow_unavailable) {
        GTEST_SKIP() << "mount: POSIX syscall driver unavailable on Windows";
    }
    FAIL() << "mount: expected a Windows operation driver for the selected backend";
}
#endif

} // namespace

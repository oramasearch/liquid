#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"
#include "node_store/service.hpp"
#include "serve/http.hpp"
#include "store/chunker.hpp"

namespace {

class temporary_node_store {
public:
    temporary_node_store() {
        path_ = std::filesystem::temp_directory_path() /
                ("liquid-node-store-" + std::to_string(++sequence_));
        std::filesystem::remove_all(path_);
    }

    ~temporary_node_store() {
        std::filesystem::remove_all(path_);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    static inline std::uint64_t sequence_{};
    std::filesystem::path path_;
};

class server_source final : public liquid::remote_blob_source {
public:
    explicit server_source(liquid::sync_server& server) : server_(server) {}

    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string, std::vector<liquid::digest>) override {
        co_return std::vector<liquid::digest>{};
    }

    liquid::runtime::task<std::uint64_t> read_range(std::string,
                                                    liquid::digest content,
                                                    std::uint64_t offset,
                                                    std::span<std::byte> output) override {
        co_return co_await server_.handle_chunks(content, offset, output);
    }

    liquid::runtime::task<void>
    write(std::string, liquid::digest, std::vector<std::byte>) override {
        co_return std::unexpected(liquid::error{
            liquid::errc::unsupported, "test source is read-only", "node-store test"});
    }

private:
    liquid::sync_server& server_;
};

class unavailable_source final : public liquid::remote_blob_source {
public:
    explicit unavailable_source(std::string context) : context_(std::move(context)) {}

    liquid::runtime::task<std::vector<liquid::digest>>
    find_missing(std::string, std::vector<liquid::digest>) override {
        co_return std::unexpected(
            liquid::error{liquid::errc::unavailable, "test image store is offline", context_});
    }

    liquid::runtime::task<std::uint64_t>
    read_range(std::string, liquid::digest, std::uint64_t, std::span<std::byte>) override {
        co_return std::unexpected(
            liquid::error{liquid::errc::unavailable, "test image store is offline", context_});
    }

    liquid::runtime::task<void>
    write(std::string, liquid::digest, std::vector<std::byte>) override {
        co_return std::unexpected(
            liquid::error{liquid::errc::unavailable, "test image store is offline", context_});
    }

private:
    std::string context_;
};

auto image_bytes(std::uint64_t seed) -> std::vector<std::byte> {
    auto value = seed + 0x9e3779b97f4a7c15ULL;
    auto output = std::vector<std::byte>(4U * 1024U * 1024U);
    for (auto& byte : output) {
        value ^= value << 13U;
        value ^= value >> 7U;
        value ^= value << 17U;
        byte = std::byte{static_cast<unsigned char>(value)};
    }
    return output;
}

auto remote_blob(std::span<const std::byte> bytes) -> liquid::blob {
    auto output = liquid::blob{};
    output.size = bytes.size();
    output.content = *liquid::hash(liquid::algorithm::blake3, bytes);
    output.remote = liquid::remote_blob_location{"memory://images", "memory://images"};
    for (const auto range : liquid::store_detail::fastcdc(bytes)) {
        const auto chunk = bytes.subspan(range.offset, range.size);
        output.chunks.push_back(*liquid::hash(liquid::algorithm::blake3, chunk));
        output.chunk_sizes.push_back(range.size);
    }
    return output;
}

auto read_file(const std::filesystem::path& path, std::size_t size) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    auto output = std::vector<std::byte>(size);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    return input && input.peek() == std::ifstream::traits_type::eof() ? output
                                                                      : std::vector<std::byte>{};
}

#if !defined(_WIN32)
auto read_error(const std::filesystem::path& path) -> int {
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return errno;
    }
    auto byte = std::byte{};
    const auto count = ::read(descriptor, &byte, 1U);
    const auto result = count < 0 ? errno : 0;
    ::close(descriptor);
    return result;
}
#endif

TEST(NodeStore, ControlSurfacesReportStatusAndDeclaredTools) {
    temporary_node_store location;
    auto opened = liquid::runtime::block_on(
        liquid::node_store_detail::service::open({.root = location.path(), .budget_bytes = 4096}));
    ASSERT_TRUE(opened) << opened.error().message;
    auto service = std::move(*opened);
    auto routes = service->control_routes();
    const auto subject = *liquid::hash(liquid::algorithm::blake3, std::span<const std::byte>{});
    auto stored_attestation = liquid::runtime::block_on(
        service->store_local_attestation({.subject = subject,
                                          .profile = "scan_result/1",
                                          .detectors = {"test"},
                                          .at = std::chrono::sys_seconds{std::chrono::seconds{1}},
                                          .issuer = liquid::actor{"test:node-store"},
                                          .subject_kind = liquid::attestation_subject_kind::drip}));
    ASSERT_TRUE(stored_attestation) << stored_attestation.error().message;

    auto readiness = liquid::runtime::block_on(
        routes->dispatch({.method = "GET", .target = "/readyz", .path = "/readyz"}));
    ASSERT_TRUE(readiness) << readiness.error().message;
    EXPECT_EQ(readiness->status, 503U);
    service->socket_bound();
    readiness = liquid::runtime::block_on(
        routes->dispatch({.method = "GET", .target = "/readyz", .path = "/readyz"}));
    ASSERT_TRUE(readiness) << readiness.error().message;
    EXPECT_EQ(readiness->status, 200U);

    auto status = liquid::runtime::block_on(
        routes->dispatch({.method = "GET", .target = "/v1", .path = "/v1"}));
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_NE(status->body.find("\"budget_bytes\": 4096"), std::string::npos);
    EXPECT_NE(status->body.find("\"mounts\": []"), std::string::npos);

    auto metrics = liquid::runtime::block_on(
        routes->dispatch({.method = "GET", .target = "/metrics", .path = "/metrics"}));
    ASSERT_TRUE(metrics) << metrics.error().message;
    EXPECT_NE(metrics->body.find("liquid_node_store_store_bytes "), std::string::npos);
    EXPECT_NE(metrics->body.find("liquid_node_store_budget_bytes 4096"), std::string::npos);
    EXPECT_NE(metrics->body.find("liquid_node_store_evictions_total 0"), std::string::npos);

    auto openapi = liquid::runtime::block_on(routes->dispatch(
        {.method = "GET", .target = "/v1/openapi.json", .path = "/v1/openapi.json"}));
    ASSERT_TRUE(openapi) << openapi.error().message;
    EXPECT_NE(openapi->body.find("/v1/attestations"), std::string::npos);
    EXPECT_EQ(openapi->body.find("\"/mount\""), std::string::npos);

    auto attestations = liquid::runtime::block_on(
        routes->dispatch({.method = "GET",
                          .target = "/v1/attestations?subject=" + liquid::to_string(subject),
                          .path = "/v1/attestations",
                          .query = "subject=" + liquid::to_string(subject)}));
    ASSERT_TRUE(attestations) << attestations.error().message;
    EXPECT_NE(attestations->body.find(liquid::to_string(*stored_attestation)), std::string::npos);
    EXPECT_NE(attestations->body.find("canonical_bytes_base64"), std::string::npos);

    auto tools = liquid::runtime::block_on(
        routes->dispatch({.method = "POST",
                          .target = "/mcp",
                          .path = "/mcp",
                          .body = R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})"}));
    ASSERT_TRUE(tools) << tools.error().message;
    EXPECT_NE(tools->body.find("node_store_status"), std::string::npos);
    EXPECT_NE(tools->body.find("node_store_mounts"), std::string::npos);
    EXPECT_NE(tools->body.find("node_store_evict"), std::string::npos);
}

TEST(NodeStore, UnixStatusUsesTheLocalRouteTable) {
    temporary_node_store location;
    auto opened = liquid::runtime::block_on(
        liquid::node_store_detail::service::open({.root = location.path()}));
    ASSERT_TRUE(opened) << opened.error().message;
    auto routes = (*opened)->unix_routes();

    auto status = liquid::runtime::block_on(
        routes->dispatch({.method = "GET", .target = "/status", .path = "/status"}));

    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->status, 200U);
    EXPECT_NE(status->body.find("\"store_bytes\": 0"), std::string::npos);
}

TEST(NodeStore, AbsentTreeReportsItsUnavailableStoreUrl) {
    temporary_node_store location;
    auto opened = liquid::runtime::block_on(
        liquid::node_store_detail::service::open({.root = location.path()}));
    ASSERT_TRUE(opened) << opened.error().message;
    auto service = std::move(*opened);
    const auto url = std::string{"https://offline.example/images"};
    auto source = std::make_shared<unavailable_source>(url);
    ASSERT_TRUE(liquid::runtime::block_on(service->register_source(url, source)));
    const auto tree = *liquid::hash(liquid::algorithm::blake3, std::span<const std::byte>{});

    auto mounted = liquid::runtime::block_on(service->mount_image({tree, url, {}}));

    ASSERT_FALSE(mounted);
    EXPECT_EQ(mounted.error().code, liquid::errc::unavailable);
    EXPECT_EQ(mounted.error().context, url);
    auto status = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(status) << status.error().message;
    ASSERT_TRUE(status->last_error);
    EXPECT_NE(status->last_error->find(url), std::string::npos);
}

TEST(NodeStore, LocalMetadataMountsWhenItsContentStoreIsUnavailable) {
#if defined(_WIN32)
    GTEST_SKIP() << "node-store image mounts require Unix FUSE";
#else
    temporary_node_store location;
    auto opened_source_store =
        liquid::runtime::block_on(liquid::store::open(location.path() / "source-store"));
    ASSERT_TRUE(opened_source_store) << opened_source_store.error().message;
    auto source_store = std::move(*opened_source_store);
    auto source_repository = liquid::runtime::block_on(
        liquid::repository::create(location.path() / "source", {.store = &source_store}));
    ASSERT_TRUE(source_repository) << source_repository.error().message;
    const auto expected = image_bytes(41U);
    ASSERT_TRUE(liquid::runtime::block_on(source_store.put(expected)));
    ASSERT_TRUE(liquid::runtime::block_on(
        source_repository->write_remote("image.bin", remote_blob(expected))));
    ASSERT_TRUE(liquid::runtime::block_on(source_repository->cut("offline image")));
    auto tree = liquid::runtime::block_on(source_repository->root_digest());
    ASSERT_TRUE(tree) << tree.error().message;

    auto server = liquid::sync_server{*source_repository};
    auto opened = liquid::runtime::block_on(
        liquid::node_store_detail::service::open({.root = location.path() / "node"}));
    ASSERT_TRUE(opened) << opened.error().message;
    auto service = std::move(*opened);
    const auto url = std::string{"memory://offline-image"};
    ASSERT_TRUE(liquid::runtime::block_on(
        service->register_source(url, std::make_shared<server_source>(server))));
    auto first = liquid::runtime::block_on(service->mount_image({*tree, url, {}}));
    if (!first && first.error().code == liquid::errc::unsupported) {
        GTEST_SKIP() << "FUSE unavailable: " << first.error().message;
    }
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_TRUE(liquid::runtime::block_on(service->unmount_image({.tree = *tree})));

    ASSERT_TRUE(liquid::runtime::block_on(
        service->register_source(url, std::make_shared<unavailable_source>(url))));
    auto second = liquid::runtime::block_on(service->mount_image({*tree, url, {}}));
    ASSERT_TRUE(second) << second.error().message << ": " << second.error().context;
    EXPECT_EQ(read_error(second->path / "image.bin"), EIO);
    auto status = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(status) << status.error().message;
    ASSERT_TRUE(status->last_error);
    EXPECT_NE(status->last_error->find(url), std::string::npos);
    ASSERT_TRUE(liquid::runtime::block_on(service->unmount_image({.tree = *tree})));
#endif
}

TEST(NodeStore, ReusesChunksAcrossTwoImagePullsAndPrefetchesEvictedContent) {
#if defined(_WIN32)
    GTEST_SKIP() << "node-store image mounts require Unix FUSE";
#else
    temporary_node_store location;
    auto opened_source_store =
        liquid::runtime::block_on(liquid::store::open(location.path() / "source-store"));
    ASSERT_TRUE(opened_source_store) << opened_source_store.error().message;
    auto source_store = std::move(*opened_source_store);
    auto source_repository = liquid::runtime::block_on(
        liquid::repository::create(location.path() / "source", {.store = &source_store}));
    ASSERT_TRUE(source_repository) << source_repository.error().message;
    auto expected = std::vector<std::vector<std::byte>>{};
    for (auto index = std::uint64_t{}; index < 16U; ++index) {
        expected.push_back(image_bytes(index));
        ASSERT_TRUE(liquid::runtime::block_on(source_store.put(expected.back())));
        const auto written = liquid::runtime::block_on(source_repository->write_remote(
            std::format("file-{:02}.bin", index), remote_blob(expected.back())));
        ASSERT_TRUE(written) << written.error().message;
    }
    ASSERT_TRUE(liquid::runtime::block_on(source_repository->cut("image A")));
    auto tree_a = liquid::runtime::block_on(source_repository->root_digest());
    ASSERT_TRUE(tree_a) << tree_a.error().message;

    auto changed = image_bytes(1007U);
    ASSERT_TRUE(liquid::runtime::block_on(source_store.put(changed)));
    ASSERT_TRUE(liquid::runtime::block_on(
        source_repository->write_remote("file-07.bin", remote_blob(changed))));
    ASSERT_TRUE(liquid::runtime::block_on(source_repository->cut("image B")));
    auto tree_b = liquid::runtime::block_on(source_repository->root_digest());
    ASSERT_TRUE(tree_b) << tree_b.error().message;

    auto server = liquid::sync_server{*source_repository};
    auto source = std::make_shared<server_source>(server);
    auto opened = liquid::runtime::block_on(
        liquid::node_store_detail::service::open({.root = location.path() / "node"}));
    ASSERT_TRUE(opened) << opened.error().message;
    auto service = std::move(*opened);
    ASSERT_TRUE(liquid::runtime::block_on(service->register_source("memory://images", source)));

    auto first = liquid::runtime::block_on(service->mount_image({*tree_a, "memory://images", {}}));
    if (!first && first.error().code == liquid::errc::unsupported) {
        GTEST_SKIP() << "FUSE unavailable: " << first.error().message;
    }
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    for (auto index = std::uint64_t{}; index < expected.size(); ++index) {
        const auto path = first->path / std::format("file-{:02}.bin", index);
        EXPECT_EQ(read_error(path), 0) << path;
        EXPECT_EQ(read_file(path, expected[index].size()), expected[index]);
    }
    auto after_a = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(after_a) << after_a.error().message;
    ASSERT_FALSE(after_a->last_error) << *after_a->last_error;
    ASSERT_EQ(after_a->mounts.size(), 1U);
    EXPECT_GE(after_a->mounts.front().bytes_fetched, 64U * 1024U * 1024U);

    const auto first_content = liquid::hash(liquid::algorithm::blake3, expected.front());
    ASSERT_TRUE(first_content) << first_content.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(service->evict(*first_content)));
    const auto chunks_before_prefetch = after_a->mounts.front().chunks_fetched;
    ASSERT_TRUE(liquid::runtime::block_on(service->prefetch({*tree_a})));
    auto after_prefetch = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(after_prefetch) << after_prefetch.error().message;
    EXPECT_GT(after_prefetch->mounts.front().chunks_fetched, chunks_before_prefetch);

    auto second = liquid::runtime::block_on(service->mount_image({*tree_b, "memory://images", {}}));
    ASSERT_TRUE(second) << second.error().message << ": " << second.error().context;
    for (auto index = std::uint64_t{}; index < expected.size(); ++index) {
        const auto& wanted = index == 7U ? changed : expected[index];
        EXPECT_EQ(read_file(second->path / std::format("file-{:02}.bin", index), wanted.size()),
                  wanted);
    }
    auto after_b = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(after_b) << after_b.error().message;
    ASSERT_EQ(after_b->mounts.size(), 2U);
    const auto b = std::ranges::find_if(
        after_b->mounts, [&](const auto& mounted) { return mounted.path == second->path; });
    ASSERT_NE(b, after_b->mounts.end());
    EXPECT_LE(b->bytes_fetched, changed.size() + 128U * 1024U);
    EXPECT_GE(b->bytes_avoided, 60U * 1024U * 1024U);
    auto metrics = liquid::runtime::block_on(service->control_routes()->dispatch(
        {.method = "GET", .target = "/metrics", .path = "/metrics"}));
    ASSERT_TRUE(metrics) << metrics.error().message;
    EXPECT_NE(metrics->body.find("liquid_node_store_chunks_fetched_total{tree=\"" +
                                 liquid::to_string(*tree_b) + "\"}"),
              std::string::npos);
    EXPECT_NE(metrics->body.find("liquid_node_store_bytes_fetched_total{tree=\"" +
                                 liquid::to_string(*tree_b) + "\"}"),
              std::string::npos);
    EXPECT_NE(metrics->body.find("liquid_node_store_bytes_avoided_total{tree=\"" +
                                 liquid::to_string(*tree_b) + "\"}"),
              std::string::npos);
    std::println("node-store pull A chunks={} bytes={} pull B chunks={} bytes={} avoided={}",
                 after_a->mounts.front().chunks_fetched,
                 after_a->mounts.front().bytes_fetched,
                 b->chunks_fetched,
                 b->bytes_fetched,
                 b->bytes_avoided);

    auto repeated =
        liquid::runtime::block_on(service->mount_image({*tree_b, "https://ignored.invalid", {}}));
    ASSERT_TRUE(repeated) << repeated.error().message;
    EXPECT_EQ(repeated->path, second->path);
    auto repeated_status = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(repeated_status) << repeated_status.error().message;
    const auto repeated_b = std::ranges::find_if(
        repeated_status->mounts, [&](const auto& mounted) { return mounted.path == second->path; });
    ASSERT_NE(repeated_b, repeated_status->mounts.end());
    EXPECT_EQ(repeated_b->references, 2U);

    const auto non_remote = liquid::runtime::block_on(service->evict(*tree_b));
    ASSERT_FALSE(non_remote);
    EXPECT_EQ(non_remote.error().code, liquid::errc::not_remote);
    ASSERT_TRUE(liquid::runtime::block_on(service->unmount_image({.tree = *tree_b})));
    ASSERT_TRUE(liquid::runtime::block_on(service->unmount_image({.path = second->path})));
    ASSERT_TRUE(liquid::runtime::block_on(service->unmount_image({.tree = *tree_a})));
    ASSERT_TRUE(liquid::runtime::block_on(service->set_budget(16U * 1024U * 1024U)));
    auto budgeted = liquid::runtime::block_on(service->status());
    ASSERT_TRUE(budgeted) << budgeted.error().message;
    EXPECT_LE(budgeted->store_bytes, budgeted->budget_bytes);
    EXPECT_TRUE(budgeted->mounts.empty());
#endif
}

} // namespace

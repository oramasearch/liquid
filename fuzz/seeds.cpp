// Writes the seed corpus for every harness in this directory (liquid #407).
//
//   liquid-fuzz-seeds <source directory> <corpus directory>
//
// Each harness gets <corpus directory>/<harness>/. Seeds come from two places: the repository's
// existing test fixtures (the RFC 8949 CBOR vectors, the record fixtures, the `.liquid` package
// files used by the import tests), and records built with liquid's own encoders so that each
// decoder starts from inputs it accepts. Inputs carry the selector byte and length prefixes that
// fuzz.hpp describes. A real identity chain is made with a throwaway LIQUID_HOME.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "encoding/cbor.hpp"
#include "git/sha1.hpp"
#include "http2/frame.hpp"
#include "http2/grpc.hpp"
#include "http2/hpack.hpp"
#include "identity/taap.hpp"
#include "liquid/configuration.hpp"
#include "liquid/identity.hpp"
#include "liquid/resolve.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"
#include "reapi/messages.hpp"
#include "sync/codec.hpp"

namespace cbor = liquid::encoding::cbor;
namespace fs = std::filesystem;

namespace {

using bytes = std::vector<std::byte>;

auto to_bytes(std::string_view text) -> bytes {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto from_hex(std::string_view hex) -> bytes {
    auto output = bytes{};
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        output.push_back(
            static_cast<std::byte>(std::stoi(std::string{hex.substr(index, 2)}, nullptr, 16)));
    }
    return output;
}

auto read_file(const fs::path& path) -> bytes {
    auto input = std::ifstream{path, std::ios::binary};
    const auto text = std::string{std::istreambuf_iterator<char>{input}, {}};
    return to_bytes(text);
}

auto selected(std::uint8_t selector, const bytes& payload) -> bytes {
    auto output = bytes{static_cast<std::byte>(selector)};
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

void append_part(bytes& output, const bytes& part) {
    output.push_back(static_cast<std::byte>((part.size() >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(part.size() & 0xffU));
    output.insert(output.end(), part.begin(), part.end());
}

class corpus {
public:
    explicit corpus(fs::path root) : root_(std::move(root)) {}

    void add(std::string_view harness, std::string_view name, const bytes& input) {
        const auto directory = root_ / harness;
        fs::create_directories(directory);
        auto output = std::ofstream{directory / name, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(input.data()),
                     static_cast<std::streamsize>(input.size()));
        ++written_;
    }

    void add(std::string_view harness,
             std::string_view name,
             const liquid::result<bytes>& input,
             std::uint8_t selector) {
        if (!input) {
            std::cerr << "seed " << harness << "/" << name << " skipped: " << input.error().message
                      << "\n";
            return;
        }
        add(harness, name, selected(selector, *input));
    }

    [[nodiscard]] auto written() const -> std::size_t {
        return written_;
    }

private:
    fs::path root_;
    std::size_t written_{};
};

auto sample_digest(std::uint8_t fill) -> liquid::digest {
    auto value = liquid::digest{liquid::algorithm::blake3, {}};
    value.bytes.fill(static_cast<std::byte>(fill));
    return value;
}

void cbor_seeds(corpus& output, const fs::path& source) {
    auto input = std::ifstream{source / "tests/fixtures/cbor/rfc8949-supported.txt"};
    auto line = std::string{};
    auto index = 0;
    while (std::getline(input, line)) {
        const auto bar = line.find('|');
        if (line.empty() || line.front() == '#' || bar == std::string::npos) {
            continue;
        }
        output.add("cbor", "rfc8949-" + std::to_string(index++), from_hex(line.substr(bar + 1)));
    }
}

void object_seeds(corpus& output, const fs::path& source) {
    auto tree_hex = std::string{};
    std::ifstream{source / "tests/fixtures/records/empty-tree.hex"} >> tree_hex;
    output.add("objects", "empty-tree", selected(1, from_hex(tree_hex)));

    auto blob = liquid::blob{};
    blob.chunks = {sample_digest(1), sample_digest(2)};
    blob.chunk_sizes = {4096, 17};
    blob.size = 4113;
    blob.content = sample_digest(3);
    output.add("objects", "blob", liquid::objects::encode(blob), 0);

    auto tree = liquid::tree{};
    tree.entries.push_back({"README.md", 0100644, liquid::node_kind::blob, sample_digest(4)});
    tree.entries.push_back({"src", 040000, liquid::node_kind::tree, sample_digest(5)});
    output.add("objects", "tree", liquid::objects::encode(tree), 1);
}

void sync_seeds(corpus& output) {
    const auto objects = std::vector{sample_digest(6), sample_digest(7)};
    output.add("sync", "metadata", liquid::sync_detail::encode_metadata({}), 0);
    output.add("sync", "object-list", liquid::sync_detail::encode_object_list(objects), 1);
    const auto contents = to_bytes("drip contents");
    auto frame = liquid::sync_detail::encode_pack_frame(sample_digest(8), contents);
    output.add("sync", "pack-frame", frame, 2);
    if (frame) {
        auto two = *frame;
        two.insert(two.end(), frame->begin(), frame->end());
        output.add("sync", "pack-frames", selected(2, two));
    }
}

void drip_seeds(corpus& output) {
    const auto record = cbor::value{cbor::value::map{
        {cbor::value{"id"}, cbor::value{bytes(16, std::byte{1})}},
        {cbor::value{"base"}, cbor::value{cbor::value::array{}}},
        {cbor::value{"ops"}, cbor::value{cbor::value::array{}}},
        {cbor::value{"by"}, cbor::value{bytes(16, std::byte{2})}},
        {cbor::value{"at"}, cbor::value{std::int64_t{1'700'000'000}}},
    }};
    output.add("drip", "journal-record", cbor::encode(record), 0);
    const auto body = cbor::value{cbor::value::map{
        {cbor::value{"v"}, cbor::value{std::uint64_t{2}}},
        {cbor::value{"kind"}, cbor::value{"drip"}},
        {cbor::value{"operation"}, cbor::value{"write"}},
        {cbor::value{"at"}, cbor::value{std::int64_t{1'700'000'000}}},
        {cbor::value{"base"}, cbor::value{}},
        {cbor::value{"paths"}, cbor::value{cbor::value::array{cbor::value{"src/main.cpp"}}}},
    }};
    output.add("drip", "signed-body", cbor::encode(body), 1);
    const auto redaction = cbor::value{cbor::value::map{
        {cbor::value{"v"}, cbor::value{std::uint64_t{1}}},
        {cbor::value{"kind"}, cbor::value{"redaction"}},
        {cbor::value{"at"}, cbor::value{std::uint64_t{1'700'000'000}}},
        {cbor::value{"path"}, cbor::value{"secrets.env"}},
    }};
    output.add("drip", "redaction-body", cbor::encode(redaction), 1);
}

void identity_seeds(corpus& output, const fs::path& scratch) {
    const auto home = scratch / "identity-home";
    std::error_code ignored;
    fs::remove_all(home, ignored);
    ::setenv("LIQUID_HOME", home.c_str(), 1);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        std::cerr << "identity seeds skipped: " << signer.error().message << "\n";
        return;
    }
    auto first = signer->document();
    auto second = liquid::runtime::block_on(signer->rotate());
    auto signed_value =
        liquid::runtime::block_on(signer->sign("liquid-fuzz-seed", to_bytes("canonical body")));
    if (first) {
        output.add("identity", "document", liquid::encode_identity_document(*first), 0);
    }
    if (second) {
        output.add("identity", "rotated-document", liquid::encode_identity_document(*second), 0);
    }
    if (first && second) {
        auto encoded_first = liquid::encode_identity_document(*first);
        auto encoded_second = liquid::encode_identity_document(*second);
        if (encoded_first && encoded_second) {
            auto chain = bytes{std::byte{3}};
            append_part(chain, *encoded_first);
            append_part(chain, *encoded_second);
            output.add("identity", "chain", chain);
        }
    }
    if (signed_value) {
        output.add("identity",
                   "signature",
                   liquid::identity_detail::canonical_signature(*signed_value),
                   1);
    }
    fs::remove_all(home, ignored);
}

void http2_seeds(corpus& output) {
    auto settings = liquid::http2::frame{liquid::http2::frame_type::settings, 0, 0, {}};
    const auto setting = liquid::http2::setting_payload(4, 1'048'576);
    settings.payload.assign(setting.begin(), setting.end());
    output.add("http2", "settings-frame", liquid::http2::encode_frame(settings), 0);
    auto data = liquid::http2::frame{
        liquid::http2::frame_type::data, liquid::http2::flag_end_stream, 1, to_bytes("payload")};
    output.add("http2", "data-frame", liquid::http2::encode_frame(data), 0);

    auto encoder = liquid::http2::hpack_encoder{};
    const auto fields = std::vector<liquid::http2::header_field>{
        {":method", "POST"},
        {":path", "/build.bazel.remote.execution.v2.Execution/Execute"},
        {"content-type", "application/grpc"},
        {"te", "trailers"},
    };
    auto first = encoder.encode(fields);
    auto second = encoder.encode(fields);
    if (first && second) {
        auto blocks = bytes{std::byte{1}};
        append_part(blocks, *first);
        append_part(blocks, *second);
        output.add("http2", "hpack-blocks", blocks);
    }
    output.add(
        "http2", "huffman", selected(2, liquid::http2::huffman_encode("application/grpc+proto")));
    output.add("http2",
               "grpc-message",
               liquid::http2::encode_grpc_message({false,
                                                   to_bytes("\x0a\x03"
                                                            "abc")}),
               3);
    output.add("http2", "grpc-status", selected(4, to_bytes("5\nnot found")));
}

void http1_seeds(corpus& output) {
    output.add("http1",
               "request-get",
               selected(0, to_bytes("\x10GET /v1/info HTTP/1.1\r\nHost: lake\r\n\r\n")));
    output.add("http1",
               "request-post",
               selected(0,
                        to_bytes("\x07POST /v1/sync HTTP/1.1\r\nHost: lake\r\n"
                                 "Content-Length: 5\r\n\r\nhello")));
    output.add("http1",
               "request-chunked",
               selected(0,
                        to_bytes("\xffPUT /v1/blob HTTP/1.1\r\nHost: lake\r\n"
                                 "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")));
    output.add("http1",
               "response-length",
               selected(1, to_bytes("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")));
    output.add("http1",
               "response-chunked",
               selected(1,
                        to_bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                 "2\r\nok\r\n0\r\n\r\n")));
}

void configuration_seeds(corpus& output, const fs::path& source) {
    auto index = 0;
    for (const auto& entry :
         fs::recursive_directory_iterator(source / "tests/import/fixtures/golden")) {
        if (entry.path().extension() != ".liquid") {
            continue;
        }
        const auto text = read_file(entry.path());
        output.add("configuration", entry.path().stem().string(), selected(0, text));
        const auto document = liquid::parse_configuration(
            std::string_view{reinterpret_cast<const char*>(text.data()), text.size()},
            entry.path());
        if (!document) {
            continue;
        }
        const auto lowered = liquid::lower(*document);
        if (!lowered) {
            continue;
        }
        for (const auto& target : lowered->targets) {
            output.add("configuration",
                       "target-" + std::to_string(index++),
                       liquid::encode_target(target),
                       1);
        }
        for (const auto& source_record : lowered->sources) {
            output.add("configuration",
                       "source-" + std::to_string(index++),
                       liquid::encode_source(source_record),
                       2);
        }
        for (const auto& package : lowered->packages) {
            output.add("resolve",
                       "package-" + std::to_string(index++),
                       liquid::encode_package(package),
                       1);
        }
    }
}

void resolve_seeds(corpus& output) {
    auto index = 0;
    for (const auto* spec :
         {"zlib", "hdf5@1.14.3", "hdf5@1.14: +mpi ~fortran mode=release", "openmpi@4.1 ^hwloc@2"}) {
        output.add("resolve", "spec-" + std::to_string(index++), selected(0, to_bytes(spec)));
    }
}

auto git_object(std::string_view kind, const bytes& payload) -> bytes {
    auto output = to_bytes(std::string{kind} + " " + std::to_string(payload.size()));
    output.push_back(std::byte{});
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

void git_seeds(corpus& output) {
    const auto commit = to_bytes("tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                                 "parent 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
                                 "author A U Thor <author@example.com> 1700000000 +0000\n"
                                 "committer A U Thor <author@example.com> 1700000000 +0000\n"
                                 "\nmessage\n");
    auto tree = to_bytes("100644 README.md");
    tree.push_back(std::byte{});
    const auto id = liquid::git_detail::sha1_hash(commit);
    tree.insert(tree.end(), id.begin(), id.end());
    output.add("git", "loose-blob", selected(0, git_object("blob", to_bytes("hello\n"))));
    output.add("git", "loose-commit", selected(0, git_object("commit", commit)));
    output.add("git", "loose-tree", selected(0, git_object("tree", tree)));
    output.add("git", "commit", selected(1, commit));
    output.add("git", "tree", selected(2, tree));
    // A delta from "hello world" to "hello there": copy 6 bytes, insert 5.
    auto delta = bytes{std::byte{3}};
    append_part(delta, to_bytes("hello world"));
    append_part(delta,
                bytes{std::byte{11},
                      std::byte{11},
                      std::byte{0x90},
                      std::byte{6},
                      std::byte{5},
                      std::byte{'t'},
                      std::byte{'h'},
                      std::byte{'e'},
                      std::byte{'r'},
                      std::byte{'e'}});
    output.add("git", "delta", delta);
}

void protobuf_seeds(corpus& output) {
    auto command = liquid::reapi::Command{};
    command.arguments = {"cc", "-c", "main.c"};
    command.output_paths = {"main.o"};
    command.working_directory = "src";
    output.add("protobuf", "command", liquid::reapi::encode(command), 2);
    output.add("protobuf", "command-fields", liquid::reapi::encode(command), 0);
    auto action = liquid::reapi::Action{};
    output.add("protobuf", "action", liquid::reapi::encode(action), 1);
    auto request = liquid::reapi::BatchUpdateBlobsRequest{};
    request.instance_name = "main";
    output.add("protobuf", "batch-update", liquid::reapi::encode(request), 8);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: liquid-fuzz-seeds <source directory> <corpus directory>\n";
        return 2;
    }
    const auto source = fs::path{argv[1]};
    const auto root = fs::path{argv[2]};
    fs::create_directories(root);
    auto output = corpus{root};
    cbor_seeds(output, source);
    object_seeds(output, source);
    sync_seeds(output);
    drip_seeds(output);
    identity_seeds(output, root);
    http2_seeds(output);
    http1_seeds(output);
    configuration_seeds(output, source);
    resolve_seeds(output);
    git_seeds(output);
    protobuf_seeds(output);
    std::cout << "wrote " << output.written() << " seeds to " << root.string() << "\n";
    return 0;
}

// Writes the seed corpus for every harness in this directory (liquid #407).
//
//   liquid-fuzz-seeds <source directory> <corpus directory>
//
// Each harness gets <corpus directory>/<harness>/. Seeds come from two places: the repository's
// existing test fixtures (the RFC 8949 CBOR vectors, the record fixtures, the `.liquid` package
// files used by the import tests), and records built with liquid's own encoders so that each
// decoder starts from inputs it accepts. Inputs carry the selector byte and length prefixes that
// fuzz.hpp describes. A real identity chain is made with a throwaway LIQUID_HOME.
//
// The harnesses of liquid #463 add: store packs and an index written through an in-memory backend,
// a Git pack and its index, deltas, the format fixtures (tests/fixtures/formats) and this program's
// own executable for the binary readers, the TSA root and the timestamp tokens inside the evidence
// records of tests/transparency/fixtures, the sealed-secret and TUF fixtures, workers messages, and
// automerge-cpp's own corpus (modules/automerge-cpp/fuzz/corpus).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <zlib.h>

#include "encoding/cbor.hpp"
#include "git/sha1.hpp"
#include "http2/frame.hpp"
#include "http2/grpc.hpp"
#include "http2/hpack.hpp"
#include "identity/taap.hpp"
#include "liquid/asn1.hpp"
#include "liquid/configuration.hpp"
#include "liquid/delta.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/resolve.hpp"
#include "liquid/runtime.hpp"
#include "liquid/x509.hpp"
#include "memory_backend.hpp"
#include "objects/records.hpp"
#include "reapi/messages.hpp"
#include "store/index.hpp"
#include "store/pack.hpp"
#include "sync/codec.hpp"
#include "workers/protocol.hpp"

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

// --- liquid #463 -------------------------------------------------------------------------------

auto read_hex_file(const fs::path& path) -> bytes {
    auto text = std::string{};
    std::ifstream{path} >> text;
    return from_hex(text);
}

auto concatenate(std::initializer_list<bytes> parts) -> bytes {
    auto output = bytes{};
    for (const auto& part : parts) {
        output.insert(output.end(), part.begin(), part.end());
    }
    return output;
}

auto prefixed(std::uint8_t selector, std::initializer_list<bytes> parts) -> bytes {
    auto output = bytes{static_cast<std::byte>(selector)};
    for (const auto& part : parts) {
        append_part(output, part);
    }
    return output;
}

// Little-endian, `width` bytes.
void put_le(bytes& output, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i != width; ++i) {
        output.push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xffU));
    }
}

void put_u32_be(bytes& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void store_seeds(corpus& output) {
    namespace store = liquid::store_detail;
    auto packs = std::make_shared<liquid::fuzz::memory_backend>();
    auto contents = store::pack_contents{7, {}, 16, true};
    for (const auto* text : {"the first chunk of a pack", "the second chunk of a pack"}) {
        const auto raw = to_bytes(text);
        const auto id = liquid::hash(liquid::algorithm::blake3, raw);
        if (!id) {
            return;
        }
        const auto entry = liquid::runtime::block_on(
            store::append_pack_entry(packs, "pack", contents, *id, raw, std::nullopt, false));
        if (!entry) {
            std::cerr << "store seeds skipped: " << entry.error().message << "\n";
            return;
        }
        contents.entries.push_back(*entry);
        contents.data_end += 49 + entry->location.length;
    }
    output.add("store", "open-pack", selected(0, packs->object("pack")));
    if (liquid::runtime::block_on(store::seal_pack(packs, "pack", contents))) {
        const auto sealed = packs->object("pack");
        output.add("store", "sealed-pack", selected(0, sealed));
        output.add("store", "sealed-pack-repaired", selected(1, sealed));
    }

    auto records = bytes{};
    auto ids = bytes{};
    auto indexes = std::make_shared<liquid::fuzz::memory_backend>();
    {
        auto index = store::chunk_index{indexes, "index", "seeds"};
        if (!liquid::runtime::block_on(index.open())) {
            return;
        }
        for (std::uint8_t fill = 1; fill <= 3; ++fill) {
            const auto id = sample_digest(fill);
            const auto location = store::chunk_location{7, 16U + 64U * fill, 64};
            (void)index.insert({id, location});
            records.insert(records.end(), id.bytes.begin(), id.bytes.end());
            put_le(records, location.pack_id, 4);
            put_le(records, location.offset, 8);
            put_le(records, location.length, 4);
            ids.insert(ids.end(), id.bytes.begin(), id.bytes.end());
        }
        if (!liquid::runtime::block_on(index.flush())) {
            return;
        }
    }
    const auto absent = sample_digest(9);
    ids.insert(ids.end(), absent.bytes.begin(), absent.bytes.end());
    output.add("store", "index-records", prefixed(3, {records, {}, ids}));
    const auto runs = liquid::runtime::block_on(indexes->list("index/run-"));
    if (runs && !runs->empty()) {
        // The run follows the two prefixed parts unprefixed; a run is larger than one part holds.
        output.add("store",
                   "index",
                   concatenate({prefixed(2, {indexes->object("index/MANIFEST"), ids}),
                                indexes->object(runs->front().key)}));
    }
}

auto zlib_deflate(const bytes& input) -> bytes {
    auto size = compressBound(static_cast<uLong>(input.size()));
    auto output = bytes(size);
    if (compress2(reinterpret_cast<Bytef*>(output.data()),
                  &size,
                  reinterpret_cast<const Bytef*>(input.data()),
                  static_cast<uLong>(input.size()),
                  Z_BEST_COMPRESSION) != Z_OK) {
        return {};
    }
    output.resize(size);
    return output;
}

void gitpack_seeds(corpus& output) {
    namespace git = liquid::git_detail;
    const auto base = to_bytes("hello\n");
    const auto target = to_bytes("hello there");
    // A delta from "hello\n" to "hello there": copy 5 bytes from offset 0, insert " there".
    const auto delta = bytes{std::byte{6},
                             std::byte{11},
                             std::byte{0x90},
                             std::byte{5},
                             std::byte{6},
                             std::byte{' '},
                             std::byte{'t'},
                             std::byte{'h'},
                             std::byte{'e'},
                             std::byte{'r'},
                             std::byte{'e'}};
    auto pack = to_bytes("PACK");
    put_u32_be(pack, 2);
    put_u32_be(pack, 2);
    const auto first = static_cast<std::uint32_t>(pack.size());
    pack.push_back(std::byte{0x36}); // blob, size 6
    const auto blob = zlib_deflate(base);
    pack.insert(pack.end(), blob.begin(), blob.end());
    const auto second = static_cast<std::uint32_t>(pack.size());
    pack.push_back(std::byte{0x6b}); // OFS_DELTA, size 11
    pack.push_back(static_cast<std::byte>(second - first));
    const auto packed_delta = zlib_deflate(delta);
    pack.insert(pack.end(), packed_delta.begin(), packed_delta.end());
    // The 20-byte trailer, then padding: object_store reads 64 header bytes at each object's
    // offset and fails on an object less than 64 bytes before the end of the file.
    pack.insert(pack.end(), 64, std::byte{});
    if (blob.empty() || packed_delta.empty() || second - first >= 128) {
        return;
    }
    struct entry {
        git::sha1 id;
        std::uint32_t offset;
    };
    auto entries = std::vector<entry>{{git::sha1_hash(git_object("blob", base)), first},
                                      {git::sha1_hash(git_object("blob", target)), second}};
    std::ranges::sort(entries, {}, &entry::id);
    auto records = bytes{};
    auto index = bytes{};
    put_u32_be(index, 0xff744f63U);
    put_u32_be(index, 2);
    for (unsigned bucket = 0; bucket < 256; ++bucket) {
        put_u32_be(
            index,
            static_cast<std::uint32_t>(std::ranges::count_if(entries, [bucket](const entry& value) {
                return std::to_integer<unsigned>(value.id.front()) <= bucket;
            })));
    }
    for (const auto& value : entries) {
        index.insert(index.end(), value.id.begin(), value.id.end());
        records.insert(records.end(), value.id.begin(), value.id.end());
        put_u32_be(records, value.offset);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        put_u32_be(index, 0);
    }
    for (const auto& value : entries) {
        put_u32_be(index, value.offset);
    }
    output.add("gitpack", "index-and-pack", prefixed(0, {index, pack}));
    output.add("gitpack", "records-and-pack", prefixed(1, {records, pack}));
}

void delta_seeds(corpus& output) {
    const auto source =
        to_bytes("liquid captures every write as a signed drip; this is the source");
    const auto target =
        to_bytes("liquid captures every write as a signed drip; this is the target");
    const auto patch = liquid::make_delta(liquid::delta_algorithm::zstd_patch, source, target);
    if (patch) {
        output.add("delta", "zstd-patch", prefixed(1, {source, *patch}));
        output.add("delta", "executable-sections", prefixed(2, {source, *patch}));
    }
    const auto record = liquid::delta_record{
        sample_digest(1), sample_digest(2), liquid::delta_algorithm::zstd_patch, sample_digest(3)};
    output.add("delta", "record", liquid::encode_delta(record), 0);
}

void text_seeds(corpus& output, const fs::path& source) {
    auto index = 0;
    for (const auto& path : {source / "tests/fixtures/status-flow.json",
                             source / "tests/fixtures/admission/review.json",
                             source / "tests/fixtures/admission/tagged.json",
                             source / "tests/fixtures/tuf/1.targets.json"}) {
        output.add("text", "json-" + std::to_string(index++), selected(0, read_file(path)));
    }
    output.add("text",
               "json-escapes",
               selected(0, to_bytes(R"({"a":[1,-2.5e3,true,false,null],"b":"é\n\"x\""})")));
    output.add("text",
               "dotenv",
               selected(1,
                        to_bytes("# database\nexport DATABASE_URL=postgres://db/app\n\n"
                                 "TOKEN=\"quoted value\"\r\nEMPTY=\n")));
}

void format_seeds(corpus& output, const fs::path& source, const fs::path& executable) {
    const auto fixtures = source / "tests/fixtures/formats";
    for (const auto* name :
         {"archive-a.zip", "archive-b.zip", "descriptor.zip", "stored.zip", "volatile-a.zip"}) {
        const auto zip = read_file(fixtures / name);
        for (std::uint8_t selector = 0; selector < 5; ++selector) {
            output.add("archive",
                       std::string{name} + "-" + std::to_string(selector),
                       selected(selector, zip));
        }
        output.add("archive", std::string{name} + "-registry", selected(10, zip));
    }
    const auto tar = read_file(fixtures / "pax.tar");
    output.add("archive", "pax.tar", selected(5, tar));
    output.add("archive", "pax.tar-registry", selected(10, tar));
    output.add("archive", "pax.tar.gz-registry", selected(10, read_file(fixtures / "pax.tar.gz")));
    output.add(
        "archive", "pax.tar.zst-registry", selected(10, read_file(fixtures / "pax.tar.zst")));
    // An ar archive with Debian's first member.
    const auto ar = to_bytes("!<arch>\n"
                             "debian-binary   0           0     0     100644  4         `\n"
                             "2.0\n");
    output.add("archive", "ar", selected(9, ar));
    output.add("archive", "deb-prefix", selected(6, ar));

    output.add("data", "sample-a.h5", selected(0, read_file(fixtures / "sample-a.h5")));
    output.add("data", "sample-b.h5", selected(0, read_file(fixtures / "sample-b.h5")));
    output.add("data", "sample.parquet", selected(1, read_file(fixtures / "sample.parquet")));
    output.add("data", "sample.sqlite", selected(2, read_file(fixtures / "sample.sqlite")));
    output.add("data",
               "minimal.pdf",
               selected(3,
                        to_bytes("%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
                                 "2 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\n"
                                 "trailer\n<< /Root 1 0 R >>\n%%EOF\n")));

    output.add("model",
               "notebook",
               selected(0,
                        to_bytes(R"json({"cells":[{"cell_type":"code","execution_count":1,)json"
                                 R"json("metadata":{},"outputs":[],"source":["print(1)"]}],)json"
                                 R"json("metadata":{},"nbformat":4,"nbformat_minor":5})json")));
const auto header = to_bytes(R"({"weight":{"dtype":"F32","shape":[2],"data_offsets":[0,8]}})");
auto safetensors = bytes{};
put_le(safetensors, header.size(), 8);
safetensors.insert(safetensors.end(), header.begin(), header.end());
safetensors.insert(safetensors.end(), 8, std::byte{});
output.add("model", "safetensors", selected(1, safetensors));
// pickle protocol 2 of {'a': 1}.
output.add("model", "pickle", selected(2, from_hex("80027d710058010000006171014b01732e")));

// This program is an ELF executable on Linux and a Mach-O one on macOS.
const auto self = read_file(executable);
if (!self.empty()) {
    for (std::uint8_t selector = 0; selector < 6; ++selector) {
        output.add("binary", "self-" + std::to_string(selector), selected(selector, self));
    }
}
// The smallest PE header: "MZ", e_lfanew at 0x3c pointing at "PE\0\0" and a COFF header.
auto pe = bytes(0x40, std::byte{});
pe[0] = std::byte{'M'};
pe[1] = std::byte{'Z'};
pe[0x3c] = std::byte{0x40};
const auto signature = to_bytes(std::string_view{"PE\0\0", 4});
pe.insert(pe.end(), signature.begin(), signature.end());
pe.insert(pe.end(), {std::byte{0x64}, std::byte{0x86}}); // x86-64
pe.insert(pe.end(), 18, std::byte{});
output.add("binary", "pe-codec", selected(2, pe));
output.add("binary", "pe-inspect", selected(5, pe));
}

// Every ContentInfo whose content type is CMS SignedData (1.2.840.113549.1.7.2): the timestamp
// tokens inside an evidence record.
void find_signed_data(std::span<const std::byte> input, std::vector<bytes>& found, unsigned depth) {
    auto reader = liquid::asn1::reader{input};
    for (;;) {
        const auto element = reader.next();
        if (!element || !*element || depth > 32) {
            return;
        }
        if (!(*element)->identifier.constructed) {
            continue;
        }
        auto children = liquid::asn1::reader{(*element)->content};
        const auto first = children.next();
        if (first && *first) {
            const auto type = liquid::asn1::object_identifier(**first);
            if (type && *type == "1.2.840.113549.1.7.2") {
                found.emplace_back((*element)->encoded.begin(), (*element)->encoded.end());
                continue;
            }
        }
        find_signed_data((*element)->content, found, depth + 1);
    }
}

void der_and_transparency_seeds(corpus& output, const fs::path& source) {
    const auto fixtures = source / "tests/transparency/fixtures";
    const auto pem = read_file(fixtures / "tsa-root.pem");
    auto trust = liquid::x509::trust_anchor_set{};
    if (!trust.add_pem(std::string_view{reinterpret_cast<const char*>(pem.data()), pem.size()}) ||
        trust.certificates().empty()) {
        std::cerr << "DER seeds skipped: tsa-root.pem did not load\n";
        return;
    }
    const auto root = trust.certificates().front();
    output.add("der", "tsa-root", selected(0, root));
    output.add("der", "tsa-root-certificate", selected(1, root));
    output.add("der", "tsa-root-pem", selected(2, pem));
    output.add("der", "tsa-root-der", selected(2, root));
    output.add("der", "tsa-root-path", prefixed(3, {root, root}));

    const auto checkpoint_signature = read_file(fixtures / "checkpoint-signature.bin");
    auto index = 0;
    for (const auto* name : {"lake-initial.ers", "lake-renewed.ers"}) {
        const auto record = read_file(fixtures / name);
        output.add("der", std::string{name} + "-der", selected(0, record));
        output.add(
            "transparency", std::string{name}, prefixed(1, {record, checkpoint_signature, root}));
        auto tokens = std::vector<bytes>{};
        find_signed_data(record, tokens, 0);
        for (const auto& token : tokens) {
            const auto number = std::to_string(index++);
            output.add("der", "signed-data-" + number, prefixed(4, {token, root}));
            output.add("transparency",
                       "timestamp-token-" + number,
                       prefixed(0, {token, checkpoint_signature, root}));
        }
    }
    auto entry_tile = bytes{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{1}};
    const auto entry_header = to_bytes("LAKE-TILE-1\n");
    entry_tile.insert(entry_tile.end(), entry_header.begin(), entry_header.end());
    put_u32_be(entry_tile, 5);
    const auto entry = to_bytes("entry");
    entry_tile.insert(entry_tile.end(), entry.begin(), entry.end());
    output.add("transparency", "entry-tile", entry_tile);
    auto hash_tile = bytes{std::byte{2}, std::byte{1}, std::byte{0}, std::byte{1}};
    const auto hash_header = to_bytes("LAKE-HASH-TILE-1\n");
    hash_tile.insert(hash_tile.end(), hash_header.begin(), hash_header.end());
    hash_tile.insert(hash_tile.end(), 32, std::byte{0x11});
    output.add("transparency", "hash-tile", hash_tile);
}

void secret_seeds(corpus& output, const fs::path& source) {
    const auto record = read_hex_file(source / "tests/fixtures/sealed-secret/record.cbor.hex");
    output.add("secret", "record", selected(0, record));
    output.add("secret", "record-unseal", selected(1, record));
    output.add(
        "secret",
        "lake-key",
        selected(2,
                 to_bytes(R"({"ok":true,"value":{"kid":"lake-key-2026-09","public_key":)"
                          R"("86517bc0c39fe876b7646cb5e34ee03daf71ccbad67450c16a970ad8cbc03e7e",)"
                          R"("algorithm":"x25519"}})")));
    output.add("secret",
               "lake-error",
               selected(2,
                        to_bytes(R"({"ok":false,"error":{"code":"not_found","message":"no key",)"
                                 R"("path":"sealing_public_key"}})")));
}

void tuf_seeds(corpus& output, const fs::path& source) {
    const auto fixtures = source / "tests/fixtures/tuf";
    const auto root = read_file(fixtures / "1.root.json");
    output.add("tuf", "root", selected(0, root));
    output.add("tuf",
               "update",
               prefixed(1,
                        {root,
                         read_file(fixtures / "1.timestamp.json"),
                         read_file(fixtures / "1.snapshot.json"),
                         read_file(fixtures / "1.targets.json"),
                         read_file(fixtures / "entry.cbor")}));
}

void workers_seeds(corpus& output) {
    namespace workers = liquid::workers;
    auto submit = workers::submit_request{};
    submit.instance = "main";
    submit.action = {"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 142, {}};
    submit.native_action = sample_digest(4);
    submit.remote_eligible = true;
    output.add("workers", "submit", workers::encode(submit), 0);
    auto worker = workers::worker_request{};
    worker.instance = "main";
    worker.worker = "worker-1";
    worker.platform = {"linux", "x86_64"};
    output.add("workers", "worker", workers::encode(worker), 1);
    auto lease = workers::lease_response{};
    lease.assigned = true;
    lease.operation = "operations/1";
    lease.token = 7;
    lease.action_digest = submit.action;
    lease.action = to_bytes("action");
    lease.command = to_bytes("command");
    lease.native_action = sample_digest(5);
    output.add("workers", "lease", workers::encode(lease), 2);
    output.add("workers",
               "heartbeat",
               workers::encode(workers::heartbeat_request{
                   .operation = "operations/1", .worker = "worker-1", .token = "7"}),
               3);
    output.add("workers",
               "complete",
               workers::encode(workers::complete_request{.operation = "operations/1",
                                                         .worker = "worker-1",
                                                         .token = "7",
                                                         .result = to_bytes("result")}),
               4);
    output.add("workers",
               "acknowledgement",
               workers::encode(workers::acknowledgement{true, "accepted"}),
               5);
    output.add("workers",
               "read-object",
               workers::encode(workers::read_object_request{to_string(sample_digest(6))}),
               6);
    output.add("workers",
               "read-object-response",
               workers::encode(workers::read_object_response{to_bytes("contents")}),
               7);
    output.add(
        "workers", "status", workers::encode(workers::scheduler_status{3, 1, 10, 0, 2, 4}), 8);
}

void automerge_seeds(corpus& output, const fs::path& source) {
    const auto directory = source / "modules/automerge-cpp/fuzz/corpus";
    std::error_code code;
    if (!fs::is_directory(directory, code)) {
        std::cerr << "automerge seeds skipped: " << directory.string() << " is absent\n";
        return;
    }
    for (const auto& entry : fs::directory_iterator{directory, code}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto bytes_read = read_file(entry.path());
        const auto name = entry.path().filename().string();
        output.add("automerge-load", name, bytes_read);
        output.add("automerge-change-chunk", name, bytes_read);
    }
    output.add("automerge-leb128",
               "values",
               bytes{std::byte{0xe5},
                     std::byte{0x8e},
                     std::byte{0x26},
                     std::byte{0x7f},
                     std::byte{0x80},
                     std::byte{0x01}});
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
    store_seeds(output);
    gitpack_seeds(output);
    delta_seeds(output);
    text_seeds(output, source);
    format_seeds(output, source, fs::path{argv[0]});
    der_and_transparency_seeds(output, source);
    secret_seeds(output, source);
    tuf_seeds(output, source);
    workers_seeds(output);
    automerge_seeds(output, source);
    std::cout << "wrote " << output.written() << " seeds to " << root.string() << "\n";
    return 0;
}

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/hash.hpp"
#include "liquid/object.hpp"
#include "reapi/messages.hpp"

namespace liquid::reapi {

struct DirectoryObject {
    Digest digest;
    Directory directory;
    std::vector<std::byte> bytes;
};

struct DirectoryTree {
    Digest root_digest;
    std::vector<DirectoryObject> objects;
};

struct TreeSource {
    std::function<result<liquid::tree>(liquid::digest)> tree;
    std::function<result<std::vector<std::byte>>(liquid::digest)> blob;
};

class DirectoryCache {
public:
    [[nodiscard]] auto convert(liquid::digest root,
                               algorithm hash_algorithm,
                               const TreeSource& source,
                               std::uint32_t root_mode = 0040755) -> result<DirectoryTree>;
    [[nodiscard]] auto size() const noexcept -> std::size_t;

private:
    std::map<std::string, DirectoryTree> entries_;
};

struct ActionMessages {
    Command command;
    Digest command_digest;
    Action action;
    Digest action_digest;
};

struct DecodedAction {
    action_input input;
    liquid::digest sandbox;
    Digest input_root_digest;
};

struct BlobResolver {
    std::function<result<std::vector<std::byte>>(liquid::digest)> bytes;
};

[[nodiscard]] auto select_digest_algorithm(const ServerCapabilities& capabilities, bool execution)
    -> result<algorithm>;
[[nodiscard]] auto digest_function(algorithm hash_algorithm) -> result<DigestFunctionValue>;
[[nodiscard]] auto digest_algorithm(DigestFunctionValue function) -> result<algorithm>;
[[nodiscard]] auto make_digest(algorithm hash_algorithm, std::span<const std::byte> bytes)
    -> result<Digest>;
[[nodiscard]] auto to_liquid_digest(const Digest& value, algorithm hash_algorithm)
    -> result<liquid::digest>;

[[nodiscard]] auto to_reapi_action(const action_input& input,
                                   const Digest& input_root_digest,
                                   liquid::digest sandbox,
                                   algorithm hash_algorithm) -> result<ActionMessages>;
[[nodiscard]] auto from_reapi_action(const Action& action,
                                     const Command& command,
                                     DigestFunctionValue function) -> result<DecodedAction>;

[[nodiscard]] auto to_reapi_result(const action_result& value,
                                   algorithm hash_algorithm,
                                   const BlobResolver& blobs) -> result<ActionResult>;
[[nodiscard]] auto from_reapi_result(const ActionResult& value,
                                     liquid::digest action,
                                     algorithm hash_algorithm) -> result<action_result>;

} // namespace liquid::reapi

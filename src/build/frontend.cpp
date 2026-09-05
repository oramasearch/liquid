#include "liquid/build.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"

namespace liquid::build {
namespace {

namespace cbor = encoding::cbor;

auto action_error(errc code, std::string message) -> error {
    return {code, std::move(message), "symbol action"};
}

auto find(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* object = std::get_if<cbor::value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    for (const auto& [candidate, child] : *object) {
        const auto* text = std::get_if<std::string>(&candidate.data);
        if (text && *text == key) {
            return &child;
        }
    }
    return nullptr;
}

auto remove_compile_output(std::vector<std::string>& command) -> void {
    for (auto index = std::size_t{}; index < command.size();) {
        if (command[index] == "-c") {
            command.erase(command.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (command[index] == "-o") {
            command.erase(command.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < command.size()) {
                command.erase(command.begin() + static_cast<std::ptrdiff_t>(index));
            }
            continue;
        }
        if (command[index].starts_with("-o") && command[index].size() > 2) {
            command.erase(command.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
}

} // namespace

runtime::task<std::vector<std::byte>> execute_symbol_action(
    repository& source, action_input compile_action, std::span<const std::byte> symbol_action) {
    auto decoded = cbor::decode(symbol_action);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    auto canonical = cbor::encode(*decoded);
    if (!canonical || canonical->size() != symbol_action.size() ||
        !std::equal(canonical->begin(), canonical->end(), symbol_action.begin())) {
        co_return std::unexpected(
            action_error(errc::corrupt, "symbol action is not canonical CBOR"));
    }
    const auto* kind_value = find(*decoded, "kind");
    const auto* flags_value = find(*decoded, "flags");
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* flags = flags_value ? std::get_if<cbor::value::array>(&flags_value->data) : nullptr;
    if (!kind || *kind != "clang-symbols" || !flags) {
        co_return std::unexpected(action_error(errc::invalid_argument, "invalid symbol action"));
    }
    remove_compile_output(compile_action.command);
    for (const auto& value : *flags) {
        const auto* flag = std::get_if<std::string>(&value.data);
        if (!flag || flag->empty()) {
            co_return std::unexpected(
                action_error(errc::corrupt, "symbol action has an invalid flag"));
        }
        compile_action.command.push_back(*flag);
    }
    compile_action.outputs.clear();
    compile_action.remote_eligible = true;
    auto action_result_id = co_await execute(source, std::move(compile_action));
    if (!action_result_id) {
        co_return std::unexpected(action_result_id.error());
    }
    auto recorded = co_await liquid::build::result(source, *action_result_id);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    if (recorded->exit != 0) {
        co_return std::unexpected(
            action_error(errc::internal, "symbol action compiler exited with a failure status"));
    }
    co_return co_await source.blob(recorded->stdout_output);
}

} // namespace liquid::build

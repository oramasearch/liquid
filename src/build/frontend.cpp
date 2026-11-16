// SPDX-License-Identifier: MIT

#include "liquid/build.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "build/wrap.hpp"
#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"

namespace liquid::build {
namespace {

namespace cbor = encoding::cbor;

constexpr auto fixed_environment = std::array<std::string_view, 7>{
    "PATH", "HOME", "TMPDIR", "LANG", "TZ", "SOURCE_DATE_EPOCH", "ZERO_AR_DATE"};

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

runtime::task<std::vector<std::byte>> execute_symbol_action(repository& source,
                                                            action_input compile_action,
                                                            std::vector<std::byte> symbol_action) {
    auto decoded = cbor::decode(symbol_action);
    if (!decoded) {
        co_return std::unexpected(action_error(decoded.error().code, decoded.error().message));
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
    for (const auto name : fixed_environment) {
        compile_action.env.erase(std::string{name});
    }
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
    const auto executable = compile_action.command.front();
    auto toolchain = co_await phase2_toolchain(
        source, compile_action.toolchain, compile_action.inputs, compile_action.cwd, executable);
    if (!toolchain) {
        co_return std::unexpected(action_error(
            toolchain.error().code, "symbol action toolchain: " + toolchain.error().message));
    }
    compile_action.toolchain = toolchain->first;
    compile_action.command.front() = std::move(toolchain->second);
    compile_action.env["LD_LIBRARY_PATH"] = "/lib";
    auto action_result_id = co_await execute(source, std::move(compile_action));
    if (!action_result_id) {
        co_return std::unexpected(
            action_error(action_result_id.error().code,
                         "symbol action execution: " + action_result_id.error().message + " (" +
                             action_result_id.error().context + ")"));
    }
    auto recorded = co_await liquid::build::result(source, *action_result_id);
    if (!recorded) {
        co_return std::unexpected(action_error(recorded.error().code, recorded.error().message));
    }
    if (recorded->exit != 0) {
        auto stderr_output = co_await source.blob(recorded->stderr_output);
        auto diagnostic = std::string{};
        if (stderr_output) {
            diagnostic.assign(reinterpret_cast<const char*>(stderr_output->data()),
                              stderr_output->size());
        }
        co_return std::unexpected(action_error(errc::internal,
                                               "symbol action compiler " + executable +
                                                   " exited with a failure status: " + diagnostic));
    }
    auto stdout_output = co_await source.blob(recorded->stdout_output);
    if (!stdout_output) {
        co_return std::unexpected(action_error(
            stdout_output.error().code, "symbol action output: " + stdout_output.error().message));
    }
    co_return std::move(*stdout_output);
}

} // namespace liquid::build

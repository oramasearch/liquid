#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '\"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto join(int argc, char** argv) -> std::string {
    auto output = std::string{};
    for (auto index = 0; index < argc; ++index) {
        if (index != 0) {
            output += ' ';
        }
        output += argv[index];
    }
    return output;
}

auto input_files(const std::filesystem::path& root)
    -> liquid::result<std::vector<std::filesystem::path>> {
    auto code = std::error_code{};
    auto output = std::vector<std::filesystem::path>{};
    if (!std::filesystem::exists(root, code)) {
        if (code) {
            return std::unexpected(liquid::error{liquid::errc::io, code.message(), root.string()});
        }
        return output;
    }
    for (std::filesystem::recursive_directory_iterator iterator{
             root, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        if (iterator->is_regular_file(code)) {
            output.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), root.string()});
    }
    return output;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

} // namespace

auto repo_init(int argc, char** argv) -> int {
    if (argc > 1) {
        return usage("init accepts at most one path");
    }
    const auto path = argc == 1 ? std::filesystem::path{argv[0]} : std::filesystem::current_path();
    auto files = input_files(path);
    if (!files) {
        return fail(files.error());
    }
    auto repository = liquid::runtime::block_on(liquid::repository::create(path));
    if (!repository) {
        return fail(repository.error());
    }
    for (const auto& source : *files) {
        auto relative = source.lexically_relative(path).generic_string();
        auto written =
            liquid::runtime::block_on(repository->write_file(std::move(relative), source));
        if (!written) {
            return fail(written.error());
        }
    }
    std::println("initialised {}", path.string());
    return 0;
}

auto repo_status(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("status accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto current = repository->status();
    if (!current) {
        return fail(current.error());
    }
    const auto head = current->head ? liquid::to_string(current->head->value) : std::string{};
    if (json) {
        std::println("{{\"pending_drips\":{},\"paused\":{},\"conflicts\":{},\"oversized_"
                     "directories\":{},\"head\":{}}}",
                     current->pending_drips,
                     current->paused ? "true" : "false",
                     current->conflicts.size(),
                     current->oversized_directories.size(),
                     current->head ? json_string(head) : "null");
    } else {
        std::println("pending_drips={}", current->pending_drips);
        std::println("paused={}", current->paused);
        std::println("conflicts={}", current->conflicts.size());
        std::println("oversized_directories={}", current->oversized_directories.size());
        std::println("head={}", head);
    }
    return 0;
}

auto repo_log(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("log accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto changes = liquid::runtime::block_on(repository->log());
    if (!changes) {
        return fail(changes.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < changes->size(); ++index) {
            const auto& change = (*changes)[index];
            std::print("{}{{\"id\":{},\"message\":{},\"author\":{},\"at\":{}}}",
                       index == 0 ? "" : ",",
                       json_string(liquid::to_string(change.id.value)),
                       json_string(change.message),
                       json_string(change.author.taap_id),
                       change.at.time_since_epoch().count());
        }
        std::println("]");
    } else {
        for (const auto& change : *changes) {
            std::println("{} {} {}",
                         liquid::to_string(change.id.value),
                         change.author.taap_id,
                         change.message);
        }
    }
    return 0;
}

auto repo_cut(int argc, char** argv) -> int {
    if (argc == 0) {
        return usage("cut requires a message");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto change = liquid::runtime::block_on(repository->cut(join(argc, argv)));
    if (!change) {
        return fail(change.error());
    }
    std::println("change={}", liquid::to_string(change->value));
    return 0;
}

auto repo_refs(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("refs accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto values = liquid::runtime::block_on(repository->refs());
    if (!values) {
        return fail(values.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < values->size(); ++index) {
            const auto& value = (*values)[index];
            std::print("{}{{\"name\":{},\"id\":{}}}",
                       index == 0 ? "" : ",",
                       json_string(value.name),
                       json_string(liquid::to_string(value.id.value)));
        }
        std::println("]");
    } else {
        for (const auto& value : *values) {
            std::println("{}={}", value.name, liquid::to_string(value.id.value));
        }
    }
    return 0;
}

} // namespace liquid_cli

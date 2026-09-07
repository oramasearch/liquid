#include "projection/platform_tools.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace liquid::projection_detail {
namespace {

auto failure(errc code, std::string message, const std::filesystem::path& context) -> error {
    return {code, std::move(message), context.string()};
}

auto text(const std::filesystem::path& path) -> result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read tool output", path));
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

#if defined(_WIN32)

auto wide(std::string_view value) -> std::wstring {
    if (value.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    auto output = std::wstring(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        output.data(),
                        size);
    return output;
}

auto quote_windows(std::string_view value) -> std::string {
    if (value.find_first_of(" \t\"") == std::string_view::npos) {
        return std::string{value};
    }
    auto output = std::string{"\""};
    auto slashes = std::size_t{};
    for (const auto character : value) {
        if (character == '\\') {
            ++slashes;
            continue;
        }
        if (character == '"') {
            output.append(slashes * 2 + 1, '\\');
            output.push_back('"');
            slashes = 0;
            continue;
        }
        output.append(slashes, '\\');
        slashes = 0;
        output.push_back(character);
    }
    output.append(slashes * 2, '\\');
    output.push_back('"');
    return output;
}

auto windows_command(const std::vector<std::string>& command) -> std::wstring {
    auto encoded = std::string{};
    for (const auto& argument : command) {
        if (!encoded.empty()) {
            encoded.push_back(' ');
        }
        encoded += quote_windows(argument);
    }
    return wide(encoded);
}

auto windows_error() -> std::string {
    return std::system_category().message(static_cast<int>(GetLastError()));
}

#else

auto pointers(std::vector<std::string>& values) -> std::vector<char*> {
    auto output = std::vector<char*>{};
    output.reserve(values.size() + 1);
    for (auto& value : values) {
        output.push_back(value.data());
    }
    output.push_back(nullptr);
    return output;
}

auto environment_block(const std::map<std::string, std::string>& overrides)
    -> std::vector<std::string> {
    auto values = std::map<std::string, std::string>{};
    for (auto current = environ; current != nullptr && *current != nullptr; ++current) {
        const auto entry = std::string_view{*current};
        const auto separator = entry.find('=');
        if (separator != std::string_view::npos) {
            values.emplace(entry.substr(0, separator), entry.substr(separator + 1));
        }
    }
    for (const auto& [name, value] : overrides) {
        values.insert_or_assign(name, value);
    }
    auto output = std::vector<std::string>{};
    output.reserve(values.size());
    for (const auto& [name, value] : values) {
        output.push_back(name + "=" + value);
    }
    return output;
}

auto process_exit(int status) -> int {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 255;
}

#endif

} // namespace

std::filesystem::path temporary_path(std::string_view label) {
    static auto sequence = std::atomic_uint64_t{};
#if defined(_WIN32)
    const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(getpid());
#endif
    return std::filesystem::temp_directory_path() /
           ("liquid-" + std::string{label} + "-" + std::to_string(process) + "-" +
            std::to_string(sequence.fetch_add(1)));
}

result<tool_result> run_tool(const std::vector<std::string>& command,
                             const std::filesystem::path& working_directory,
                             const std::map<std::string, std::string>& environment) {
    if (command.empty() || command.front().empty()) {
        return std::unexpected(error{errc::invalid_argument, "platform tool command is empty", {}});
    }
    const auto output_path = temporary_path("tool-stdout");
    const auto error_path = temporary_path("tool-stderr");
#if defined(_WIN32)
    auto security = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto output_handle = CreateFileW(wide(output_path.string()).c_str(),
                                           GENERIC_WRITE,
                                           FILE_SHARE_READ,
                                           &security,
                                           CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_TEMPORARY,
                                           nullptr);
    const auto error_handle = CreateFileW(wide(error_path.string()).c_str(),
                                          GENERIC_WRITE,
                                          FILE_SHARE_READ,
                                          &security,
                                          CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_TEMPORARY,
                                          nullptr);
    if (output_handle == INVALID_HANDLE_VALUE || error_handle == INVALID_HANDLE_VALUE) {
        if (output_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(output_handle);
        }
        if (error_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(error_handle);
        }
        return std::unexpected(error{errc::io, windows_error(), command.front()});
    }
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output_handle;
    startup.hStdError = error_handle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    auto information = PROCESS_INFORMATION{};
    auto encoded = windows_command(command);
    auto directory = wide(working_directory.string());
    for (const auto& [name, value] : environment) {
        SetEnvironmentVariableW(wide(name).c_str(), wide(value).c_str());
    }
    const auto created = CreateProcessW(nullptr,
                                        encoded.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        directory.empty() ? nullptr : directory.c_str(),
                                        &startup,
                                        &information);
    CloseHandle(output_handle);
    CloseHandle(error_handle);
    if (!created) {
        return std::unexpected(error{errc::io, windows_error(), command.front()});
    }
    WaitForSingleObject(information.hProcess, INFINITE);
    auto status = DWORD{};
    GetExitCodeProcess(information.hProcess, &status);
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    const auto exit = static_cast<int>(status);
#else
    const auto output_descriptor = ::open(output_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    const auto error_descriptor = ::open(error_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (output_descriptor < 0 || error_descriptor < 0) {
        if (output_descriptor >= 0) {
            ::close(output_descriptor);
        }
        if (error_descriptor >= 0) {
            ::close(error_descriptor);
        }
        return std::unexpected(error{errc::io, std::strerror(errno), command.front()});
    }
    auto arguments = std::vector<std::string>{command.begin(), command.end()};
    auto argument_pointers = pointers(arguments);
    auto environment_values = environment_block(environment);
    auto environment_pointers = pointers(environment_values);
    auto actions = posix_spawn_file_actions_t{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output_descriptor, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error_descriptor, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output_descriptor);
    posix_spawn_file_actions_addclose(&actions, error_descriptor);
#if defined(__APPLE__)
    if (!working_directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
    }
#endif
    auto pid = pid_t{};
    const auto spawned = posix_spawnp(&pid,
                                      arguments.front().c_str(),
                                      &actions,
                                      nullptr,
                                      argument_pointers.data(),
                                      environment_pointers.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(output_descriptor);
    ::close(error_descriptor);
    if (spawned != 0) {
        return std::unexpected(error{errc::io, std::strerror(spawned), command.front()});
    }
    auto status = int{};
    if (::waitpid(pid, &status, 0) < 0) {
        return std::unexpected(error{errc::io, std::strerror(errno), command.front()});
    }
    const auto exit = process_exit(status);
#endif
    auto output = text(output_path);
    auto error_output = text(error_path);
    auto ignored = std::error_code{};
    std::filesystem::remove(output_path, ignored);
    std::filesystem::remove(error_path, ignored);
    if (!output) {
        return std::unexpected(output.error());
    }
    if (!error_output) {
        return std::unexpected(error_output.error());
    }
    return tool_result{exit, std::move(*output), std::move(*error_output)};
}

result<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read file", path));
    }
    const auto value =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return std::vector<std::byte>{reinterpret_cast<const std::byte*>(value.data()),
                                  reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

result<std::string> read_text_file(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

result<void> write_text_file(const std::filesystem::path& path, std::string_view value) {
    auto code = std::error_code{};
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.parent_path()));
    }
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    if (!output) {
        return std::unexpected(failure(errc::io, "cannot write file", path));
    }
    return {};
}

result<digest> sha256_file(const std::filesystem::path& path) {
    auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return hash(algorithm::sha256, *bytes);
}

result<void> remove_known_path(const std::filesystem::path& path) {
    if (path.empty() || !path.filename().string().starts_with("liquid-")) {
        return std::unexpected(error{
            errc::invalid_argument, "refusing to remove an unknown platform path", path.string()});
    }
    auto code = std::error_code{};
    std::filesystem::remove_all(path, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path));
    }
    return {};
}

} // namespace liquid::projection_detail

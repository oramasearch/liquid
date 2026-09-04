#include "sandbox/sandbox.hpp"

#if defined(__APPLE__)

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace liquid::sandbox_detail {
namespace {

auto escaped(std::string value) -> std::string {
    for (auto offset = std::size_t{}; (offset = value.find('"', offset)) != std::string::npos;) {
        value.insert(offset, "\\");
        offset += 2;
    }
    return value;
}

auto write_profile(const launch_request& request, const std::filesystem::path& path)
    -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(
            error{errc::io, "cannot create sandbox-exec profile", path.string()});
    output << "(version 1)\n(deny default)\n(allow process*)\n(deny network*)\n";
    output << "(allow file-read* (subpath \"" << escaped(request.work_root) << "\"))\n";
    output << "(allow file-read* (subpath \"" << escaped(request.toolchain_root) << "\"))\n";
    output << "(allow file-read* (subpath \"/System/Library\") (subpath \"/usr/lib\"))\n";
    output << "(allow file-write* (subpath \"" << escaped(request.output_root.string())
           << "\") (subpath \"" << escaped(request.temporary_root) << "\"))\n";
    output.close();
    if (!output)
        return std::unexpected(error{errc::io, "cannot write sandbox-exec profile", path.string()});
    return {};
}

} // namespace

auto launch(const launch_request& request) -> result<launch_result> {
    const auto profile = request.sandbox_root / "sandbox.sb";
    auto written = write_profile(request, profile);
    if (!written)
        return std::unexpected(written.error());
    return std::unexpected(
        error{errc::unsupported,
              "sandbox-exec actions require the fixed /liquid mount prepared by the executor",
              profile.string()});
}

} // namespace liquid::sandbox_detail

#endif

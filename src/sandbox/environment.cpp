#include "sandbox/environment.hpp"

#include <array>

namespace liquid::sandbox_detail {
namespace {

constexpr auto fixed_names =
    std::array{"PATH", "HOME", "TMPDIR", "LANG", "TZ", "SOURCE_DATE_EPOCH"};

auto mechanism_for(std::string_view os) -> result<std::string> {
    if (os == "linux")
        return "user+mount+pid+net namespaces";
    if (os == "macos")
        return "sandbox-exec";
    if (os == "windows")
        return std::unexpected(
            error{errc::unsupported,
                  "sandboxed actions are unavailable on Windows until Cloud Files and job objects",
                  "liquid Phase 6"});
    return std::unexpected(
        error{errc::unsupported, "unsupported action platform", std::string{os}});
}

auto action_token(const digest& action) -> std::string {
    const auto text = to_string(action);
    const auto separator = text.find(':');
    return separator == std::string::npos ? text : text.substr(separator + 1);
}

} // namespace

auto definition(const platform& target) -> result<objects::sandbox_record> {
    auto mechanism = mechanism_for(target.os);
    if (!mechanism)
        return std::unexpected(mechanism.error());
    return objects::sandbox_record{.mechanism = std::move(*mechanism),
                                   .work_root = "/liquid/work/{action}/",
                                   .toolchain_root = "/liquid/toolchain/{toolchain}/",
                                   .home_root = "/liquid/home/{action}/",
                                   .temporary_root = "/liquid/tmp/{action}/",
                                   .fixed_environment = {fixed_names.begin(), fixed_names.end()},
                                   .network = false};
}

auto environment(const action_input& input,
                 const digest& action,
                 std::chrono::sys_seconds source_time) -> result<environment_result> {
    for (const auto* name : fixed_names) {
        if (input.env.contains(name))
            return std::unexpected(error{errc::invalid_argument,
                                         "action environment cannot override a fixed variable",
                                         name});
    }
    const auto token = action_token(action);
    const auto toolchain = to_string(input.toolchain);
    environment_result output{.values = input.env,
                              .work_root = "/liquid/work/" + token,
                              .toolchain_root = "/liquid/toolchain/" + toolchain,
                              .home_root = "/liquid/home/" + token,
                              .temporary_root = "/liquid/tmp/" + token};
    output.values["PATH"] = output.toolchain_root + "/bin";
    output.values["HOME"] = output.home_root;
    output.values["TMPDIR"] = output.temporary_root;
    output.values["LANG"] = "C.UTF-8";
    output.values["TZ"] = "UTC";
    output.values["SOURCE_DATE_EPOCH"] = std::to_string(source_time.time_since_epoch().count());
    return output;
}

} // namespace liquid::sandbox_detail

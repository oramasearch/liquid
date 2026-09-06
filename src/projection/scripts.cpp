#include "liquid/projection.hpp"

#include <format>
#include <ranges>

namespace liquid {
namespace {

auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto item : value) {
        if (item == '\'') {
            output += "'\\''";
        } else {
            output.push_back(item);
        }
    }
    output.push_back('\'');
    return output;
}

auto command_guard(std::string_view command, std::string arguments) -> std::string {
    auto invocation = std::string{command};
    if (!arguments.empty()) {
        invocation += " " + std::move(arguments);
    }
    return "if command -v " + shell_quote(command) + " >/dev/null 2>&1; then\n    " + invocation +
           "\nfi\n";
}

auto systemd_guard(std::string command) -> std::string {
    return "if [ -d /run/systemd/system ]; then\n    systemctl " + std::move(command) + "\nfi\n";
}

auto cache_command(install_cache cache) -> std::string {
    switch (cache) {
    case install_cache::ldconfig:
        return command_guard("ldconfig", "");
    case install_cache::desktop_database:
        return command_guard("update-desktop-database", "-q");
    case install_cache::mime_database:
        return command_guard("update-mime-database", shell_quote("/usr/share/mime"));
    case install_cache::man_db:
        return command_guard("mandb", "-q");
    case install_cache::icon_cache:
        return command_guard("gtk-update-icon-cache",
                             "-q -t -f " + shell_quote("/usr/share/icons/hicolor"));
    case install_cache::systemd_daemon_reload:
        return systemd_guard("daemon-reload");
    case install_cache::fonts:
        return command_guard("fc-cache", "-f");
    }
    return {};
}

auto header() -> std::string {
    return "#!/bin/sh\nset -eu\n";
}

auto pre_install(const install_intent& intent) -> std::string {
    auto output = header();
    for (const auto& group : intent.groups) {
        output += "if ! getent group " + shell_quote(group.name) + " >/dev/null 2>&1; then\n";
        output += "    if command -v 'groupadd' >/dev/null 2>&1; then\n";
        output += "        groupadd ";
        if (group.system) {
            output += "--system ";
        }
        output += shell_quote(group.name) + "\n";
        output += "    elif command -v 'addgroup' >/dev/null 2>&1; then\n";
        output += "        addgroup ";
        if (group.system) {
            output += "-S ";
        }
        output += shell_quote(group.name) + "\n    fi\nfi\n";
    }
    for (const auto& user : intent.users) {
        output += "if ! getent passwd " + shell_quote(user.name) + " >/dev/null 2>&1; then\n";
        output += "    if command -v 'useradd' >/dev/null 2>&1; then\n";
        output += "        useradd ";
        if (user.system) {
            output += "--system ";
        }
        output += "--home-dir " + shell_quote(user.home.generic_string()) + " --no-create-home " +
                  shell_quote(user.name) + "\n";
        output += "    elif command -v 'adduser' >/dev/null 2>&1; then\n";
        output += "        adduser ";
        if (user.system) {
            output += "-S ";
        }
        output += "-H -h " + shell_quote(user.home.generic_string()) + " " +
                  shell_quote(user.name) + "\n    fi\nfi\n";
    }
    for (const auto& directory : intent.directories) {
        output += std::format("install -d -m '{:04o}' -o {} {}\n",
                              directory.mode,
                              shell_quote(directory.owner),
                              shell_quote(directory.path.generic_string()));
    }
    output += "exit 0\n";
    return output;
}

auto post_install(const install_intent& intent) -> std::string {
    auto output = header();
    for (const auto& symlink : intent.symlinks) {
        const auto path = shell_quote(symlink.path.generic_string());
        const auto target = shell_quote(symlink.target.generic_string());
        output += "if [ \"$(readlink " + path + " 2>/dev/null || true)\" != " + target +
                  " ]; then\n    ln -sfn " + target + " " + path + "\nfi\n";
    }
    for (const auto& alternative : intent.alternatives) {
        output += command_guard("update-alternatives",
                                "--install " + shell_quote("/usr/bin/" + alternative.name) + " " +
                                    shell_quote(alternative.name) + " " +
                                    shell_quote(alternative.path.generic_string()) + " " +
                                    std::to_string(alternative.priority));
    }
    for (const auto& service : intent.services) {
        output += systemd_guard("daemon-reload");
        output += systemd_guard("enable --now " + shell_quote(service.name + ".service"));
    }
    for (const auto cache : intent.caches) {
        output += cache_command(cache);
    }
    output += "exit 0\n";
    return output;
}

auto removal_test(projection_target target) -> std::string {
    switch (target) {
    case projection_target::deb:
        return "[ \"${1:-}\" = 'remove' ]";
    case projection_target::rpm:
        return "[ \"${1:-0}\" = '0' ]";
    case projection_target::apk:
        return "true";
    case projection_target::oci_image:
    case projection_target::ostree_commit:
    case projection_target::bootc_image:
    case projection_target::helm_chart:
        return "false";
    }
    return "false";
}

auto pre_remove(projection_target target, const install_intent& intent) -> std::string {
    auto output = header();
    output += "if " + removal_test(target) + "; then\n";
    for (const auto& service : intent.services) {
        output += "    if [ -d /run/systemd/system ]; then\n";
        output += "        systemctl disable --now " + shell_quote(service.name + ".service") +
                  " >/dev/null 2>&1 || true\n    fi\n";
    }
    for (const auto& alternative : intent.alternatives) {
        output += "    if command -v 'update-alternatives' >/dev/null 2>&1; then\n";
        output += "        update-alternatives --remove " + shell_quote(alternative.name) + " " +
                  shell_quote(alternative.path.generic_string()) + " >/dev/null 2>&1 || true\n";
        output += "    fi\n";
    }
    for (const auto& symlink : intent.symlinks) {
        const auto path = shell_quote(symlink.path.generic_string());
        const auto target = shell_quote(symlink.target.generic_string());
        output += "    if [ \"$(readlink " + path + " 2>/dev/null || true)\" = " + target +
                  " ]; then\n        rm -f " + path + "\n    fi\n";
    }
    output += "fi\nexit 0\n";
    return output;
}

auto post_remove(projection_target target, const install_intent& intent) -> std::string {
    auto output = header();
    output += "if " + removal_test(target) + "; then\n";
    for (const auto cache : intent.caches) {
        auto command = cache_command(cache);
        for (const auto line : std::views::split(command, '\n')) {
            if (!line.empty()) {
                output += "    " + std::string{line.begin(), line.end()} + "\n";
            }
        }
    }
    if (!intent.services.empty()) {
        output += "    if [ -d /run/systemd/system ]; then\n";
        output += "        systemctl daemon-reload\n    fi\n";
    }
    output += "fi\nexit 0\n";
    return output;
}

} // namespace

result<generated_maintainer_scripts> generate_maintainer_scripts(projection_target target,
                                                                 const install_intent& intent) {
    if (target != projection_target::deb && target != projection_target::rpm &&
        target != projection_target::apk) {
        return std::unexpected(error{errc::invalid_argument,
                                     "maintainer scripts require a package target",
                                     std::string{to_string(target)}});
    }
    auto valid = validate_install_intent(intent);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    return generated_maintainer_scripts{pre_install(intent),
                                        post_install(intent),
                                        pre_remove(target, intent),
                                        post_remove(target, intent)};
}

} // namespace liquid

#include "projection/projection.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace liquid::projection_detail {
namespace {

auto lower(std::string value) -> std::string {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

auto rpm_soname(std::string_view value, std::string_view architecture) -> std::string {
    return std::string{value} + (architecture == "x86_64" || architecture == "aarch64" ||
                                         architecture == "ppc64" || architecture == "ppc64le" ||
                                         architecture == "s390x" || architecture == "riscv64"
                                     ? "()(64bit)"
                                     : "");
}

auto deb_dependency(std::string_view soname) -> std::string {
    static const auto packages =
        std::map<std::string_view, std::string_view>{{"libc.so.6", "libc6"},
                                                     {"libdl.so.2", "libc6"},
                                                     {"libm.so.6", "libc6"},
                                                     {"libpthread.so.0", "libc6"},
                                                     {"librt.so.1", "libc6"},
                                                     {"libstdc++.so.6", "libstdc++6"},
                                                     {"libgcc_s.so.1", "libgcc-s1"}};
    const auto found = packages.find(soname);
    return found == packages.end() ? lower(std::string{soname}) : std::string{found->second};
}

auto arch_dependency(std::string_view soname) -> std::string {
    static const auto packages =
        std::map<std::string_view, std::string_view>{{"libc.so.6", "glibc"},
                                                     {"libdl.so.2", "glibc"},
                                                     {"libm.so.6", "glibc"},
                                                     {"libpthread.so.0", "glibc"},
                                                     {"librt.so.1", "glibc"},
                                                     {"libstdc++.so.6", "gcc-libs"},
                                                     {"libgcc_s.so.1", "gcc-libs"}};
    const auto found = packages.find(soname);
    return found == packages.end() ? lower(std::string{soname}) : std::string{found->second};
}

auto unique(std::vector<std::string> values) -> std::vector<std::string> {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

} // namespace

std::map<std::string, std::uint8_t> trace_output_layers(const build_trace& trace, digest target) {
    auto distance = std::map<std::string, std::uint64_t>{{to_string(target), 0}};
    for (auto pass = std::size_t{}; pass < trace.actions.size(); ++pass) {
        for (const auto& edge : trace.edges) {
            const auto dependent = distance.find(to_string(edge.dependent));
            if (dependent == distance.end()) {
                continue;
            }
            auto& value = distance[to_string(edge.dependency)];
            value = std::max(value, dependent->second + 1);
        }
    }
    auto output = std::map<std::string, std::uint8_t>{};
    for (const auto& action : trace.actions) {
        const auto found = distance.find(to_string(action.action));
        if (found == distance.end()) {
            continue;
        }
        const auto layer = static_cast<std::uint8_t>(found->second == 0   ? 2
                                                     : found->second == 1 ? 1
                                                                          : 0);
        for (auto path : action.outputs) {
            while (path.starts_with("./") || path.starts_with('/')) {
                path.erase(0, path.starts_with("./") ? 2 : 1);
            }
            output.insert_or_assign(std::move(path), layer);
        }
    }
    return output;
}

std::vector<std::string> dependencies(const prepared_projection& prepared,
                                      projection_target target) {
    auto output = std::vector<std::string>{};
    for (const auto& soname : prepared.needed) {
        switch (target) {
        case projection_target::deb:
        case projection_target::debian_source:
            output.push_back(deb_dependency(soname));
            break;
        case projection_target::rpm:
            output.push_back(rpm_soname(soname, prepared.metadata.architecture));
            break;
        case projection_target::apk:
            output.push_back("so:" + soname);
            break;
        case projection_target::pkgbuild:
            output.push_back(arch_dependency(soname));
            break;
        default:
            break;
        }
    }
    output.insert(output.end(),
                  prepared.metadata.declared_dependencies.begin(),
                  prepared.metadata.declared_dependencies.end());
    return unique(std::move(output));
}

std::vector<std::string> provides(const prepared_projection& prepared, projection_target target) {
    auto output = prepared.metadata.provides;
    for (const auto& soname : prepared.sonames) {
        switch (target) {
        case projection_target::deb:
        case projection_target::debian_source:
            output.push_back(lower(soname));
            break;
        case projection_target::rpm:
            output.push_back(rpm_soname(soname, prepared.metadata.architecture));
            break;
        case projection_target::apk:
            output.push_back("so:" + soname);
            break;
        default:
            break;
        }
    }
    return unique(std::move(output));
}

} // namespace liquid::projection_detail

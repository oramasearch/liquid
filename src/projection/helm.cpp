#include "projection/oci.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "formats/deflate/deflate.hpp"
#include "formats/package/common.hpp"

namespace liquid::projection_detail {
namespace {

auto yaml_quote(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '\"') {
            output.push_back('\\');
        }
        output.push_back(character);
    }
    output.push_back('\"');
    return output;
}

auto service_name_is_valid(std::string_view value) -> bool {
    return !value.empty() && value.size() <= 63 && value.front() != '-' && value.back() != '-' &&
           std::ranges::all_of(value, [](unsigned char character) {
               return std::islower(character) || std::isdigit(character) || character == '-';
           });
}

auto scalar_is_valid(std::string_view value) -> bool {
    return value.find_first_of("\r\n") == std::string_view::npos &&
           value.find('\0') == std::string_view::npos;
}

auto image_repository_is_valid(std::string_view value) -> bool {
    if (value.empty() || !scalar_is_valid(value) || value.find('@') != std::string_view::npos) {
        return false;
    }
    const auto last_slash = value.rfind('/');
    const auto last_colon = value.rfind(':');
    return last_colon == std::string_view::npos ||
           (last_slash != std::string_view::npos && last_colon < last_slash);
}

auto service_is_valid(const projection_service& service) -> bool {
    if (!service_name_is_valid(service.name) ||
        !image_repository_is_valid(service.image_repository) ||
        service.image.alg != algorithm::sha256 ||
        std::ranges::any_of(service.ports, [](auto port) { return port == 0; }) ||
        !std::ranges::all_of(service.volumes, scalar_is_valid)) {
        return false;
    }
    return std::ranges::all_of(service.env, [](const auto& value) {
        return scalar_is_valid(value.first) && scalar_is_valid(value.second);
    });
}

auto chart_files(const prepared_projection& prepared, std::span<const projection_service> services)
    -> result<std::vector<formats::package::file>> {
    if (services.empty()) {
        return std::unexpected(error{
            errc::invalid_argument, "Helm projection requires at least one service", "--service"});
    }
    auto chart = std::string{"apiVersion: v2\nname: "} + yaml_quote(prepared.metadata.name) +
                 "\ndescription: " + yaml_quote(prepared.metadata.description) +
                 "\ntype: application\nversion: " + yaml_quote(prepared.metadata.version) +
                 "\nappVersion: " + yaml_quote(prepared.metadata.version) + "\n";
    auto values = std::string{"services:\n"};
    for (const auto& service : services) {
        if (!service_is_valid(service)) {
            return std::unexpected(
                error{errc::invalid_argument,
                      "Helm service requires a name, repository, and SHA-256 image",
                      service.name});
        }
        values += "  " + yaml_quote(service.name) +
                  ":\n    image:\n      repository: " + yaml_quote(service.image_repository) +
                  "\n      digest: " + yaml_quote(to_string(service.image)) + "\n    ports:";
        if (service.ports.empty()) {
            values += " []\n";
        } else {
            values += "\n";
            for (const auto port : service.ports) {
                values += "      - " + std::to_string(port) + "\n";
            }
        }
        values += "    env:";
        if (service.env.empty()) {
            values += " {}\n";
        } else {
            values += "\n";
            for (const auto& [name, value] : service.env) {
                values += "      " + yaml_quote(name) + ": " + yaml_quote(value) + "\n";
            }
        }
        values += "    volumes:";
        if (service.volumes.empty()) {
            values += " []\n";
        } else {
            values += "\n";
            for (const auto& volume : service.volumes) {
                values += "      - " + yaml_quote(volume) + "\n";
            }
        }
    }
    const auto deployment = R"({{- range $name, $service := .Values.services }}
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: {{ $name }}
spec:
  replicas: 1
  selector:
    matchLabels:
      app.kubernetes.io/name: {{ $name }}
  template:
    metadata:
      labels:
        app.kubernetes.io/name: {{ $name }}
    spec:
      containers:
        - name: {{ $name }}
          image: "{{ $service.image.repository }}@{{ $service.image.digest }}"
          imagePullPolicy: IfNotPresent
          {{- with $service.ports }}
          ports:
            {{- range . }}
            - containerPort: {{ . }}
            {{- end }}
          {{- end }}
          envFrom:
            - configMapRef:
                name: {{ $name }}
          {{- with $service.volumes }}
          volumeMounts:
            {{- range $index, $path := . }}
            - name: volume-{{ $index }}
              mountPath: {{ $path | quote }}
            {{- end }}
          {{- end }}
      {{- with $service.volumes }}
      volumes:
        {{- range $index, $_ := . }}
        - name: volume-{{ $index }}
          emptyDir: {}
        {{- end }}
      {{- end }}
{{- end }}
)";
    const auto service = R"({{- range $name, $service := .Values.services }}
{{- if $service.ports }}
---
apiVersion: v1
kind: Service
metadata:
  name: {{ $name }}
spec:
  selector:
    app.kubernetes.io/name: {{ $name }}
  ports:
    {{- range $service.ports }}
    - port: {{ . }}
      targetPort: {{ . }}
    {{- end }}
{{- end }}
{{- end }}
)";
    const auto config = R"({{- range $name, $service := .Values.services }}
---
apiVersion: v1
kind: ConfigMap
metadata:
  name: {{ $name }}
data:
  {{- if $service.env }}
  {{- range $key, $value := $service.env }}
  {{ $key }}: {{ $value | quote }}
  {{- end }}
  {{- else }} {}
  {{- end }}
{{- end }}
)";
    return std::vector<formats::package::file>{
        {"Chart.yaml", formats::package::text_bytes(chart), 0644},
        {"values.yaml", formats::package::text_bytes(values), 0644},
        {"templates/deployment.yaml", formats::package::text_bytes(deployment), 0644},
        {"templates/service.yaml", formats::package::text_bytes(service), 0644},
        {"templates/configmap.yaml", formats::package::text_bytes(config), 0644}};
}

auto materialise_chart(const std::filesystem::path& root,
                       std::span<const formats::package::file> files) -> result<void> {
    for (const auto& file : files) {
        auto written = write_file(root / file.path, file.bytes);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    return {};
}

} // namespace

runtime::task<projection_report> project_helm(repository& source,
                                              prepared_projection prepared,
                                              std::span<const projection_service> services) {
    auto built = co_await runtime::offload(
        [prepared, services = std::vector<projection_service>{services.begin(), services.end()}]()
            -> result<
                std::tuple<std::filesystem::path, digest, std::filesystem::path, oci_descriptor>> {
            auto files = chart_files(prepared, services);
            if (!files) {
                return std::unexpected(files.error());
            }
            const auto chart_root = prepared.options.output_directory / prepared.metadata.name;
            auto materialised = materialise_chart(chart_root, *files);
            if (!materialised) {
                return std::unexpected(materialised.error());
            }
            auto chart_tar = formats::package::tar(
                *files, prepared.options.source_date_epoch, prepared.metadata.name);
            if (!chart_tar) {
                return std::unexpected(chart_tar.error());
            }
            auto chart_digest = formats::package::sha256(*chart_tar);
            if (!chart_digest) {
                return std::unexpected(chart_digest.error());
            }
            const auto layout_root =
                prepared.options.output_directory / (prepared.metadata.name + "-chart.oci");
            auto config_json = std::string{"{\"name\":"} + yaml_quote(prepared.metadata.name) +
                               ",\"version\":" + yaml_quote(prepared.metadata.version) + "}";
            auto config = oci_blob(layout_root,
                                   formats::package::text_bytes(config_json),
                                   "application/vnd.cncf.helm.config.v1+json");
            if (!config) {
                return std::unexpected(config.error());
            }
            auto compressed = formats::deflate::gzip(*chart_tar);
            auto layer = oci_blob(
                layout_root, compressed, "application/vnd.cncf.helm.chart.content.v1.tar+gzip");
            if (!layer) {
                return std::unexpected(layer.error());
            }
            auto manifest = oci_manifest(layout_root,
                                         *config,
                                         std::span{&*layer, 1},
                                         {},
                                         "application/vnd.cncf.helm.config.v1+json");
            if (!manifest) {
                return std::unexpected(manifest.error());
            }
            manifest->annotations["org.opencontainers.image.ref.name"] =
                prepared.metadata.name + ":" + prepared.metadata.version;
            return std::tuple{chart_root, *chart_digest, layout_root, *manifest};
        });
    if (!built) {
        co_return std::unexpected(built.error());
    }
    if (!*built) {
        co_return std::unexpected(built->error());
    }
    auto& [chart_root, chart_digest, layout_root, manifest] = **built;
    auto chart_statement = co_await store_projection_attestation(
        source, prepared, projection_target::helm_chart, chart_digest);
    auto oci_statement = co_await store_projection_attestation(
        source, prepared, projection_target::helm_chart, manifest.content);
    if (!chart_statement || !oci_statement) {
        co_return std::unexpected(chart_statement ? oci_statement.error()
                                                  : chart_statement.error());
    }
    auto referrers =
        co_await attach_attestations(source, prepared, layout_root, manifest, *oci_statement);
    if (!referrers) {
        co_return std::unexpected(referrers.error());
    }
    auto indexed = co_await runtime::offload([layout_root, manifest, referrers = *referrers] {
        return write_oci_index(layout_root, std::span{&manifest, 1}, referrers);
    });
    if (!indexed) {
        co_return std::unexpected(indexed.error());
    }
    if (!*indexed) {
        co_return std::unexpected(indexed->error());
    }
    auto report = projection_report{
        .artifacts = {{chart_root, chart_digest, "application/vnd.cncf.helm.chart.directory.v1"},
                      {layout_root, manifest.content, manifest.media_type}},
        .attestations = prepared.source_attestations,
        .pinned = {{prepared.metadata.name, manifest.content}},
        .abi_break_allowed = prepared.abi_break_allowed};
    report.attestations.push_back(*chart_statement);
    report.attestations.push_back(*oci_statement);
    for (const auto& service : services) {
        report.pinned.push_back({service.name, service.image});
    }
    co_return report;
}

} // namespace liquid::projection_detail

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "cli/metadata.hpp"
#include "liquid/liquid.hpp"

namespace liquid_cli {

auto set_executable(std::filesystem::path path) -> void;
auto frontend_module_path(bool compiler) -> std::filesystem::path;

auto repo_init(int argc, char** argv) -> int;
auto repo_status(int argc, char** argv) -> int;
auto repo_write(int argc, char** argv) -> int;
auto repo_log(int argc, char** argv) -> int;
auto repo_cut(int argc, char** argv) -> int;
auto repo_refs(int argc, char** argv) -> int;
auto diff(int argc, char** argv) -> int;
auto tree_move(int argc, char** argv) -> int;
auto tree_remove(int argc, char** argv) -> int;
auto anchor(int argc, char** argv) -> int;
auto compose_nest(int argc, char** argv) -> int;
auto compose_split(int argc, char** argv) -> int;
auto compose_combine(int argc, char** argv) -> int;
auto capture_pause(int argc, char** argv) -> int;
auto capture_resume(int argc, char** argv) -> int;
auto capture_watch(int argc, char** argv) -> int;
auto history_undo(int argc, char** argv) -> int;
auto history_operations(int argc, char** argv) -> int;
auto path_history(int argc, char** argv) -> int;
auto locks_lock(int argc, char** argv) -> int;
auto locks_unlock(int argc, char** argv) -> int;
auto identity(int argc, char** argv) -> int;
auto keys(int argc, char** argv) -> int;
auto vfs_mount(int argc, char** argv) -> int;
auto vfs_materialise(int argc, char** argv) -> int;
auto garbage_collect(int argc, char** argv) -> int;
auto retain(int argc, char** argv) -> int;
auto attestation(int argc, char** argv) -> int;
auto verify(int argc, char** argv) -> int;
auto replay(int argc, char** argv) -> int;
auto provenance(int argc, char** argv) -> int;
auto policy(int argc, char** argv) -> int;
auto sbom(int argc, char** argv) -> int;
auto wrap(int argc, char** argv) -> int;
auto why(int argc, char** argv) -> int;
auto cache(int argc, char** argv) -> int;
auto git_import(int argc, char** argv) -> int;
auto git_export(int argc, char** argv) -> int;
auto build(int argc, char** argv) -> int;
auto test(int argc, char** argv) -> int;
auto why_slow(int argc, char** argv) -> int;
auto facts(int argc, char** argv) -> int;
auto delta(int argc, char** argv) -> int;
auto sync_push(int argc, char** argv) -> int;
auto sync_pull(int argc, char** argv) -> int;
auto serve(int argc, char** argv) -> int;
auto daemon_command(int argc, char** argv) -> int;
auto data(int argc, char** argv) -> int;
auto drive(int argc, char** argv) -> int;
auto scheduler(int argc, char** argv) -> int;
auto worker(int argc, char** argv) -> int;
auto node_store(int argc, char** argv) -> int;
auto search(int argc, char** argv) -> int;
auto diff(int argc, char** argv) -> int;
auto symbols(int argc, char** argv) -> int;
auto secret(int argc, char** argv) -> int;
auto abi(int argc, char** argv) -> int;
auto hot_update(int argc, char** argv) -> int;
auto edit(int argc, char** argv) -> int;
auto resolve(int argc, char** argv) -> int;
auto project(int argc, char** argv) -> int;
auto review(int argc, char** argv) -> int;
auto deploy(int argc, char** argv) -> int;

} // namespace liquid_cli

namespace {

using liquid_cli::argument_policy;
using liquid_cli::command;
using liquid_cli::flag;

constexpr auto flag_json = std::to_array<flag>({
    {"--json", {}, "Write stable JSON instead of text.", "false"},
});
constexpr auto flags_diff = std::to_array<flag>({
    {"--text", {}, "Include textual line-difference details.", "false"},
    {"--formats", {}, "Inspect supported structured container formats.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
});
constexpr auto flags_write = std::to_array<flag>({
    {"--from", "<file>", "Read the new content from this file.", "none"},
    {"--base",
     "<change>[@<cursor>]",
     "State the change and optional journal cursor read by the caller.",
     "none"},
    {"--depends-on", "<path>", "Add a path read while planning; repeatable.", "none"},
    {"--on-stale", "<policy>", "Choose refuse, disjoint, or force stale-write handling.", "refuse"},
});
constexpr auto flags_daemon = std::to_array<flag>({
    {"--shutdown", {}, "Stop the checkout daemon.", "false"},
    {"--serve", "<checkout>", "Run the internal daemon service for a checkout.", "none"},
    {"--idle-ms", "<count>", "Exit the service after this idle interval.", "configured value"},
    {"--memory", "<bytes>", "Set the resident graph-value byte ceiling.", "configured value"},
    {"--validation-rate", "<count>", "Set the clean-validation sampling rate.", "configured value"},
    {"--validation-ms", "<count>", "Set the idle validation interval.", "configured value"},
});
constexpr auto flags_search = std::to_array<flag>({
    {"--explain", {}, "Print the executed search plan as JSON.", "false"},
    {"--ref", "<name>", "Search a named reference or change digest.", "working tree"},
    {"--limit", "<count>", "Limit results to a value from 1 through 1000.", "20"},
});
constexpr auto flags_resolve = std::to_array<flag>({
    {"--universe", "<path>", "Read package candidates from this directory.", "current directory"},
    {"--reuse", {}, "Reuse the existing resolution when it remains valid.", "false"},
    {"--explain", {}, "Show the selected candidates and rejected constraints.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--why", "<package>", "Explain the constraints selecting a package.", "none"},
});
constexpr auto flags_log = std::to_array<flag>({
    {"--ops", {}, "Show signed drips and operation metadata.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--timestamps", {}, "Verify timestamp evidence after the verify subcommand.", "false"},
});
constexpr auto flags_atomic = std::to_array<flag>({
    {"--atomic", {}, "Refuse every reference update when any update is refused.", "false"},
});
constexpr auto flags_history = std::to_array<flag>({
    {"--restore", "<change>", "Restore the path from the named change.", "none"},
});
constexpr auto flags_drive = std::to_array<flag>({
    {"--watch", {}, "Continue synchronizing after the first successful transfer.", "false"},
});
constexpr auto flags_project = std::to_array<flag>({
    {"--name", "<name>", "Set the package, image, or chart name.", "required"},
    {"--version", "<version>", "Set the package, image, or chart version.", "required"},
    {"--arch", "<arch>", "Set the target architecture.", "source architecture"},
    {"--out", "<dir>", "Write projected artifacts below this directory.", "projection"},
    {"--depends", "<dependency>", "Declare a package dependency; repeatable.", "none"},
    {"--service", "<value>", "Add a Helm service declaration; repeatable.", "none"},
    {"--compat-gzip", {}, "Use gzip-compatible package compression.", "false"},
    {"--allow-abi-break", {}, "Permit an incompatible ABI projection.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
});
constexpr auto flags_data = std::to_array<flag>({
    {"--session",
     "<id>",
     "Select the session to branch for an application database.",
     "new session"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
});
constexpr auto flags_deploy = std::to_array<flag>({
    {"--control",
     "<url>",
     "Select the deployment control-plane endpoint.",
     "repository configuration"},
    {"--env", "<name>", "Select the deployment environment.", "production"},
    {"--image", "<ref@sha256:...>", "Deploy this image reference.", "none"},
    {"--to", "<change|deployment>", "Roll back to this recorded change or deployment.", "none"},
    {"--wait", {}, "Return failure when the created deployment is not ready.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
});
constexpr auto flags_serve = std::to_array<flag>({
    {"--listen", "<address>", "Select the listener address.", "127.0.0.1"},
    {"--port", "<port>", "Select the sync listener port.", "8080"},
    {"--allow-ref", "<glob>", "Permit a reference-name glob; repeatable.", "all references"},
    {"--instance", "<name>", "Select the REAPI instance name.", "empty"},
    {"--insecure-http", {}, "Serve admission requests without TLS.", "false"},
    {"--policy", "<file>", "Read the admission policy from this file.", "none"},
    {"--policies", "<dir>", "Read admission policies from this directory.", "none"},
    {"--control-listen", "<address>", "Select the admission control listener.", "none"},
    {"--tls-cert", "<path>", "Read the admission TLS certificate from this file.", "none"},
    {"--tls-key", "<path>", "Read the admission TLS private key from this file.", "none"},
    {"--attestation-source", "<url>", "Read admission attestations from this source.", "none"},
    {"--store", "<path>", "Read admission attestations from this store.", "none"},
    {"--log-cache", "<path>", "Store admission log cache data at this path.", "none"},
    {"--decision-replay",
     "<count>",
     "Retain this many recent admission decisions for replay.",
     "implementation default"},
});
constexpr auto flags_keys = std::to_array<flag>({
    {"--to", "<custody-uri>", "Rewrap a data key for this custody URI.", "none"},
    {"--threshold", "<count>", "Set the number of shares required to recover a key.", "none"},
    {"--shares", "<count>", "Set the number of custody shares to create.", "none"},
});
constexpr auto flags_attest = std::to_array<flag>({
    {"--tree", "<digest>", "Bind an image attestation to this repository tree.", "none"},
    {"--result", "<id>", "Bind an image attestation to this build result.", "none"},
    {"--store", "<path>", "Read or write attestations in this store.", "repository store"},
});
constexpr auto flags_to = std::to_array<flag>({
    {"--to", "<change>", "Select the target change.", "none"},
});
constexpr auto flags_at = std::to_array<flag>({
    {"--at", "<path>", "Place the combined repository at this path.", "repository root"},
});
constexpr auto flags_undo = std::to_array<flag>({
    {"--session", "<id>", "Undo operations from the named session.", "none"},
    {"--path", "<path>", "Limit a session undo to a path; repeatable.", "all session paths"},
    {"--drip", "<id>", "Limit a session undo to a drip; repeatable.", "all session drips"},
});
constexpr auto flags_mount = std::to_array<flag>({
    {"--read-only", {}, "Deny writes through the mounted working tree.", "false"},
});
constexpr auto flags_gc = std::to_array<flag>({
    {"--dry-run", {}, "Report collection without removing data.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--explain", {}, "Explain the roots retaining live objects.", "false"},
    {"--compact", {}, "Compact eligible packs after collection.", "false"},
    {"--compact-below",
     "<fraction>",
     "Compact packs below this live-byte fraction.",
     "implementation default"},
});
constexpr auto flags_verify = std::to_array<flag>({
    {"--graph", {}, "Cleanly validate persisted query values before actions.", "false"},
    {"--all", {}, "Validate every eligible value or action.", "sample"},
    {"--changed", {}, "Validate results without a recorded verification.", "false"},
    {"--explain", {}, "Include first-difference details.", "false"},
});
constexpr auto flags_replay = std::to_array<flag>({
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--record", {}, "Record the run as a verification sample.", "false"},
});
constexpr auto flags_sbom = std::to_array<flag>({
    {"--spdx", {}, "Write an SPDX 2.3 document.", "false"},
    {"--cyclonedx", {}, "Write a CycloneDX 1.5 document.", "false"},
});
constexpr auto flags_dsse = std::to_array<flag>({
    {"--dsse", {}, "Write the in-toto/DSSE projection.", "false"},
});
constexpr auto flags_wrap = std::to_array<flag>({
    {"--jobs",
     "<count>",
     "Set driver parallelism and compilation-database width.",
     "runtime default"},
    {"--target", "<triple>", "Select configure facts for a target triple.", "host target"},
    {"--install-prefix",
     "<absolute-path>",
     "Rewrite install-tree paths for this destination prefix.",
     "none"},
    {"--allow-embedded-prefix",
     {},
     "Allow artifacts that retain the configured install prefix.",
     "false"},
    {"--input", "<name>=<result|tree>", "Bind a named result or tree input; repeatable.", "none"},
    {"--remote", "<endpoint>", "Use a stock REAPI remote endpoint.", "none"},
    {"--scheduler", "<endpoint>", "Use a liquid scheduler endpoint.", "none"},
    {"--remote-instance", "<name>", "Select the remote instance.", "empty"},
    {"--remote-header", "<name=value>", "Add a remote request header; repeatable.", "none"},
    {"--remote-tls", {}, "Enable TLS for the remote connection.", "false"},
    {"--remote-no-verify", {}, "Disable remote TLS certificate verification.", "false"},
    {"--remote-ca", "<path>", "Use a certificate-authority file.", "system trust"},
    {"--remote-client-cert", "<path>", "Use a client certificate file.", "none"},
    {"--remote-client-key", "<path>", "Use a client private-key file.", "none"},
    {"--remote-publish", {}, "Publish results to the configured remote.", "false"},
    {"--remote-deadline-ms",
     "<count>",
     "Set the remote request deadline.",
     "implementation default"},
    {"--no-cache", {}, "Execute without publishing cache entries.", "false"},
    {"--no-verify", {}, "Skip background verification sampling.", "false"},
    {"--explain", {}, "Print recorded cache-miss details.", "false"},
    {"--fail-on-abi-break", {}, "Return failure when a rebuilt library is incompatible.", "false"},
});
constexpr auto flags_edit = std::to_array<flag>({
    {"--apply", {}, "Apply the planned edit.", "false"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--value", {}, "Use a value selection for an extraction.", "false"},
    {"--agent", "<name>", "Record the named agent as the edit author.", "none"},
    {"--at", "<line>", "Insert or move content at this one-based line.", "none"},
});
constexpr auto flags_secret = std::to_array<flag>({
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--lake", "<url>", "Fetch a recipient public key from this Lake endpoint.", "none"},
    {"--public-key", "<hex>", "Use this recipient public key.", "none"},
    {"--kid", "<id>", "Use this recipient key identifier.", "none"},
    {"--cluster", "<name>", "Bind the secret to this cluster.", "none"},
    {"--namespace", "<name>", "Bind the secret to this namespace.", "default"},
    {"--name", "<name>", "Bind the secret to this name.", "none"},
    {"--value", "<text>", "Seal this text value.", "none"},
    {"--value-file", "<path>", "Seal the contents of this file.", "none"},
    {"--out", "<file>", "Write a sealed secret record to this file.", "standard output"},
    {"--private-key-file", "<path>", "Read the unsealing private key from this file.", "none"},
});
constexpr auto flags_review = std::to_array<flag>({
    {"--lake", "<url>", "Select the Lake repository that owns the review.", "none"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--state", "<open|approved|closed>", "Limit list results to this review state.", "all"},
});
constexpr auto flags_hot_update = std::to_array<flag>({
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--endpoint", "<path>", "Select the local update endpoint for apply.", "none"},
});
constexpr auto flags_node_store = std::to_array<flag>({
    {"--root", "<path>", "Store node chunks below this directory.", "LIQUID_NODE_STORE_ROOT"},
    {"--listen", "<unix://path>", "Select the node-store service listener.", "none"},
    {"--control-listen", "<address>", "Select the node-store control listener.", "none"},
    {"--budget-bytes", "<count>", "Set the node-cache byte budget.", "implementation default"},
    {"--control", "<address>", "Select the node-store control endpoint.", "none"},
    {"--json", {}, "Write stable JSON instead of text.", "false"},
    {"--socket", "<path>", "Select the mount socket path.", "none"},
    {"--store", "<url>", "Select the node chunk store endpoint.", "none"},
});
constexpr auto flags_build = std::to_array<flag>({
    {"--plan", {}, "Print the configured action plan without executing.", "false"},
    {"--events", {}, "Run a child command and write JSON Lines build events.", "false"},
    {"--profile", {}, "Run a child command through the observed wrapper.", "false"},
    {"--chrome", {}, "Write the latest trace as Chrome trace-event JSON.", "false"},
    {"--jobs",
     "<count>",
     "Set driver parallelism and compilation-database width.",
     "runtime default"},
    {"--target", "<triple>", "Select configure facts for a target triple.", "host target"},
    {"--remote", "<endpoint>", "Use a stock REAPI remote endpoint.", "none"},
    {"--scheduler", "<endpoint>", "Use a liquid scheduler endpoint.", "none"},
    {"--remote-instance", "<name>", "Select the remote instance.", "empty"},
    {"--remote-header", "<name=value>", "Add a remote request header; repeatable.", "none"},
    {"--remote-tls", {}, "Enable TLS for the remote connection.", "false"},
    {"--remote-no-verify", {}, "Disable remote TLS certificate verification.", "false"},
    {"--remote-ca", "<path>", "Use a certificate-authority file.", "system trust"},
    {"--remote-client-cert", "<path>", "Use a client certificate file.", "none"},
    {"--remote-client-key", "<path>", "Use a client private-key file.", "none"},
    {"--remote-publish", {}, "Publish results to the configured remote.", "false"},
    {"--remote-deadline-ms",
     "<count>",
     "Set the remote request deadline.",
     "implementation default"},
    {"--no-cache", {}, "Execute without publishing cache entries.", "false"},
    {"--no-verify", {}, "Skip background verification sampling.", "false"},
    {"--explain", {}, "Print recorded cache-miss details.", "false"},
    {"--fail-on-abi-break", {}, "Return failure when a rebuilt library is incompatible.", "false"},
});
constexpr auto flags_scheduler = std::to_array<flag>({
    {"--listen", "<address>", "Select the REAPI and worker listener.", "none"},
    {"--control-listen", "<address>", "Select the control listener.", "127.0.0.1:8080"},
    {"--store", "<path>", "Select the scheduler store directory.", "platform cache"},
    {"--instance", "<name>", "Select the scheduler instance.", "empty"},
    {"--lease-ms", "<count>", "Set the worker lease duration.", "configured value"},
});
constexpr auto flags_worker = std::to_array<flag>({
    {"--scheduler", "<address>", "Select the scheduler endpoint.", "none"},
    {"--control-listen", "<address>", "Select the control listener.", "127.0.0.1:8081"},
    {"--root", "<path>", "Select the worker state directory.", "platform cache"},
    {"--instance", "<name>", "Select the scheduler instance.", "empty"},
    {"--size-class", "<class>", "Choose tiny, short, medium, or long.", "medium"},
    {"--poll-ms", "<count>", "Set the scheduler polling interval.", "configured value"},
});
constexpr auto flags_docs = std::to_array<flag>({
    {"--format", "<markdown|json|man>", "Select the output format.", "markdown"},
    {"--out", "<dir>", "Write generated files below this directory.", "standard output"},
});

constexpr auto normal_exit = "0 on success, 1 on an operation failure, and 2 for invalid usage.";
constexpr auto information_exit = "0 when the information was written and 2 for invalid usage.";

constexpr auto commands = std::to_array<command>({
    {"version",
     "version [--json]",
     "print the version",
     "available",
     flag_json,
     "Print the libliquid version used by this binary.",
     "liquid version --json",
     information_exit,
     argument_policy::ordinary,
     nullptr},
    {"init",
     "init [path]",
     "create a repository and import its regular files",
     "available",
     {},
     "Create a repository and import the regular files already present at the target path.",
     "liquid init .",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::repo_init},
    {"status",
     "status [--json]",
     "show repository status",
     "available",
     flag_json,
     "Show repository, capture, daemon, conflict, and remote-byte state.",
     "liquid status --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::repo_status},
    {"data",
     "data branch <database> [--session <id>] | data list [--json] | data <discard|promote> <id>",
     "manage session database branches",
     "available with PostgreSQL client tools",
     flags_data,
     "Create, list, discard, or promote PostgreSQL branches bound to repository sessions.",
     "liquid data branch application --session 0123456789abcdef0123456789abcdef",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::data},
    {"write",
     "write <path> --from <file> --base <change>[@<cursor>] [--depends-on <path>]... "
     "[--on-stale refuse|disjoint|force]",
     "write a file from a stated base",
     "available",
     flags_write,
     "Write a file while recording the exact repository state used to plan the edit.",
     "liquid write notes.txt --from /tmp/notes.txt --base blake3:<change>",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::repo_write},
    {"daemon",
     "daemon [--shutdown] | daemon --serve <checkout> [service options]",
     "start or stop the checkout daemon",
     "available on macOS and Linux",
     flags_daemon,
     "Start, stop, or run the per-checkout process that owns capture and graph work.",
     "liquid daemon --shutdown",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::daemon_command},
    {"search",
     "search [--explain] <query> [--ref <name>] [--limit <count>]",
     "search paths and contents",
     "available",
     flags_search,
     "Search ranked text, exact substrings, or regular expressions in the working tree or a ref.",
     "liquid search --explain '/query.*graph/'",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::search},
    {"resolve",
     "resolve <spec>... [--universe <path>] [--reuse] [--explain] [--json] | resolve --why "
     "<package> <spec>... [--universe <path>] [--reuse] [--json]",
     "concretise package specifications",
     "available",
     flags_resolve,
     "Resolve package constraints against an available candidate universe.",
     "liquid resolve example@^1.0 --universe packages",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::resolve},
    {"project",
     "project <oci|deb|rpm|apk|helm> <result|install-tree> --name <name> --version <version> "
     "[--arch <arch>] [--out <dir>] [--depends <dependency>] [--service <value>] "
     "[--compat-gzip] [--allow-abi-break] [--json]",
     "project a build into an installable package, image, or chart",
     "available",
     flags_project,
     "Project a recorded build result or install tree into an OCI image, package, or Helm chart.",
     "liquid project oci blake3:<result-digest> --name example --version 1.0.0 --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::project},
    {"log",
     "log [--ops] [--json] | log timestamps <log-url> | log verify --timestamps",
     "show or verify change, signed drip, or timestamp history",
     "available",
     flags_log,
     "Show named changes or the signed drips and operation metadata that produced them.",
     "liquid log --ops --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::repo_log},
    {"cut",
     "cut <message>",
     "cut the working change",
     "available",
     {},
     "Name the current aggregation of captured drips as a reviewable change.",
     "liquid cut \"import existing files\"",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::repo_cut},
    {"refs",
     "refs [--json]",
     "list repository references",
     "available",
     flag_json,
     "List the repository's named references and their change identifiers.",
     "liquid refs --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::repo_refs},
    {"diff",
     "diff <a> <b> [path] [--text] [--formats] [--json]",
     "compare changes and supported container formats",
     "available",
     flags_diff,
     "Compare two changes, optionally limit the result to a path, and inspect supported formats.",
     "liquid diff main feature --text --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::diff},
    {"history",
     "history <path> [--restore <change>]",
     "show or restore path history",
     "available",
     flags_history,
     "Show the changes affecting a path or restore its content from a selected change.",
     "liquid history notes.txt",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::path_history},
    {"drive",
     "drive <sync <folder> <remote> [--watch]|status [folder]|pin <path>|unpin <path>>",
     "synchronize a drive folder",
     "available",
     flags_drive,
     "Synchronize a local folder with a drive remote or report synchronization state.",
     "liquid drive sync ./documents https://example.invalid/drive --watch",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::drive},
    {"push",
     "push [--atomic] <url>",
     "push repository changes and objects",
     "available",
     flags_atomic,
     "Send signed metadata, requested references, and missing objects to a sync endpoint.",
     "liquid push http://127.0.0.1:8080",
     "0 when every reference applied, 1 when transport fails or a reference is refused, and 2 for "
     "invalid usage.",
     argument_policy::ordinary,
     liquid_cli::sync_push},
    {"pull",
     "pull <url>",
     "pull repository changes and objects",
     "available",
     {},
     "Receive signed metadata and missing objects from a sync endpoint.",
     "liquid pull http://127.0.0.1:8080",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::sync_pull},
    {"review",
     "review <list|show|open|comment|approve|close|protections|watch> --lake <url> [--json] ...",
     "review a lake change in place",
     "available",
     flags_review,
     "Open, inspect, comment on, approve, close, or watch a Lake review for a repository change.",
     "liquid review list --lake https://lake.example.invalid --state open --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::review},
    {"deploy",
     "deploy [--control <url>] [--env <name>] [--image <ref@sha256:...>] [--to "
     "<change|deployment>] "
     "[--wait] | deploy <status|list> [--control <url>] [--env <name>] [--json]",
     "create, inspect, or roll back an environment deployment",
     "available",
     flags_deploy,
     "Create, inspect, list, or roll back a deployment through the configured control plane.",
     "liquid deploy --env staging --image registry.example/app@sha256:<digest>",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::deploy},
    {"serve",
     "serve sync <path> [sync options] | serve reapi --listen <address> [--instance <name>] | "
     "serve admission <--policy <file>|--policies <dir>> [admission options]",
     "serve repository sync, REAPI, or image admission",
     "available",
     flags_serve,
     "Serve repository synchronization, the repository CAS and ActionCache, or image admission.",
     "liquid serve sync . --listen 127.0.0.1 --port 8080",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::serve},
    {"move",
     "move <from> <to>",
     "record a path move",
     "available",
     {},
     "Move a path while preserving its repository node identity.",
     "liquid move notes.txt archive/notes.txt",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::tree_move},
    {"rm",
     "rm <path>",
     "record a path removal",
     "available",
     {},
     "Remove a path and record the operation in repository history.",
     "liquid rm archive/notes.txt",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::tree_remove},
    {"anchor",
     "anchor <path>:<line>@<change> --to <change>",
     "map a text position between changes",
     "available",
     flags_to,
     "Follow a one-based text position through edits, moves, replacements, and deletion.",
     "liquid anchor src/main.cpp:42@blake3:<old> --to blake3:<new>",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::anchor},
    {"nest",
     "nest <repository> <path>",
     "nest another repository",
     "available",
     {},
     "Attach another repository beneath a path while retaining its history.",
     "liquid nest ../shared-assets vendor/assets",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::compose_nest},
    {"split",
     "split <path> <into>",
     "split a subtree into a repository",
     "available",
     {},
     "Create a repository from a subtree while retaining its history.",
     "liquid split src ../src-repository",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::compose_split},
    {"combine",
     "combine <repository> [--at <path>]",
     "combine another repository",
     "available",
     flags_at,
     "Combine another repository at the root or a selected path.",
     "liquid combine ../src-repository --at imported-src",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::compose_combine},
    {"ops",
     "ops [--json]",
     "list recorded operations",
     "available",
     flag_json,
     "List operation identifiers, sessions, paths, and operation kinds.",
     "liquid ops --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::history_operations},
    {"undo",
     "undo [op-id] | undo --session <id> [--path <path>]... [--drip <id>]...",
     "undo the newest, named, or session operations",
     "available",
     flags_undo,
     "Append inverse operations for one operation or a selected portion of a session.",
     "liquid undo --session <32-hex-id> --path src",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::history_undo},
    {"lock",
     "lock <path>",
     "advisory-lock a path",
     "available",
     {},
     "Record an advisory lock for a repository path.",
     "liquid lock src/main.cpp",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::locks_lock},
    {"unlock",
     "unlock <path>",
     "remove an advisory lock",
     "available",
     {},
     "Remove the caller's advisory lock from a repository path.",
     "liquid unlock src/main.cpp",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::locks_unlock},
    {"mount",
     "mount <mountpoint> [--read-only]",
     "mount a lazy working tree",
     "available where FUSE is available",
     flags_mount,
     "Mount the repository working tree and hydrate content when it is read.",
     "liquid mount /tmp/liquid-mount --read-only",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::vfs_mount},
    {"materialise",
     "materialise <target> [path]",
     "write a full working tree",
     "available",
     {},
     "Write all or part of the current tree into a destination directory.",
     "liquid materialise /tmp/liquid-checkout src",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::vfs_materialise},
    {"gc",
     "gc [--dry-run] [--json] [--explain] [--compact] [--compact-below <fraction>] [repository]",
     "checkpoint and collect a repository",
     "available",
     flags_gc,
     "Checkpoint reachable roots, collect unreachable objects, and optionally compact packs.",
     "liquid gc --dry-run --explain .",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::garbage_collect},
    {"import",
     "import <path> | import git <path>",
     "import a local tree or Git repository",
     "available",
     {},
     "Import reachable local Git history with its identity mapping.",
     "liquid import git ../source-repository",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::git_import},
    {"export",
     "export git <path> [ref]",
     "export as a Git repository",
     "available",
     {},
     "Write a standalone Git repository and report compatibility losses.",
     "liquid export git ../exported-repository main",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::git_export},
    {"retain",
     "retain explain <digest>",
     "explain retention for an object",
     "available",
     {},
     "Explain the rule and lease retaining an object or why it is due for eviction.",
     "liquid retain explain blake3:<object-digest>",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::retain},
    {"identity",
     "identity [show|rotate] [--json]",
     "show or rotate the device identity",
     "available",
     flag_json,
     "Show the device TAAP identity or rotate its signing key with a linked successor document.",
     "liquid identity rotate --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::identity},
    {"keys",
     "keys <show|rewrap|rotate|revoke|split> ...",
     "inspect and manage data-key custody",
     "available",
     flags_keys,
     "Inspect, rotate, revoke, rewrap, or split data-key custody material.",
     "liquid keys show",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::keys},
    {"attest",
     "attest <show|verify> <digest> | attest image <ref@sha256:...> --tree <digest> [...]",
     "show, verify, or mint an image attestation",
     "available",
     flags_attest,
     "Decode, verify, or mint a registered TAAP attestation from the current store.",
     "liquid attest verify blake3:<attestation-digest>",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::attestation},
    {"delta",
     "delta <create|apply|fetch> ...",
     "create, apply, or fetch an artifact delta",
     "available",
     {},
     "Create a signed zstd delta, apply one with digest checks, or fetch by target digest.",
     "liquid delta create release-1.tar release-2.tar",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::delta},
    {"verify",
     "verify <action|--all|--changed> [--explain] | verify --graph [--all] [--explain]",
     "re-execute and compare cached actions or the query graph",
     "available on Linux with FUSE and on macOS with macFUSE",
     flags_verify,
     "Cleanly recompute graph values or re-execute cached actions and quarantine divergence.",
     "liquid verify --graph --all --explain",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::verify},
    {"replay",
     "replay <action|result> [--json] [--record]",
     "reproduce and compare a recorded action",
     "available on Linux with FUSE and on macOS with macFUSE",
     flags_replay,
     "Reconstruct a recorded action, compare every result byte, and optionally record "
     "verification.",
     "liquid replay blake3:<result-digest> --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::replay},
    {"provenance",
     "provenance [--dsse] <result|path>",
     "print and verify build provenance",
     "available",
     flags_dsse,
     "Verify result-bound provenance and write its native or in-toto/DSSE projection.",
     "liquid provenance --dsse build/output.o",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::provenance},
    {"policy",
     "policy <check|explain> [target]",
     "check or explain build-input policy",
     "available",
     {},
     "Evaluate configured source and toolchain attestation requirements for cached work.",
     "liquid policy explain build/output.o",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::policy},
    {"sbom",
     "sbom <target> <--spdx|--cyclonedx>",
     "print a derived or explicitly inferred SBOM",
     "available",
     flags_sbom,
     "Verify and print a graph-derived SBOM or clearly label local binary inference.",
     "liquid sbom build/output.o --spdx",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::sbom},
    {"abi",
     "abi <show|diff> ... [--json]",
     "show or compare library ABIs",
     "available",
     flag_json,
     "Show a linked-library ABI descriptor or compare two descriptors for compatibility.",
     "liquid abi show build/libexample.so --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::abi},
    {"symbols",
     "symbols <path>",
     "show compiler or syntactic symbols for a source path",
     "available",
     {},
     "Show content-addressed symbols from a compiler record or the labelled syntactic fallback.",
     "liquid symbols src/main.cpp",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::symbols},
    {"hot-update",
     "hot-update <classify|apply|status> ...",
     "classify, apply, or inspect runtime updates",
     "available on macOS arm64 and Linux x86_64",
     flags_hot_update,
     "Classify a build update, deliver a signed patch, or inspect an artifact's update chain.",
     "liquid hot-update status blake3:<result> --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::hot_update},
    {"secret",
     "secret <public-key|seal|unseal> ...",
     "fetch, seal, or unseal a Lake environment secret",
     "available",
     flags_secret,
     "Fetch a recipient key, seal a scoped secret, or unseal a locally held secret record.",
     "liquid secret public-key --lake https://lake.example.invalid --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::secret},
    {"edit",
     "edit <rename|extract|move|apply> ... [--json] [--apply] [--agent <name>]",
     "preview or apply a structural edit",
     "available on macOS and Linux",
     flags_edit,
     "Plan or apply a structural rename, extraction, move, or edit operation.",
     "liquid edit rename src/main.cpp:42 old_name new_name --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::edit},
    {"wrap",
     "wrap [--jobs <count>] [--target <triple>] [--input <name>=<result|tree>] "
     "[--install-prefix <absolute-path>] [--allow-embedded-prefix] "
     "[--remote <endpoint>|--scheduler <endpoint>] "
     "[remote options] "
     "[--no-cache] [--no-verify] [--explain] [--fail-on-abi-break] <command> "
     "[args...]",
     "run an existing build as cached actions",
     "available on Linux with FUSE and on macOS with macFUSE",
     flags_wrap,
     "Run an unchanged build in the observed mount and reuse valid tool actions before they start.",
     "liquid wrap --jobs 8 --explain cmake --build build",
     normal_exit,
     argument_policy::child_command,
     liquid_cli::wrap},
    {"build",
     "build [target] | build --plan [target] | build --events <command> [args...] | "
     "build --profile [remote options] <command> [args...] | build trace [--chrome]",
     "plan or start a configured repository build, profile a command, stream events, or export a "
     "trace",
     "available on Linux with FUSE and on macOS with macFUSE",
     flags_build,
     "Plan or execute a configured graph, wrap a child command, stream events, or export a trace.",
     "liquid build --plan",
     normal_exit,
     argument_policy::build_command,
     liquid_cli::build},
    {"test",
     "test [filter]",
     "run discovered tests as cached actions",
     "available on Linux with FUSE and on macOS with macFUSE",
     {},
     "Run discovered CMake or native tests as cached build actions.",
     "liquid test quick",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::test},
    {"why",
     "why <target> [--json]",
     "explain the latest cache lookup",
     "available",
     flag_json,
     "Explain a recorded cache hit or miss without inferring from unrecorded checkout state.",
     "liquid why build/output.o --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::why},
    {"why-slow",
     "why-slow <target>",
     "show the critical path to a build target",
     "available",
     {},
     "Show the longest dependency path and phase timings ending at a target.",
     "liquid why-slow build/app",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::why_slow},
    {"cache",
     "cache stats [--json]",
     "show build-cache statistics",
     "available",
     flag_json,
     "Show cumulative disk and remote cache hits, misses, rates, and byte counts.",
     "liquid cache stats --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::cache},
    {"facts",
     "facts <list|show|export> [digest]",
     "inspect or export configure facts",
     "available",
     {},
     "Inspect content-keyed configure checks or export the shared fact bundle.",
     "liquid facts list",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::facts},
    {"scheduler",
     "scheduler --listen <address> [--control-listen <address>] [--store <path>] [--instance "
     "<name>] [--lease-ms <count>]",
     "run the remote-execution scheduler and CAS",
     "available",
     flags_scheduler,
     "Serve CAS, ActionCache, Execution, and the liquid worker lease queue.",
     "liquid scheduler --listen 0.0.0.0:50051",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::scheduler},
    {"worker",
     "worker --scheduler <address> [--control-listen <address>] [--root <path>] [--instance "
     "<name>] [--size-class <class>] [--poll-ms <count>]",
     "run a remote-execution worker",
     "available on Linux with FUSE",
     flags_worker,
     "Poll a liquid scheduler and execute leased build actions in the Phase 2 sandbox.",
     "liquid worker --scheduler 127.0.0.1:50051",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::worker},
    {"pause",
     "pause",
     "pause working-tree capture",
     "available",
     {},
     "Pause watcher-backed capture for the current checkout.",
     "liquid pause",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::capture_pause},
    {"resume",
     "resume",
     "resume working-tree capture",
     "available",
     {},
     "Resume watcher-backed capture for the current checkout.",
     "liquid resume",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::capture_resume},
    {"watch",
     "watch",
     "capture changes until Ctrl-C",
     "available",
     {},
     "Run foreground capture for the current checkout until interrupted.",
     "liquid watch",
     "0 after an orderly stop, 1 on an operation failure, and 2 for invalid usage.",
     argument_policy::ordinary,
     liquid_cli::capture_watch},
    {"docs",
     "docs [--format markdown|json|man] [--out <dir>]",
     "generate the CLI reference",
     "available",
     flags_docs,
     "Render the complete command and option reference from this binary's typed command table.",
     "liquid docs --format markdown --out docs/reference/cli",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::docs},
    {"node-store",
     "node-store <serve|status|mount|unmount|prefetch> [options]",
     "run or control the runtime node chunk store",
     "available on Linux with FUSE and on macOS with macFUSE",
     flags_node_store,
     "Run, inspect, or mount the runtime node chunk store.",
     "liquid node-store status --json",
     normal_exit,
     argument_policy::ordinary,
     liquid_cli::node_store},
});

auto print_usage() -> void {
    std::println("liquid — a version control system, build system, package manager,");
    std::println("artifact service and runtime in one program.");
    std::println("");
    std::println("usage: liquid <command> [options]");
    std::println("");
    std::println("commands:");
    for (const auto& command : commands) {
        const auto suffix = command.availability == "available"
                                ? std::string{}
                                : std::format(" ({})", command.availability);
        std::println("  {:{}} {}{}", command.name, 16, command.summary, suffix);
    }
}

auto usage_error(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

} // namespace

namespace liquid_cli {

auto command_table() -> std::span<const command> {
    return commands;
}

auto print_command_help(const command& definition) -> void {
    std::println("usage: liquid {}", definition.usage);
    std::println("");
    std::println("{}", definition.description);
    std::println("");
    std::println("availability: {}", definition.availability);
    if (!definition.flags.empty()) {
        std::println("");
        std::println("options:");
        for (const auto& option : definition.flags) {
            const auto spelling = option.argument.empty()
                                      ? std::string{option.name}
                                      : std::format("{} {}", option.name, option.argument);
            std::println(
                "  {:{}} {} (default: {})", spelling, 30, option.description, option.default_value);
        }
    }
    std::println("");
    std::println("exit status: {}", definition.exit_status);
}

} // namespace liquid_cli

int main(int argc, char** argv) {
    liquid_cli::set_executable(argv[0]);
    if (argc < 2) {
        print_usage();
        return 2;
    }
    auto command_name = std::string_view{argv[1]};
    if (command_name == "--help") {
        print_usage();
        return 0;
    }
    if (command_name == "help") {
        if (argc == 2) {
            print_usage();
            return 0;
        }
        if (argc != 3) {
            return usage_error("help accepts at most one command");
        }
        command_name = argv[2];
        const auto found = std::ranges::find(commands, command_name, &command::name);
        if (found == commands.end()) {
            return usage_error(std::format("unknown command '{}'", command_name));
        }
        liquid_cli::print_command_help(*found);
        return 0;
    }
    if (command_name == "--version") {
        command_name = "version";
    }
    const auto found = std::ranges::find(commands, command_name, &command::name);
    if (found == commands.end()) {
        return usage_error(std::format("unknown command '{}'", command_name));
    }
    if (argc == 3 && std::string_view{argv[2]} == "--help") {
        liquid_cli::print_command_help(*found);
        return 0;
    }
    const auto flag_error = liquid_cli::validate_flags(*found, argc - 2, argv + 2);
    if (!flag_error.empty()) {
        return usage_error(flag_error);
    }
    if (command_name == "version") {
        if (argc != 2 && !(argc == 3 && std::string_view{argv[2]} == "--json")) {
            return usage_error("version accepts only --json");
        }
        if (argc == 3) {
            std::println("{{\"version\":\"{}\",\"grammar\":\"{}\"}}",
                         liquid::version_string(),
                         liquid::grammar_digest());
            return 0;
        }
        std::println("liquid {}", liquid::version_string());
        return 0;
    }
    if (!found->handler) {
        return usage_error(std::format("{} {}", found->name, found->availability));
    }
    return found->handler(argc - 2, argv + 2);
}

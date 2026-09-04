#!/usr/bin/env python3

import hashlib
import os
from pathlib import Path
import sys

from build.bazel.remote.execution.v2 import remote_execution_pb2 as re
from build.bazel.semver import semver_pb2
from google.bytestream import bytestream_pb2
from google.longrunning import operations_pb2
from google.protobuf import any_pb2
from google.protobuf import duration_pb2
from google.protobuf import timestamp_pb2
from google.protobuf import wrappers_pb2
from google.rpc import status_pb2


def varint(value):
    output = bytearray()
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)
    return bytes(output)


def digest(seed, size=123):
    return re.Digest(hash=f"{seed:02x}" * 32, size_bytes=size)


def timestamp(seconds, nanos):
    return timestamp_pb2.Timestamp(seconds=seconds, nanos=nanos)


def duration(seconds, nanos):
    return duration_pb2.Duration(seconds=seconds, nanos=nanos)


def any_value(name, payload):
    return any_pb2.Any(type_url=f"type.googleapis.com/{name}", value=payload)


def node_properties(mode):
    return re.NodeProperties(
        properties=[re.NodeProperty(name="owner", value="liquid")],
        mtime=timestamp(1_700_000_000, 123_000_000),
        unix_mode=wrappers_pb2.UInt32Value(value=mode),
    )


def metadata():
    return re.ExecutedActionMetadata(
        worker="worker-01",
        queued_timestamp=timestamp(1_700_000_000, 1),
        worker_start_timestamp=timestamp(1_700_000_001, 2),
        worker_completed_timestamp=timestamp(1_700_000_010, 3),
        input_fetch_start_timestamp=timestamp(1_700_000_002, 4),
        input_fetch_completed_timestamp=timestamp(1_700_000_003, 5),
        execution_start_timestamp=timestamp(1_700_000_004, 6),
        execution_completed_timestamp=timestamp(1_700_000_008, 7),
        output_upload_start_timestamp=timestamp(1_700_000_008, 8),
        output_upload_completed_timestamp=timestamp(1_700_000_009, 9),
        auxiliary_metadata=[any_value("liquid.Manifest", b"manifest")],
        virtual_execution_duration=duration(4, 10),
    )


def action_result():
    return re.ActionResult(
        output_files=[
            re.OutputFile(
                path="out/file.o",
                digest=digest(2, 321),
                is_executable=True,
                contents=b"object",
                node_properties=node_properties(0o100755),
            )
        ],
        output_directories=[
            re.OutputDirectory(
                path="out/tree",
                tree_digest=digest(3, 222),
                is_topologically_sorted=True,
                root_directory_digest=digest(4, 111),
            )
        ],
        exit_code=-7,
        stdout_raw=b"stdout",
        stdout_digest=digest(5, 6),
        stderr_raw=b"stderr",
        stderr_digest=digest(6, 6),
        execution_metadata=metadata(),
        output_file_symlinks=[re.OutputSymlink(path="legacy-file", target="file.o")],
        output_directory_symlinks=[re.OutputSymlink(path="legacy-dir", target="tree")],
        output_symlinks=[
            re.OutputSymlink(
                path="out/link", target="file.o", node_properties=node_properties(0o120000)
            )
        ],
    )


def directory_fixture():
    return re.Directory(
        files=[
            re.FileNode(
                name="file.txt",
                digest=digest(7, 12),
                is_executable=True,
                node_properties=node_properties(0o100755),
            )
        ],
        directories=[re.DirectoryNode(name="sub", digest=digest(8, 45))],
        symlinks=[
            re.SymlinkNode(
                name="link", target="file.txt", node_properties=node_properties(0o120000)
            )
        ],
        node_properties=node_properties(0o40755),
    )


def save_fixture(output, manifest, name, message, preserve_unknown=False):
    data = message.SerializeToString(deterministic=True)
    if preserve_unknown:
        data += varint((19_000 << 3) | 0) + varint(123_456)
        parsed = type(message)()
        parsed.ParseFromString(data)
        data = parsed.SerializeToString(deterministic=True)
    (output / f"{name}.bin").write_bytes(data)
    manifest.append(f"{name}.bin {message.DESCRIPTOR.full_name}")


def write_message_fixtures(output):
    manifest = []
    platform = re.Platform(
        properties=[
            re.Platform.Property(name="OSFamily", value="linux"),
            re.Platform.Property(name="arch", value="x86_64"),
        ]
    )
    command = re.Command(
        arguments=["clang", "-c", "hello.c"],
        environment_variables=[re.Command.EnvironmentVariable(name="LANG", value="C.UTF-8")],
        output_files=["legacy.o"],
        output_directories=["legacy-dir"],
        output_paths=["hello.o"],
        platform=platform,
        working_directory="src",
        output_node_properties=["unix_mode"],
        output_directory_format=re.Command.TREE_AND_DIRECTORY,
    )
    status = status_pb2.Status(
        code=9,
        message="precondition",
        details=[any_value("google.rpc.DebugInfo", b"detail")],
    )
    operation = operations_pb2.Operation(
        name="operations/one",
        metadata=any_value("build.bazel.remote.execution.v2.ExecuteOperationMetadata", b"meta"),
        done=True,
        response=any_value("build.bazel.remote.execution.v2.ExecuteResponse", b"response"),
    )
    cache_capabilities = re.CacheCapabilities(
        digest_functions=[re.DigestFunction.SHA256, re.DigestFunction.BLAKE3],
        action_cache_update_capabilities=re.ActionCacheUpdateCapabilities(update_enabled=True),
        cache_priority_capabilities=re.PriorityCapabilities(
            priorities=[re.PriorityCapabilities.PriorityRange(min_priority=-10, max_priority=20)]
        ),
        max_batch_total_size_bytes=1_048_576,
        symlink_absolute_path_strategy=re.SymlinkAbsolutePathStrategy.ALLOWED,
        supported_compressors=[re.Compressor.ZSTD, re.Compressor.BROTLI],
        supported_batch_update_compressors=[re.Compressor.DEFLATE],
        max_cas_blob_size_bytes=8_388_608,
        split_blob_support=True,
        splice_blob_support=True,
        fast_cdc_2020_params=re.FastCdc2020Params(avg_chunk_size_bytes=65_536, seed=42),
        rep_max_cdc_params=re.RepMaxCdcParams(
            min_chunk_size_bytes=4_096, horizon_size_bytes=64
        ),
    )
    execution_capabilities = re.ExecutionCapabilities(
        digest_function=re.DigestFunction.BLAKE3,
        exec_enabled=True,
        execution_priority_capabilities=re.PriorityCapabilities(
            priorities=[re.PriorityCapabilities.PriorityRange(min_priority=-5, max_priority=5)]
        ),
        supported_node_properties=["mtime", "unix_mode"],
        digest_functions=[re.DigestFunction.SHA256, re.DigestFunction.BLAKE3],
    )
    fixtures = [
        ("any", any_value("liquid.Test", b"payload"), False),
        ("timestamp", timestamp(1_700_000_000, 42), False),
        ("duration", duration(-12, -345), False),
        ("status", status, False),
        ("operation", operation, False),
        ("digest", digest(1), True),
        ("digest-function", re.DigestFunction(), False),
        ("node-properties", node_properties(0o100644), False),
        (
            "file-node",
            re.FileNode(
                name="file", digest=digest(2), is_executable=True,
                node_properties=node_properties(0o100755)
            ),
            False,
        ),
        ("directory-node", re.DirectoryNode(name="dir", digest=digest(3)), False),
        (
            "symlink-node",
            re.SymlinkNode(
                name="link", target="target", node_properties=node_properties(0o120000)
            ),
            False,
        ),
        ("directory", directory_fixture(), False),
        ("platform", platform, False),
        ("command", command, False),
        (
            "action",
            re.Action(
                command_digest=digest(4),
                input_root_digest=digest(5),
                timeout=duration(30, 100),
                do_not_cache=True,
                salt=b"salt",
                platform=platform,
            ),
            False,
        ),
        ("executed-action-metadata", metadata(), False),
        (
            "output-file",
            re.OutputFile(
                path="out", digest=digest(6), is_executable=True, contents=b"inline",
                node_properties=node_properties(0o100755)
            ),
            False,
        ),
        (
            "output-directory",
            re.OutputDirectory(
                path="out", tree_digest=digest(7), is_topologically_sorted=True,
                root_directory_digest=digest(8)
            ),
            False,
        ),
        (
            "output-symlink",
            re.OutputSymlink(
                path="link", target="target", node_properties=node_properties(0o120000)
            ),
            False,
        ),
        (
            "tree",
            re.Tree(root=directory_fixture(), children=[re.Directory(files=[])]),
            False,
        ),
        ("action-result", action_result(), False),
        (
            "execute-request",
            re.ExecuteRequest(
                instance_name="projects/test/instances/default",
                skip_cache_lookup=True,
                action_digest=digest(9),
                execution_policy=re.ExecutionPolicy(priority=-2),
                results_cache_policy=re.ResultsCachePolicy(priority=3),
                digest_function=re.DigestFunction.BLAKE3,
                inline_stdout=True,
                inline_stderr=True,
                inline_output_files=["out/file.o"],
            ),
            False,
        ),
        (
            "execute-response",
            re.ExecuteResponse(
                result=action_result(),
                cached_result=True,
                status=status,
                server_logs={
                    "setup": re.LogFile(digest=digest(10), human_readable=True),
                    "worker": re.LogFile(digest=digest(11), human_readable=False),
                },
                message="complete",
            ),
            False,
        ),
        (
            "execute-operation-metadata",
            re.ExecuteOperationMetadata(
                stage=re.ExecutionStage.EXECUTING,
                action_digest=digest(12),
                stdout_stream_name="streams/stdout",
                stderr_stream_name="streams/stderr",
                partial_execution_metadata=metadata(),
                digest_function=re.DigestFunction.BLAKE3,
            ),
            False,
        ),
        ("wait-execution-request", re.WaitExecutionRequest(name="operations/one"), False),
        (
            "get-action-result-request",
            re.GetActionResultRequest(
                instance_name="instance",
                action_digest=digest(13),
                inline_stdout=True,
                inline_stderr=True,
                inline_output_files=["out"],
                digest_function=re.DigestFunction.SHA256,
            ),
            False,
        ),
        (
            "update-action-result-request",
            re.UpdateActionResultRequest(
                instance_name="instance",
                action_digest=digest(14),
                action_result=action_result(),
                results_cache_policy=re.ResultsCachePolicy(priority=4),
                digest_function=re.DigestFunction.BLAKE3,
            ),
            False,
        ),
        (
            "find-missing-blobs-request",
            re.FindMissingBlobsRequest(
                instance_name="instance",
                blob_digests=[digest(15), digest(16)],
                digest_function=re.DigestFunction.BLAKE3,
            ),
            False,
        ),
        (
            "find-missing-blobs-response",
            re.FindMissingBlobsResponse(missing_blob_digests=[digest(16)]),
            False,
        ),
        (
            "batch-update-blobs-request",
            re.BatchUpdateBlobsRequest(
                instance_name="instance",
                requests=[
                    re.BatchUpdateBlobsRequest.Request(
                        digest=digest(17, 4), data=b"blob", compressor=re.Compressor.ZSTD
                    )
                ],
                digest_function=re.DigestFunction.BLAKE3,
            ),
            False,
        ),
        (
            "batch-update-blobs-response",
            re.BatchUpdateBlobsResponse(
                responses=[
                    re.BatchUpdateBlobsResponse.Response(digest=digest(17, 4), status=status)
                ]
            ),
            False,
        ),
        (
            "batch-read-blobs-request",
            re.BatchReadBlobsRequest(
                instance_name="instance",
                digests=[digest(18, 4)],
                acceptable_compressors=[re.Compressor.ZSTD, re.Compressor.DEFLATE],
                digest_function=re.DigestFunction.SHA256,
            ),
            False,
        ),
        (
            "batch-read-blobs-response",
            re.BatchReadBlobsResponse(
                responses=[
                    re.BatchReadBlobsResponse.Response(
                        digest=digest(18, 4), data=b"blob", status=status,
                        compressor=re.Compressor.ZSTD
                    )
                ]
            ),
            False,
        ),
        (
            "get-tree-request",
            re.GetTreeRequest(
                instance_name="instance", root_digest=digest(19), page_size=100,
                page_token="next", digest_function=re.DigestFunction.BLAKE3
            ),
            False,
        ),
        (
            "get-tree-response",
            re.GetTreeResponse(directories=[directory_fixture()], next_page_token="next"),
            False,
        ),
        (
            "get-capabilities-request",
            re.GetCapabilitiesRequest(instance_name="instance"),
            False,
        ),
        ("cache-capabilities", cache_capabilities, False),
        ("execution-capabilities", execution_capabilities, False),
        (
            "server-capabilities",
            re.ServerCapabilities(
                cache_capabilities=cache_capabilities,
                execution_capabilities=execution_capabilities,
                deprecated_api_version=semver_pb2.SemVer(major=2, minor=0, patch=0),
                low_api_version=semver_pb2.SemVer(major=2, minor=2, patch=0),
                high_api_version=semver_pb2.SemVer(
                    major=2, minor=3, patch=0, prerelease="fixture"
                ),
            ),
            False,
        ),
        (
            "read-request",
            bytestream_pb2.ReadRequest(resource_name="blobs/hash/123", read_offset=4, read_limit=8),
            False,
        ),
        ("read-response", bytestream_pb2.ReadResponse(data=b"bytes"), False),
        (
            "write-request",
            bytestream_pb2.WriteRequest(
                resource_name="uploads/id/blobs/hash/123", write_offset=4,
                finish_write=True, data=b"bytes"
            ),
            False,
        ),
        ("write-response", bytestream_pb2.WriteResponse(committed_size=9), False),
        (
            "query-write-status-request",
            bytestream_pb2.QueryWriteStatusRequest(resource_name="uploads/id/blobs/hash/123"),
            False,
        ),
        (
            "query-write-status-response",
            bytestream_pb2.QueryWriteStatusResponse(committed_size=9, complete=True),
            False,
        ),
    ]
    for name, message, preserve_unknown in fixtures:
        save_fixture(output, manifest, name, message, preserve_unknown)
    (output / "MANIFEST").write_text("\n".join(manifest) + "\n", encoding="utf-8")


def write_directory_reference(output):
    contents = {
        "hello.txt": b"hello\n",
        "run.sh": b"#!/bin/sh\necho hi\n",
        "nested.txt": b"nested\n",
    }

    def sha256_digest(data):
        return re.Digest(hash=hashlib.sha256(data).hexdigest(), size_bytes=len(data))

    def mode_properties(mode):
        return re.NodeProperties(unix_mode=wrappers_pb2.UInt32Value(value=mode))

    child = re.Directory(
        files=[
            re.FileNode(
                name="nested.txt",
                digest=sha256_digest(contents["nested.txt"]),
                node_properties=mode_properties(0o100644),
            )
        ],
        node_properties=mode_properties(0o40755),
    )
    child_bytes = child.SerializeToString(deterministic=True)
    root = re.Directory(
        files=[
            re.FileNode(
                name="hello.txt",
                digest=sha256_digest(contents["hello.txt"]),
                node_properties=mode_properties(0o100644),
            ),
            re.FileNode(
                name="run.sh",
                digest=sha256_digest(contents["run.sh"]),
                is_executable=True,
                node_properties=mode_properties(0o100755),
            ),
        ],
        directories=[re.DirectoryNode(name="sub", digest=sha256_digest(child_bytes))],
        symlinks=[
            re.SymlinkNode(
                name="hello-link",
                target="hello.txt",
                node_properties=mode_properties(0o120000),
            )
        ],
        node_properties=mode_properties(0o40755),
    )
    root_bytes = root.SerializeToString(deterministic=True)
    (output / "directory-reference.bin").write_bytes(root_bytes)
    (output / "directory-reference-child.bin").write_bytes(child_bytes)
    (output / "directory-reference.sha256").write_text(
        hashlib.sha256(root_bytes).hexdigest() + "\n", encoding="utf-8"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate.py OUTPUT_DIRECTORY")
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    write_message_fixtures(output)
    write_directory_reference(output)
    (output / "VERSION").write_text(
        "remote-apis " + os.environ["REMOTE_APIS_COMMIT"] + "\n"
        "googleapis " + os.environ["GOOGLEAPIS_COMMIT"] + "\n"
        "protobuf-python 6.32.0\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()

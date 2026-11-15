# SPDX-License-Identifier: MIT

# Pinned by its multi-platform index digest (lake#1003); a new base is a reviewed change of the digest.
FROM python:3.13.7-slim-bookworm@sha256:adafcc17694d715c905b4c7bebd96907a1fd5cf183395f0ebc4d3428bd22d92d

ARG REMOTE_APIS_COMMIT=77ec630134abbf9aa525f921eee4e5d11dc20f7e
ARG GOOGLEAPIS_COMMIT=64aa30b277168edd20efee0c9ceb4ca01248931d

RUN apt-get update \
    && apt-get install --yes --no-install-recommends git libprotobuf-dev protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir protobuf==6.32.0

RUN git clone --filter=blob:none https://github.com/bazelbuild/remote-apis.git /remote-apis \
    && git -C /remote-apis checkout "${REMOTE_APIS_COMMIT}"
RUN git clone --filter=blob:none https://github.com/googleapis/googleapis.git /googleapis \
    && git -C /googleapis checkout "${GOOGLEAPIS_COMMIT}"

RUN mkdir /generated \
    && protoc \
        --proto_path=/remote-apis \
        --proto_path=/googleapis \
        --proto_path=/usr/include \
        --python_out=/generated \
        /remote-apis/build/bazel/remote/execution/v2/remote_execution.proto \
        /remote-apis/build/bazel/semver/semver.proto \
        /googleapis/google/bytestream/bytestream.proto \
        /googleapis/google/longrunning/operations.proto \
        /googleapis/google/rpc/status.proto \
        /googleapis/google/api/annotations.proto \
        /googleapis/google/api/client.proto \
        /googleapis/google/api/field_behavior.proto \
        /googleapis/google/api/http.proto \
        /googleapis/google/api/launch_stage.proto \
        /googleapis/google/api/resource.proto

ENV PYTHONPATH=/generated
ENV REMOTE_APIS_COMMIT=${REMOTE_APIS_COMMIT}
ENV GOOGLEAPIS_COMMIT=${GOOGLEAPIS_COMMIT}

COPY generate.py /generator/generate.py
ENTRYPOINT ["python", "/generator/generate.py"]

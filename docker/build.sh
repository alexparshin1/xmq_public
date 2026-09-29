#!/bin/bash
# Builds the XMQ image as alexeyparshin/xmq:<version> and :latest.
#
# --no-cache: the .deb is downloaded inside a RUN step from a URL that does not change between
# rebuilds of the same version, so Docker would otherwise reuse the cached layer and package a
# stale .deb into an image that looks new.
set -eu
cd "$(dirname "$0")"
source ./versions.sh

IMAGE=alexeyparshin/xmq

echo "Building $IMAGE:$XMQ_VERSION (XMQ $XMQ_VERSION, SPTK $SPTK_VERSION)"
docker build --no-cache --pull \
    --build-arg XMQ_VERSION="$XMQ_VERSION" \
    --build-arg SPTK_VERSION="$SPTK_VERSION" \
    -t "$IMAGE:$XMQ_VERSION" -t "$IMAGE:latest" .

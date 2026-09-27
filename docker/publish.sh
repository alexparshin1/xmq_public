#!/bin/bash
# Pushes the XMQ image to Docker Hub as <version> and latest.
#
# Build it first, and build it with --no-cache: the .deb is downloaded inside a RUN step from a
# URL that does not change between rebuilds of the same version, so Docker will otherwise reuse
# the cached layer and package a stale .deb into an image that looks new.
#
#   docker build --no-cache --pull -t alexeyparshin/xmq:<version> .
#   ./publish.sh
set -eu
cd "$(dirname "$0")"

IMAGE=alexeyparshin/xmq
# Taken from the Dockerfile rather than written twice, so the tag cannot drift from the .deb.
VERSION=$(sed -n 's/^ARG XMQ_VERSION=//p' Dockerfile)

# The image installs a published .deb instead of building from source, so nothing in this
# repository implies what is inside it. It is checked here, on the image about to be pushed.
./check-image.sh "$IMAGE:$VERSION"

docker login
docker tag "$IMAGE:$VERSION" "$IMAGE:latest"
docker push "$IMAGE:$VERSION"
docker push "$IMAGE:latest"

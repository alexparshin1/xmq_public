#!/bin/bash
# Pushes the XMQ image to Docker Hub as <version> and latest.
#
# Build it first, with ./build.sh.
set -eu
cd "$(dirname "$0")"
source ./versions.sh

IMAGE=alexeyparshin/xmq

# The image installs a published .deb instead of building from source, so nothing in this
# repository implies what is inside it. It is checked here, on the image about to be pushed.
./check-image.sh "$IMAGE:$XMQ_VERSION"

docker login
docker tag "$IMAGE:$XMQ_VERSION" "$IMAGE:latest"
docker push "$IMAGE:$XMQ_VERSION"
docker push "$IMAGE:latest"

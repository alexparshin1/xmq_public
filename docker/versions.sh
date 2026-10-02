# Sourced by build.sh and publish.sh: the versions the image is built from, read from the
# projects' own VERSION.txt files so they are never written down a second time here.
#
#   source ./versions.sh     # also before "docker compose up --build"

DOCKER_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

XMQ_VERSION=$(< "$DOCKER_DIR/../VERSION.txt")
SPTK_VERSION=$(< "$DOCKER_DIR/../VERSION.SPTK.txt")
export XMQ_VERSION SPTK_VERSION

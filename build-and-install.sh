#!/bin/bash
set -euo pipefail

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<'EOF'
Usage: ./build-and-install.sh [NAME=VALUE ...]

Build, sign, install, and open ISS.app in ~/Applications.
Requires macOS and the Xcode command-line tools.

Optional Makefile overrides:
  ./build-and-install.sh APP_INSTALL_DIR=/Applications
  ./build-and-install.sh SDKROOT=/path/to/MacOSX.sdk

Quit a running ISS app before updating. If you use the legacy command-line
service, run `make uninstall` first. Stop any foreground iss process too.
EOF
    exit 0
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "Error: ISS requires macOS." >&2
    exit 1
fi

if ! command -v make >/dev/null 2>&1 ||
   ! xcrun --find clang >/dev/null 2>&1 ||
   ! xcrun --find swiftc >/dev/null 2>&1; then
    echo "Error: Install the Xcode command-line tools with: xcode-select --install" >&2
    exit 1
fi

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec make -C "$PROJECT_DIR" install-app "$@"

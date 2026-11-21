#!/usr/bin/env bash
set -euo pipefail

# RS232 revision used for the directory-mode BoIP compatibility checks.
revision=8f7231a78
source_dir="${1:-build/fujinet-firmware}"
if [[ ! -e "$source_dir" ]]; then
    mkdir -p "$(dirname "$source_dir")"
    git clone --filter=blob:none --no-checkout https://github.com/FujiNetWIFI/fujinet-firmware.git "$source_dir"
fi
source_dir="$(realpath "$source_dir")"
if [[ ! -e "$source_dir/.git" ]]; then
    echo "Expected a dedicated firmware checkout at $source_dir" >&2
    exit 1
fi
if [[ "$(git -C "$source_dir" rev-parse --show-toplevel)" != "$source_dir" ]]; then
    echo "Expected a standalone firmware checkout at $source_dir" >&2
    exit 1
fi
if ! git -C "$source_dir" rev-parse --verify --quiet "${revision}^{commit}" >/dev/null; then
    git -C "$source_dir" fetch origin master
fi
git -C "$source_dir" switch --detach "$revision"
cd "$source_dir"
./build.sh -p RS232

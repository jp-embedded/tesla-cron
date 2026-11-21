#!/bin/bash
set -euo pipefail
umask 077

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=go1.27.3
sdk=v0.4.1
case "$(uname -m)" in
    aarch64|arm64) arch=arm64 ;;
    x86_64|amd64) arch=amd64 ;;
    *) echo "Unsupported native proxy build architecture." >&2; exit 1 ;;
esac
[[ $(uname -s) == Linux ]] || { echo "This build script targets Linux." >&2; exit 1; }
for command in curl python3 tar sha256sum; do
    command -v "$command" >/dev/null || { echo "Missing build tool: $command" >&2; exit 1; }
done

# Toolchain/dependency caches stay in a private temporary build tree, not the system.
temporary_root=${TMPDIR:-/tmp/opencode}
mkdir -p "$temporary_root"
build=${TESLA_PROXY_BUILD_DIR:-$(mktemp -d "$temporary_root/tesla-proxy-build.XXXXXX")}
echo "Building official vehicle-command $sdk in $build"
if [[ ! -x "$build/go/bin/go" ]]; then
curl --fail --silent --show-error --location --connect-timeout 15 --max-time 120 \
    'https://go.dev/dl/?mode=json&include=all' --output "$build/releases.json"
checksum=$(python3 - "$build/releases.json" "$version" "$arch" <<'PY'
import json
import sys
with open(sys.argv[1]) as stream:
    releases = json.load(stream)
for release in releases:
    if release['version'] == sys.argv[2]:
        for file in release['files']:
            if file['os'] == 'linux' and file['arch'] == sys.argv[3] and file['kind'] == 'archive':
                print(file['sha256'])
                raise SystemExit(0)
raise SystemExit('Pinned Go archive is absent from official release metadata.')
PY
)
archive="$build/$version.linux-$arch.tar.gz"
curl --fail --silent --show-error --location --connect-timeout 15 --max-time 600 \
    "https://go.dev/dl/$version.linux-$arch.tar.gz" --output "$archive"
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
tar -xzf "$archive" -C "$build"
fi

export GOTOOLCHAIN=local
export GOPROXY=https://proxy.golang.org,direct
export GOSUMDB=sum.golang.org
export GONOSUMDB= GONOPROXY= GOINSECURE=
export GOCACHE="$build/cache" GOMODCACHE="$build/modules" GOBIN="$build/bin"
export GOWORK=off CGO_ENABLED=0
# The tagged SDK contains replace directives, so build it as the main module;
# `go install package@version` deliberately refuses such modules.
"$build/go/bin/go" mod download -json "github.com/teslamotors/vehicle-command@$sdk" > "$build/sdk.json"
source_directory=$(python3 - "$build/sdk.json" <<'PY'
import json
import sys
with open(sys.argv[1]) as stream:
    module = json.load(stream)
if module.get('Sum') != 'h1:J4ne/TNGwgodJLYJDLm/hjoygXyQ/bpqO/EiCaeoobM=':
    raise SystemExit('Tagged SDK checksum differs from the pinned Go checksum-database record.')
print(module['Dir'])
PY
)
mkdir -p "$build/bin"
"$build/go/bin/go" -C "$source_directory" build -mod=readonly -trimpath \
    -o "$build/bin/tesla-http-proxy" ./cmd/tesla-http-proxy
mkdir -p "$root/.proxy"
install -m 755 "$build/bin/tesla-http-proxy" "$root/.proxy/tesla-http-proxy"
"$root/.proxy/tesla-http-proxy" -h >/dev/null 2>&1
echo "Built .proxy/tesla-http-proxy ($sdk). No system installation or Tesla requests made."

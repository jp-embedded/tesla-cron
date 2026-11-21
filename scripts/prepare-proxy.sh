#!/bin/bash
set +x
set -euo pipefail
umask 077

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
directory="$root/.proxy"
mkdir -p "$directory"
chmod 700 "$directory"

if [[ $# -gt 1 ]]; then
    echo "Usage: $0 [existing-Tesla-command-private-key.pem]" >&2
    exit 1
fi
if [[ $# -eq 1 ]]; then
    openssl pkey -in "$1" -check -noout >/dev/null
    if [[ -e "$directory/command.key" ]]; then
        cmp -s "$1" "$directory/command.key" || { echo "Refusing to replace a different command key." >&2; exit 1; }
    else
        install -m 600 "$1" "$directory/command.key"
    fi
fi
[[ -f "$directory/command.key" ]] || { echo "Supply the existing paired command key; this script never creates a replacement." >&2; exit 1; }
openssl pkey -in "$directory/command.key" -check -noout >/dev/null

if [[ -e "$directory/localhost.key" || -e "$directory/localhost.crt" ]]; then
    [[ -f "$directory/localhost.key" && -f "$directory/localhost.crt" ]] || {
        echo "Incomplete local TLS pair; refusing automatic replacement." >&2; exit 1;
    }
else
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -keyout "$directory/localhost.key" -out "$directory/localhost.crt" \
        -days 3650 -sha256 -subj '/CN=tesla-cron local proxy' \
        -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1' \
        -addext 'basicConstraints=critical,CA:FALSE' \
        -addext 'keyUsage=critical,digitalSignature' \
        -addext 'extendedKeyUsage=serverAuth' >/dev/null 2>&1
fi
chmod 600 "$directory/command.key" "$directory/localhost.key"
chmod 644 "$directory/localhost.crt"
openssl x509 -in "$directory/localhost.crt" -checkend 2592000 -noout >/dev/null || {
    echo "Local TLS certificate expires within 30 days; deliberate rotation is required." >&2; exit 1;
}
certificate_public=$(openssl x509 -in "$directory/localhost.crt" -pubkey -noout | openssl pkey -pubin -outform DER | sha256sum)
tls_public=$(openssl pkey -in "$directory/localhost.key" -pubout -outform DER | sha256sum)
command_public=$(openssl pkey -in "$directory/command.key" -pubout -outform DER | sha256sum)
[[ $certificate_public == "$tls_public" && $tls_public != "$command_public" ]] || {
    echo "TLS certificate/key mismatch or unsafe reuse of the command key." >&2; exit 1;
}
openssl x509 -in "$directory/localhost.crt" -checkip 127.0.0.1 -noout >/dev/null
echo "Prepared separate local TLS and recovered command keys under ignored .proxy/. Caddy was not changed."

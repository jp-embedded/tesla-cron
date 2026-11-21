#!/bin/bash
set +x
set -euo pipefail
umask 077

[[ $EUID -eq 0 ]] || { echo "Run this script with sudo after building/preparing .proxy/." >&2; exit 1; }
[[ $# -eq 0 ]] || { echo "Usage: sudo $0" >&2; exit 1; }
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_directory="$root/.proxy"
destination=/etc/tesla-cron/proxy
service=tesla-cron-proxy.service
for command in openssl systemctl curl install cmp sha256sum; do
    command -v "$command" >/dev/null || { echo "Missing install tool: $command" >&2; exit 1; }
done
read -r _ systemd_version _ < <(systemctl --version)
[[ "$systemd_version" =~ ^[0-9]+$ ]] && (( systemd_version >= 247 )) || {
    echo "systemd v247+ is required for private service credentials." >&2; exit 1;
}
[[ -x "$source_directory/tesla-http-proxy" ]] || { echo "Build the proxy first: ./scripts/build-proxy.sh" >&2; exit 1; }
"$source_directory/tesla-http-proxy" -h >/dev/null 2>&1
for file in command.key localhost.key localhost.crt; do
    [[ -f "$source_directory/$file" ]] || { echo "Prepare local proxy credentials first: ./scripts/prepare-proxy.sh existing-command-key.pem" >&2; exit 1; }
done
openssl pkey -in "$source_directory/command.key" -check -noout >/dev/null
[[ ! -L /etc/tesla-cron && ! -L "$destination" && ! -L /usr/local/share/tesla-cron ]] || {
    echo "Refusing a symlinked proxy configuration directory." >&2; exit 1;
}
if [[ -e "$destination/command.key" ]]; then
    cmp -s "$source_directory/command.key" "$destination/command.key" || {
        echo "Installed command key differs; refusing automatic key rotation." >&2; exit 1;
    }
fi
if [[ -e "$destination/localhost.key" || -e "$destination/localhost.crt" ]]; then
    [[ -f "$destination/localhost.key" && -f "$destination/localhost.crt" ]] || {
        echo "Installed TLS pair is incomplete; refusing automatic replacement." >&2; exit 1;
    }
    tls_directory="$destination"
else
    tls_directory="$source_directory"
fi
openssl x509 -in "$tls_directory/localhost.crt" -checkend 2592000 -noout >/dev/null || {
    echo "TLS certificate needs deliberate rotation before installation." >&2; exit 1;
}
certificate_public=$(openssl x509 -in "$tls_directory/localhost.crt" -pubkey -noout | openssl pkey -pubin -outform DER | sha256sum)
tls_public=$(openssl pkey -in "$tls_directory/localhost.key" -pubout -outform DER | sha256sum)
command_public=$(openssl pkey -in "$source_directory/command.key" -pubout -outform DER | sha256sum)
[[ $certificate_public == "$tls_public" && $tls_public != "$command_public" ]] || {
    echo "TLS pair mismatch or unsafe reuse of the command key." >&2; exit 1;
}
openssl x509 -in "$tls_directory/localhost.crt" -checkip 127.0.0.1 -noout >/dev/null

install -d -o root -g root -m 700 /etc/tesla-cron "$destination"
install -d -o root -g root -m 755 /usr/local/libexec
install -d -o root -g root -m 755 /usr/local/share/tesla-cron
install -o root -g root -m 755 "$source_directory/tesla-http-proxy" /usr/local/libexec/tesla-http-proxy
if [[ ! -e "$destination/command.key" ]]; then
    install -o root -g root -m 600 "$source_directory/command.key" "$destination/command.key"
fi
if [[ "$tls_directory" == "$source_directory" ]]; then
    install -o root -g root -m 600 "$source_directory/localhost.key" "$destination/localhost.key"
    install -o root -g root -m 644 "$source_directory/localhost.crt" "$destination/localhost.crt"
fi
# The certificate is public, unlike its private key. Keep a readable explicit
# client trust file outside the private credentials directory, not in system CA trust.
install -o root -g root -m 644 "$destination/localhost.crt" /usr/local/share/tesla-cron/proxy.crt
install -o root -g root -m 644 "$root/systemd/$service" "/etc/systemd/system/$service"
systemctl daemon-reload
systemctl enable "$service"
systemctl restart "$service"
for attempt in {1..10}; do
    if response=$(curl --fail --silent --show-error --noproxy '*' \
        --cacert "$destination/localhost.crt" --connect-timeout 1 --max-time 3 \
        https://127.0.0.1:4443/health 2>/dev/null) && [[ "$response" == OK ]]; then
        echo "Persistent loopback proxy installed; TLS-verified /health passed. Caddy and charging cron were not changed."
        exit 0
    fi
    sleep 1
done
echo "Proxy health failed. Inspect: sudo journalctl -u $service -n 30" >&2
exit 1

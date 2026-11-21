#!/bin/bash
set +x
set -euo pipefail
umask 077
if [[ -f /etc/tesla-cron/runtime.env ]]; then
    set -a
    source /etc/tesla-cron/runtime.env
    set +a
fi
unset TESLA_CLIENT_SECRET
exec /usr/local/bin/tesla_cron "$@"

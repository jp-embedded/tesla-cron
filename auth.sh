#!/bin/bash

# Never trace credentials, including when invoked with bash -x.
set +x
set -euo pipefail
umask 077

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
auth_config=${TESLA_AUTH_CONFIG:-"$script_dir/.auth.env"}
if [[ -f "$auth_config" ]]; then
    # This is a trusted, local shell configuration; never commit credentials.
    source "$auth_config"
elif [[ -n ${TESLA_AUTH_CONFIG:-} ]]; then
    echo "Auth configuration file not found." >&2
    exit 1
fi

for command in curl python3; do
    command -v "$command" >/dev/null || { echo "Required command missing: $command" >&2; exit 1; }
done

: "${TESLA_CLIENT_ID:?Set TESLA_CLIENT_ID in .auth.env or the environment}"
: "${TESLA_CLIENT_SECRET:?Set TESLA_CLIENT_SECRET in .auth.env or the environment}"
: "${REDIRECT_URI:?Set REDIRECT_URI to the exact callback registered with Tesla}"
export TESLA_CLIENT_ID TESLA_CLIENT_SECRET REDIRECT_URI
export AUDIENCE=${AUDIENCE:-https://fleet-api.prd.eu.vn.cloud.tesla.com}
export TESLA_AUTH_SCOPE="openid offline_access user_data vehicle_device_data vehicle_location vehicle_cmds vehicle_charging_cmds"
export TESLA_AUTH_STATE
TESLA_AUTH_STATE=$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')

echo "Visit this URL:"
python3 - <<'PY'
import os
from urllib.parse import urlencode, urlsplit

redirect = urlsplit(os.environ['REDIRECT_URI'])
if redirect.scheme != 'https' or not redirect.netloc:
    raise SystemExit('REDIRECT_URI must be an HTTPS callback registered with Tesla.')
params = {
    'client_id': os.environ['TESLA_CLIENT_ID'],
    'redirect_uri': os.environ['REDIRECT_URI'],
    'response_type': 'code',
    'scope': os.environ['TESLA_AUTH_SCOPE'],
    'state': os.environ['TESLA_AUTH_STATE'],
    'prompt_missing_scopes': 'true',
}
print('https://auth.tesla.com/oauth2/v3/authorize?' + urlencode(params))
PY
echo
echo "After authorizing, paste the full callback URL (even if its page does not load)."
IFS= read -r callback
export TESLA_AUTH_CODE
TESLA_AUTH_CODE=$(TESLA_AUTH_CALLBACK="$callback" python3 - <<'PY'
import os
import secrets
from urllib.parse import parse_qs, urlsplit

callback = urlsplit(os.environ['TESLA_AUTH_CALLBACK'])
expected = urlsplit(os.environ['REDIRECT_URI'])
if (callback.scheme, callback.netloc, callback.path) != (expected.scheme, expected.netloc, expected.path):
    raise SystemExit('Callback URL does not match REDIRECT_URI; no token request was made.')
params = parse_qs(callback.query, keep_blank_values=True)
states = params.get('state', [])
if len(states) != 1 or not secrets.compare_digest(states[0], os.environ['TESLA_AUTH_STATE']):
    raise SystemExit('OAuth state mismatch; no token request was made. Restart authorization.')
if 'error' in params:
    raise SystemExit('Tesla declined authorization; no token request was made.')
codes = params.get('code', [])
if len(codes) != 1 or not codes[0]:
    raise SystemExit('Callback has no unique authorization code; no token request was made.')
print(codes[0])
PY
)
unset callback

# Encode on stdin so the client secret and authorization code are not curl arguments.
response=$(python3 - <<'PY' | curl --silent --show-error \
    --connect-timeout 15 --max-time 60 \
    --header 'Content-Type: application/x-www-form-urlencoded' \
    --data-binary @- --write-out $'\n%{http_code}' \
    'https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token'
import os
import sys
from urllib.parse import urlencode

sys.stdout.write(urlencode({
    'grant_type': 'authorization_code',
    'client_id': os.environ['TESLA_CLIENT_ID'],
    'client_secret': os.environ['TESLA_CLIENT_SECRET'],
    'code': os.environ['TESLA_AUTH_CODE'],
    'audience': os.environ['AUDIENCE'],
    'redirect_uri': os.environ['REDIRECT_URI'],
}))
PY
)
unset TESLA_CLIENT_SECRET TESLA_AUTH_CODE
http_status=${response##*$'\n'}
if [[ ! "$http_status" =~ ^2[0-9][0-9]$ ]]; then
    echo "Token exchange failed (HTTP $http_status); existing tokens were not changed." >&2
    echo "Check the app credentials, exact registered callback, and code expiry." >&2
    exit 1
fi

printf '%s' "${response%$'\n'*}" | python3 -c '
import json
import os
from pathlib import Path
import sys
import tempfile

try:
    tokens = json.load(sys.stdin)
except (ValueError, TypeError):
    raise SystemExit("Invalid token response; existing tokens were not changed.")
names = ("access_token", "refresh_token")
if not isinstance(tokens, dict) or any(not isinstance(tokens.get(name), str) or not tokens[name].strip() or tokens[name] == "null" for name in names):
    raise SystemExit("Response lacks valid access/refresh tokens; existing tokens were not changed.")
directory = Path(sys.argv[1])
directory.mkdir(mode=0o700, parents=True, exist_ok=True)
pending = []
try:
    for name in names:
        with tempfile.NamedTemporaryFile(mode="w", dir=directory, prefix=".auth-", delete=False) as stream:
            pending.append((Path(stream.name), directory / (name + ".txt")))
            stream.write(tokens[name] + "\n")
    for temporary, destination in pending:
        os.replace(temporary, destination)
finally:
    for temporary, _ in pending:
        temporary.unlink(missing_ok=True)
print("Saved access and refresh tokens to " + str(directory) + " (owner-only permissions).")
' "${TOKEN_DIR:-/var/tmp/tesla-cron}"

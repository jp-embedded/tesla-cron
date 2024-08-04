#!/bin/bash

TESLA_CLIENT_ID=fc18xxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
TESLA_CLIENT_SECRET=ta-secret.xxxxxxxxx
REDIRECT_URI=https://host.com/tesla/auth

# scopes we want, which includes user_data
# unrelated: offline_access is provided here, which is mentioned in docs, but it too is not in access token
scope="openid user_data offline_access vehicle_device_data vehicle_cmds vehicle_charging_cmds"
state=${state:-123456781234}
: ${TESLA_CLIENT_ID:?required}
: ${TESLA_CLIENT_SECRET:?required}
AUDIENCE=${AUDIENCE:-https://fleet-api.prd.eu.vn.cloud.tesla.com}

echo "Visit this URL:"
#echo 'https://auth.tesla.com/oauth2/v3/authorize?client_id=fc184c3d-dc13-4f3c-a33d-af8621e84d0f&locale=da-DK&prompt=login&redirect_uri=https://jp-embedded.com/tesla/auth&response_type=code&scope=openid%20user_data%20vehicle_device_data%20vehicle_cmds%20vehicle_charging_cmds%20energy_device_data%20energy_cmds%20offline_access&state=abc123'
echo "https://auth.tesla.com/oauth2/v3/authorize?client_id=${TESLA_CLIENT_ID}&locale=da-DK&prompt=login&redirect_uri=${REDIRECT_URI}&response_type=code&scope=openid%20user_data%20vehicle_device_data%20vehicle_cmds%20vehicle_charging_cmds%20energy_device_data%20energy_cmds%20offline_access&state=abc123"
echo
echo "When the auth sequence is finished and it redirects to the callback URL"
echo "copy the code=XYZ from the URL and paste it here:"
read code


token_response=$(
curl -sS 'https://auth.tesla.com/oauth2/v3/token' \
-H 'Content-Type: application/json' \
-d "$(jq -n \
--arg client_id "$TESLA_CLIENT_ID" \
--arg client_secret "$TESLA_CLIENT_SECRET" \
--arg code "${code:?required}" \
--arg scope "$scope" \
'{
    grant_type: "authorization_code",
    client_id: $client_id,
    client_secret: $client_secret,
    code: $code,
    scope: $scope,
    audience: "https://fleet-api.prd.eu.vn.cloud.tesla.com",
    redirect_uri: "https://jp-embedded.com/tesla/auth"
  }')"
)
access_token=$(echo "$token_response" | jq -r .access_token)
refresh_token=$(echo "$token_response" | jq -r .refresh_token)
# id_token=$(echo "$token_response" | jq -r .id_token)

echo "access_token: ${access_token}"
echo

echo "Saving tokens"
mkdir -p /var/tmp/tesla-cron
echo "$access_token" > /var/tmp/tesla-cron/access_token.txt
echo "$refresh_token" > /var/tmp/tesla-cron/refresh_token.txt

# the following fails because access token does not have `user_data` scope, even though we asked for it:
curl "${AUDIENCE}/api/1/users/me" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $(cat /var/tmp/tesla-cron/access_token.txt)"


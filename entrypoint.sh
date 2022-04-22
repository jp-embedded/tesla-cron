#!/bin/bash

# This scripts updates the auth, then starts the cron job.

# Helper functions.
function die() {
    echo "Fatal error: " "$@"
    exit 1
}

function log() {
    DATE=`date +"%Y-%m-%dT%H:%M:%S"`
    echo $DATE ">> $@"
}

# TODO: Check the environment variables are actually set

log "Setting up authentification"
# Run the auth setup 
# A NEW, NON-INTERACTIVE AUTH SCRIPT IS NEEDED.
# python3 set-auth.py "${TESLA_ACCOUNT_EMAIL}" "${TESLA_SSO_REFRESH_TOKEN}" || die "Unable to setup refresh token"

log "Running tesla-cron"
# Auth up, run the cronjob
exec ./tesla-cron/tesla_cron

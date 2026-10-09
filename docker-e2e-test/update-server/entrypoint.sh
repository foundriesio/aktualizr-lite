#!/bin/bash -e
#
# Run a local update-server for the e2e tests, bootstrapping auth, PKI and TUF on first
# start; a restart reuses $DATADIR.

DATADIR=${DATADIR:-/data}
HOSTNAME=${UPDATE_SERVER_HOSTNAME:-update-server}

mkdir -p "$DATADIR"

if [ ! -f "$DATADIR/auth/hmac.secret" ]; then
    echo "## Initializing auth (test mode) ..."
    fioserver --datadir "$DATADIR" auth-init --test
fi

if [ ! -f "$DATADIR/certs/tls.crt" ]; then
    echo "## Generating PKI (root CA, server TLS, device CA) ..."
    fioserver --datadir "$DATADIR" pki-init --dnsname "$HOSTNAME"
fi

if [ ! -f "$DATADIR/tuf/keys/root.key" ]; then
    echo "## Initializing TUF ..."
    fioserver --datadir "$DATADIR" tuf-init
fi

echo "## Starting update-server ..."
exec fioserver serve --datadir "$DATADIR"

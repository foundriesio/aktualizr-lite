#!/bin/bash -e
#
# Bootstrap and run a local update-server instance for e2e testing, as a
# self-hosted stand-in for the real Foundries.io Factory backend.
#
# On first run (empty $DATADIR) this generates a fresh test-mode PKI
# (self-signed root/device CAs -- no real Foundries factory needed) and
# initializes TUF, then starts `fioserver serve`. Re-running against an
# already-initialized $DATADIR (e.g. a restarted container reusing the same
# volume) skips the steps that already completed, matching the sequence
# proven by update-server's own e2e fixtures (contrib/e2e/conftest.py).

DATADIR=${DATADIR:-/data}
HOSTNAME=${UPDATE_SERVER_HOSTNAME:-update-server}
FACTORY=${UPDATE_SERVER_FACTORY:-e2e-factory}

mkdir -p "$DATADIR"

if [ ! -f "$DATADIR/auth/hmac.secret" ]; then
    echo "## Initializing auth (test mode) ..."
    fioserver --datadir "$DATADIR" auth-init --test
fi

if [ ! -f "$DATADIR/certs/tls.pem" ]; then
    echo "## Generating PKI (root CA, server TLS, device CA) ..."
    fioserver --datadir "$DATADIR" pki-init --dnsname "$HOSTNAME" --factory "$FACTORY"
fi

if [ ! -f "$DATADIR/tuf/keys/root.key" ]; then
    echo "## Initializing TUF ..."
    fioserver --datadir "$DATADIR" tuf-init
fi

echo "## Starting update-server ..."
exec fioserver serve --datadir "$DATADIR" --rolloutinterval "${UPDATE_SERVER_ROLLOUT_INTERVAL:-5s}"

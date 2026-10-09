#!/bin/sh -e
# Regenerates the e2e registry's self-signed cert; it is also its own CA.
cd "$(dirname "$0")"
openssl req -new -x509 -nodes -days 36500 -config openssl.cnf -keyout registry.key -out registry.crt

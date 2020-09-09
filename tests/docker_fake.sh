#! /bin/bash
set -eEo pipefail

for x in ps; do
  if [ "$1" = "$x" ] ; then
    cat "$(dirname $0)/$1.in"
    exit 0
  fi
done

echo "Unknown command: $*"
exit 1

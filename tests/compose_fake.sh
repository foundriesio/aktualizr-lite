#! /bin/bash
set -eEo pipefail

echo $0 called with $*

for x in download config pull down up; do
  if [ "$1" = "$x" ] ; then
    echo $* > $x.log
    if [ "$2" = "FAILTEST" ] ; then
      exit 1
    fi
    exit 0
  fi
done

echo "Unknown command: $*"
exit 1

#! /bin/bash
set -eEo pipefail

echo $0 called with $*

exit_code=0
for x in download config pull down up start; do
  if [ "$1" = "$x" ] ; then
    echo $* > $x.log
    if [ "$2" = "FAILTEST" ] ; then
      exit 1
    fi
    if [ -f "$1.res" ] ; then
      exit_code=$(cat "$1.res")
    fi
    exit ${exit_code}
  fi
done

echo "Unknown command: $*"
exit 1

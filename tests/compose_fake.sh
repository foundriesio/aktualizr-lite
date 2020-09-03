#! /bin/bash
set -eEo pipefail

# echo $0 called with $*

CMD=$1
if [ "$CMD" = "-f" ]; then
  # if docker-compose.yml specified via -f parameters
  CMD=$3
  if [ "$CMD" = "config" ]; then
    cat $2
  else
    cat "$(dirname "$2")/$3.in"
  fi
fi

for x in download config pull down up start images ps; do
  if [ "$CMD" = "$x" ] ; then
    echo $* > $x.log
    if [ "$2" = "FAILTEST" ] ; then
      exit 1
    fi
    exit 0
  fi
done

echo "Unknown command: $*"
exit 1

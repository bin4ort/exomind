#!/bin/sh
set -e
rm -rf "$TMPDIR/cache"
if [ "$DEBUG" = "1" ]; then
    echo "dbg"
fi
cd /opt/app || exit 1
RESULT=$(ls /tmp)

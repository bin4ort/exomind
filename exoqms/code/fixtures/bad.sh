#!/bin/sh
rm -rf $TMPDIR/cache
if [ $DEBUG = "1" ]; then
    echo "dbg"
fi
cd /opt/app
RESULT=`ls /tmp`

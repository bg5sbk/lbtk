#!/bin/sh

CLUSTER=oio-cluster
AWK=/usr/bin/awk

# CLI arguments parsing
NS=$1
SRVTYPE=$2
if [ -z $NS ] || [ -z $SRVTYPE ] ; then
	echo "Usage: $0 NS SRVTYPE" 1>&2
	exit 1
fi

while true ; do
	$CLUSTER -r $NS | $AWK -F\| "/\|$SRVTYPE\|/{print \$3}"
	echo
	sleep 1
done


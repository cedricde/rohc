#!/bin/sh
#
# file:        test_empty_payload.sh
# description: Check that the ROHC library correctly compresses/decompresses
#              IP/ROHC packets with empty payloads
# author:      Didier Barvaux <didier@barvaux.org>
#
# This script may be used by creating a link "test_empty_payload_PACKET.sh"
# where:
#    PACKET  is the packet type to check for, it is used to choose the source
#            capture located in the 'inputs' subdirectory.
#
# Script arguments:
#    test_empty_payload_PACKET.sh [verbose]
# where:
#   verbose          prints the traces of test application
#

# parse arguments
SCRIPT="$0"
VERBOSE="$1"
VERY_VERBOSE="$2"
if [ "x$MAKELEVEL" != "x" ] ; then
	BASEDIR="${srcdir}"
	APP="./test_empty_payload"
else
	BASEDIR=$( dirname "${SCRIPT}" )
	APP="${BASEDIR}/test_empty_payload"
fi

# extract the packet type from the name of the script
PACKET_TYPE=$( echo "${SCRIPT}" | \
               sed -e 's#^.*/test_empty_payload_\(.\+\)\.sh#\1#' )
CAPTURE_SOURCE="${BASEDIR}/inputs/${PACKET_TYPE}.pcap"

# check that capture exists
if [ ! -r "${CAPTURE_SOURCE}" ] ; then
	echo "source capture not found or not readable, please do not run $0 directly!"
	exit 1
fi

CMD="${APP} ${CAPTURE_SOURCE} ${PACKET_TYPE}"

# run in verbose mode or quiet mode
if [ "${VERBOSE}" = "verbose" ] ; then
	if [ "${VERY_VERBOSE}" = "verbose" ] ; then
		${CMD} || exit $?
	else
		${CMD} > /dev/null || exit $?
	fi
else
	${CMD} > /dev/null 2>&1 || exit $?
fi

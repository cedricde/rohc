#!/bin/sh
#
# file:        test_non_regression_kernel.sh
# description: Check that the behaviour of the ROHC library did not changed
#              without developpers noticing it (in Linux kernel).
# authors:     Didier Barvaux <didier.barvaux@toulouse.viveris.com>
#              Didier Barvaux <didier@barvaux.org>
#

cur_dir="$( dirname "$0" )"
tests_nr=0
statuses=0

for testfile in $( ls -1 ${cur_dir}/test_non_regression_*_maxcontexts0_wlsb4_smallcid.sh | grep -v tcp ) ; do

	echo -n "running ${testfile}... "
	tests_nr=$(( ${tests_nr} + 1 ))

	KERNEL_SUFFIX=_kernel ./${testfile} &>/dev/null
	ret=$?
	statuses=$(( ${statuses} + ${ret} ))

	if [ ${ret} -eq 0 ] ; then
		echo "OK"
	elif [ ${ret} -eq 1 ] ; then
		echo "FAIL"
	elif [ ${ret} -eq 77 ] ; then
		echo "SKIP"
	else
		echo "ERROR (${ret})"
	fi
done

echo "${tests_nr} tests performed, exiting with code ${statuses}"
exit ${statuses}


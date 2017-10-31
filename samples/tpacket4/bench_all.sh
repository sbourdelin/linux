#!/bin/bash

DIR=`dirname "${BASH_SOURCE[0]}"`

IF=p3p2
DURATION=60
CORE=14
ZC=17

echo "You might want to change the parameters in ${BASH_SOURCE[0]}"
echo "${IF} cpu${CORE} duration ${DURATION}s zc ${ZC}"

sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=2 --rxdrop
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=3 --rxdrop
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=4 --rxdrop
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=4 --rxdrop --zerocopy ${ZC}

sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=2 --txonly
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=3 --txonly
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=4 --txonly
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=4 --txonly --zerocopy ${ZC}

sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=2 --l2fwd
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=3 --l2fwd
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=4 --l2fwd
sudo taskset -c ${CORE} timeout -s int ${DURATION} ${DIR}/tpbench -i ${IF} --version=4 --l2fwd --zerocopy ${ZC}



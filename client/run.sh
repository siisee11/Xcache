#!/bin/bash

#SERVER_IP=$1
#SERVER_PORT=$2
SERVER_IP="115.145.173.67"
SERVER_PORT=2123

PMDFC_MODULE=pmdfc_net.ko
PMDFC_MOD=pmdfc_net

install_pmdfc_net() {
	insmod $PMDFC_MODULE ip=$SERVER_IP port=$SERVER_PORT
}

remove_pmdfc_net() {
    	rmmod $PMDFC_MOD
}

usage(){
    	echo "Usage:"
	echo "\t./run.sh 1 (install module)"
	echo "\t./run.sh 2 (remove module)"
}

install_module() {

	insmod $PMDFC_MODULE ip=$SERVER_IP port=$SERVER_PORT
	sleep 1
	insmod test/test_page.ko
#	insmod test/test_micro.ko

#	if ["$1" == "pmdfc_net.ko" ]; then
#    		insmod $1 ip=$SERVER_IP port=$SERVER_PORT
#	else
#	    	insmod $1
#	fi
}

remove_module() {
#    	rmmod pmdfc_test
    	rmmod test_page
	sleep .5
	rmmod pmdfc_net
}

if [ "$1" == "1" ]; then
	install_module
elif [ "$1" == "2" ]; then
	remove_module
else
	usage
fi

#set -e

#if [ "$1" == "1" ]; then
#	install_pmdfc_net
#elif [ "$1" == "2" ]; then
#	remove_pmdfc_net
#else
#	usage
#fi	

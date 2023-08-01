#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1
# Copyright (C) 2022, Google Inc, Steven Rostedt <rostedt@goodmis.org>
#
# This checks if any function is listed in a man page that is not listed
# in the main man page.

if [ $# -lt 1 ]; then
	echo "usage: check-manpages man-page-path"
	exit 1
fi

cd $1

MAIN=libccli
MAIN_FILE=${MAIN}.txt

HEADER_FILE=../include/ccli.h

PROCESSED=""

# Ignore man pages that do not contain functions
IGNORE=""

for man in ${MAIN}-*.txt; do

	for a in `sed -ne '/^NAME/,/^SYNOP/{/^[a-z]/{s/, *$//;s/,/\n/g;s/ //g;s/-.*$/-/;/-/{s/-//p;q};p}}' $man`; do
		if [ "${PROCESSED/:${a} /}" != "${PROCESSED}" ]; then
			P="${PROCESSED/:${a} */}"
			echo "Found ${a} in ${man} and in ${P/* /}"
		fi
		PROCESSED="${man}:${a} ${PROCESSED}"
		if [ "${IGNORE/$man/}" != "${IGNORE}" ]; then
			continue
		fi
		if ! grep -q '\*'${a}'\*' $MAIN_FILE; then
			if [ "$last" == "" ]; then
				echo
			fi
			if [ "$last" != "$man" ]; then
				echo "Missing functions from $MAIN_FILE that are in $man"
				last=$man
			fi
			echo "   ${a}"
		fi
	done
done

# The printk __attribute__ is being picked up
DEPRECATED="*__attribute__*"

sed -ne 's/^[a-z].*[ \*]\([a-z_][a-z_]*\)(.*/\1/p' -e 's/^\([a-z_][a-z_]*\)(.*/\1/p' ${HEADER_FILE} | while read f; do
	if ! grep -q '\*'${f}'\*' $MAIN_FILE; then
		if [ "${DEPRECATED/\*$f\*/}" != "${DEPRECATED}" ]; then
			continue;
		fi
		if [ "$last" == "" ]; then
			echo
			echo "Missing functions from $MAIN_FILE that are in ${HEADER_FILE}"
			last=$f
		fi
		echo "   ${f}"
	fi
done

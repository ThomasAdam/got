#!/bin/sh
#
# Copyright (c) 2019 Stefan Sperling <stsp@openbsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

. ./common.sh

function test_update_basic {
	local testroot=`test_init update_basic`

	got checkout $testroot/repo $testroot/wt > $testroot/stdout
	if [ "$?" != "0" ]; then
		test_done "$testroot" "$?"
		return 1
	fi

	echo "modified alpha" > $testroot/repo/alpha
	git_commit $testroot/repo -m "modified alpha"

	echo "U  alpha" > $testroot/stdout.expected
	echo -n "Updated to commit " >> $testroot/stdout.expected
	git_show_head $testroot/repo >> $testroot/stdout.expected
	echo >> $testroot/stdout.expected

	(cd $testroot/wt && got update > $testroot/stdout)

	cmp $testroot/stdout.expected $testroot/stdout
	if [ "$?" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$?"
		return 1
	fi

	echo "modified alpha" > $testroot/content.expected
	echo "beta" >> $testroot/content.expected
	echo "zeta" >> $testroot/content.expected
	echo "delta" >> $testroot/content.expected
	cat $testroot/wt/alpha $testroot/wt/beta $testroot/wt/epsilon/zeta \
	    $testroot/wt/gamma/delta > $testroot/content

	cmp $testroot/content.expected $testroot/content
	if [ "$?" != "0" ]; then
		diff -u $testroot/content.expected $testroot/content
	fi
	test_done "$testroot" "$?"
}

run_test test_update_basic

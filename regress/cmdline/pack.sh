#!/bin/sh
#
# Copyright (c) 2021 Stefan Sperling <stsp@openbsd.org>
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

# disable automatic packing for these tests
export GOT_TEST_PACK=""

test_pack_all_loose_objects() {
	local testroot=`test_init pack_all_loose_objects`

	# tags should also be packed
	got tag -r $testroot/repo -m 1.0 1.0 >/dev/null

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	gotadmin pack -r $testroot/repo > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi
	packname=`grep ^Wrote $testroot/stdout | cut -d ' ' -f2`
	gotadmin listpack $testroot/repo/.git/objects/pack/pack-$packname \
		> $testroot/stdout

	for d in $testroot/repo/.git/objects/[0-9a-f][0-9a-f]; do
		id0=`basename $d`
		ret=0
		for e in `ls $d`; do
			obj_id=${id0}${e}
			if grep -q ^$obj_id $testroot/stdout; then
				continue
			fi
			echo "loose object $obj_id was not packed" >&2
			ret=1
			break
		done
		if [ "$ret" == "1" ]; then
			break
		fi
	done

	test_done "$testroot" "$ret"
}

test_pack_exclude() {
	local testroot=`test_init pack_exclude`
	local commit0=`git_show_head $testroot/repo`

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	got branch -r $testroot/repo mybranch
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	got checkout -b mybranch $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo a new line >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "edit alpha" >/dev/null)

	gotadmin pack -r $testroot/repo -x master > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi
	packname=`grep ^Wrote $testroot/stdout | cut -d ' ' -f2`
	gotadmin listpack $testroot/repo/.git/objects/pack/pack-$packname \
		> $testroot/stdout

	tree0=`got cat -r $testroot/repo $commit0 | grep ^tree | \
		cut -d ' ' -f2`
	excluded_ids=`got tree -r $testroot/repo -c $commit0 -R -i | \
		cut -d ' ' -f 1`
	excluded_ids="$excluded_ids $commit0 $tree0"
	for id in $excluded_ids; do
		ret=0
		if grep -q ^$id $testroot/stdout; then
			echo "found excluded object $id in pack file" >&2
			ret=1
		fi
		if [ "$ret" == "1" ]; then
			break
		fi
	done
	if [ "$ret" == "1" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	for d in $testroot/repo/.git/objects/[0-9a-f][0-9a-f]; do
		id0=`basename $d`
		ret=0
		for e in `ls $d`; do
			obj_id=${id0}${e}
			excluded=0
			for id in $excluded_ids; do
				if [ "$obj_id" == "$id" ]; then
					excluded=1
					break
				fi
			done
			if [ "$excluded" == "1" ]; then
				continue
			fi
			if grep -q ^$obj_id $testroot/stdout; then
				continue
			fi
			echo "loose object $obj_id was not packed" >&2
			ret=1
			break
		done
		if [ "$ret" == "1" ]; then
			break
		fi
	done

	test_done "$testroot" "$ret"
}

test_pack_include() {
	local testroot=`test_init pack_include`
	local commit0=`git_show_head $testroot/repo`

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	got branch -r $testroot/repo mybranch
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	got checkout -b mybranch $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo a new line >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "edit alpha" >/dev/null)
	local commit1=`git_show_branch_head $testroot/repo mybranch`

	gotadmin pack -r $testroot/repo master > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi
	packname=`grep ^Wrote $testroot/stdout | cut -d ' ' -f2`
	gotadmin listpack $testroot/repo/.git/objects/pack/pack-$packname \
		> $testroot/stdout

	tree1=`got cat -r $testroot/repo $commit1 | grep ^tree | \
		cut -d ' ' -f2`
	alpha1=`got tree -r $testroot/repo -i -c $commit1 | \
		grep "[0-9a-f] alpha$" | cut -d' ' -f 1`
	excluded_ids="$alpha1 $commit1 $tree1"
	for id in $excluded_ids; do
		ret=0
		if grep -q ^$id $testroot/stdout; then
			echo "found excluded object $id in pack file" >&2
			ret=1
		fi
		if [ "$ret" == "1" ]; then
			break
		fi
	done
	if [ "$ret" == "1" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	tree0=`got cat -r $testroot/repo $commit0 | grep ^tree | \
		cut -d ' ' -f2`
	included_ids=`got tree -r $testroot/repo -c $commit0 -R -i | \
		cut -d ' ' -f 1`
	included_ids="$included_ids $commit0 $tree0"
	for obj_id in $included_ids; do
		for id in $excluded_ids; do
			if [ "$obj_id" == "$id" ]; then
				excluded=1
				break
			fi
		done
		if [ "$excluded" == "1" ]; then
			continue
		fi
		if grep -q ^$obj_id $testroot/stdout; then
			continue
		fi
		echo "included object $obj_id was not packed" >&2
		ret=1
		break
	done

	test_done "$testroot" "$ret"
}

test_pack_ambiguous_arg() {
	local testroot=`test_init pack_ambiguous_arg`
	local commit0=`git_show_head $testroot/repo`

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	got branch -r $testroot/repo mybranch
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	got checkout -b mybranch $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo a new line >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "edit alpha" >/dev/null)
	local commit1=`git_show_branch_head $testroot/repo mybranch`

	gotadmin pack -r $testroot/repo -x master master \
		> $testroot/stdout 2> $testroot/stderr
	ret="$?"
	if [ "$ret" == "0" ]; then
		echo "gotadmin pack succeeded unexpectedly" >&2
		test_done "$testroot" "1"
		return 1
	fi

	printf "\rpacking 1 reference\n" > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "gotadmin: not enough objects to pack" > $testroot/stderr.expected
	cmp -s $testroot/stderr.expected $testroot/stderr
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stderr.expected $testroot/stderr
	fi
	test_done "$testroot" "$ret"
}

test_pack_loose_only() {
	local testroot=`test_init pack_loose_only`
	local commit0=`git_show_head $testroot/repo`

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	got branch -r $testroot/repo mybranch
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	got checkout -b mybranch $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo a new line >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "edit alpha" >/dev/null)

	# pack objects belonging to the 'master' branch; its objects
	# should then be excluded while packing 'mybranch' since they
	# are already packed
	gotadmin pack -r $testroot/repo master > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi

	gotadmin pack -r $testroot/repo mybranch > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi
	packname=`grep ^Wrote $testroot/stdout | cut -d ' ' -f2`
	gotadmin listpack $testroot/repo/.git/objects/pack/pack-$packname \
		> $testroot/stdout

	tree0=`got cat -r $testroot/repo $commit0 | grep ^tree | \
		cut -d ' ' -f2`
	excluded_ids=`got tree -r $testroot/repo -c $commit0 -R -i | \
		cut -d ' ' -f 1`
	excluded_ids="$excluded_ids $commit0 $tree0"
	for id in $excluded_ids; do
		ret=0
		if grep -q ^$id $testroot/stdout; then
			echo "found excluded object $id in pack file" >&2
			ret=1
		fi
		if [ "$ret" == "1" ]; then
			break
		fi
	done
	if [ "$ret" == "1" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	for d in $testroot/repo/.git/objects/[0-9a-f][0-9a-f]; do
		id0=`basename $d`
		ret=0
		for e in `ls $d`; do
			obj_id=${id0}${e}
			excluded=0
			for id in $excluded_ids; do
				if [ "$obj_id" == "$id" ]; then
					excluded=1
					break
				fi
			done
			if [ "$excluded" == "1" ]; then
				continue
			fi
			if grep -q ^$obj_id $testroot/stdout; then
				continue
			fi
			echo "loose object $obj_id was not packed" >&2
			ret=1
			break
		done
		if [ "$ret" == "1" ]; then
			break
		fi
	done

	test_done "$testroot" "$ret"
}

test_pack_all_objects() {
	local testroot=`test_init pack_all_objects`
	local commit0=`git_show_head $testroot/repo`

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	got branch -r $testroot/repo mybranch
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	got checkout -b mybranch $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo a new line >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "edit alpha" >/dev/null)

	# pack objects belonging to the 'master' branch
	gotadmin pack -r $testroot/repo master > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi

	# pack mybranch, including already packed objects on the
	# 'master' branch which are reachable from mybranch
	gotadmin pack -r $testroot/repo -a mybranch > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "gotadmin pack failed unexpectedly" >&2
		test_done "$testroot" "$ret"
		return 1
	fi
	packname=`grep ^Wrote $testroot/stdout | cut -d ' ' -f2`
	gotadmin listpack $testroot/repo/.git/objects/pack/pack-$packname \
		> $testroot/stdout

	for d in $testroot/repo/.git/objects/[0-9a-f][0-9a-f]; do
		id0=`basename $d`
		ret=0
		for e in `ls $d`; do
			obj_id=${id0}${e}
			if grep -q ^$obj_id $testroot/stdout; then
				continue
			fi
			echo "loose object $obj_id was not packed" >&2
			ret=1
			break
		done
		if [ "$ret" == "1" ]; then
			break
		fi
	done

	test_done "$testroot" "$ret"
}

test_pack_bad_ref() {
	local testroot=`test_init pack_bad_ref`
	local commit0=`git_show_head $testroot/repo`

	# no pack files should exist yet
	ls $testroot/repo/.git/objects/pack/ > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	got branch -r $testroot/repo mybranch
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	got checkout -b mybranch $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	gotadmin pack -r $testroot/repo refs/got/worktree/ \
		> $testroot/stdout 2> $testroot/stderr
	ret="$?"
	if [ "$ret" == "0" ]; then
		echo "gotadmin pack succeeded unexpectedly" >&2
		test_done "$testroot" "1"
		return 1
	fi

	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "gotadmin: not enough objects to pack" > $testroot/stderr.expected
	cmp -s $testroot/stderr.expected $testroot/stderr
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stderr.expected $testroot/stderr
	fi
	test_done "$testroot" "$ret"
}

test_parseargs "$@"
run_test test_pack_all_loose_objects
run_test test_pack_exclude
run_test test_pack_include
run_test test_pack_ambiguous_arg
run_test test_pack_loose_only
run_test test_pack_all_objects
run_test test_pack_bad_ref

#!/bin/sh
#
# Copyright (c) 2019, 2020 Stefan Sperling <stsp@openbsd.org>
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

regress_run_only=""

export GIT_AUTHOR_NAME="Flan Hacker"
export GIT_AUTHOR_EMAIL="flan_hacker@openbsd.org"
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
export GOT_AUTHOR="$GIT_AUTHOR_NAME <$GIT_AUTHOR_EMAIL>"
export GOT_AUTHOR_8="flan_hac"
export GOT_AUTHOR_11="flan_hacker"
export GOT_LOG_DEFAULT_LIMIT=0
export GOT_TEST_ROOT="/tmp"
export GOT_IGNORE_GITCONFIG=1
export GOT_VERSION_STR=`got --version | cut -d ' ' -f2`

if [ -z "$GOT_TEST_HTTP_PORT" ]; then
	export GOT_TEST_HTTP_PORT=8080
fi

export LC_ALL=C

export MALLOC_OPTIONS=S

git_init()
{
	git init -q "$1"

	# Switch the default branch to match our test expectations if needed.
	# Only need to change HEAD since 'git init' did not create any refs.
	# Relying on implementation details of 'git init' is no problem for us.
	# We want to be alerted when Git changes fundamental assumptions such
	# as what an empty repository looks like and where the default branch
	# is set. In such cases Got's own tooling might well need to change
	# its behaviour, too, and our tests should fail.
	# TODO: Update all tests to assume 'main' instead of 'master' and
	# switch to main here, to match Got's own default.
	echo "ref: refs/heads/master" > "$1/.git/HEAD"
}

maybe_pack_repo()
{
	local repo="$1"
	if [ -n "$GOT_TEST_PACK" ]; then
		arg=""
		if [ "$GOT_TEST_PACK" = "ref-delta" ]; then
			arg="-D"
		fi

		(cd $repo && gotadmin pack -a $arg > /dev/null)
		(cd $repo && gotadmin cleanup -a -q)
	fi
}

git_commit()
{
	local repo="$1"
	shift
	git -C $repo commit --author="$GOT_AUTHOR" -q -a "$@"
	maybe_pack_repo $repo
}

git_rm()
{
	local repo="$1"
	shift
	git -C $repo rm -q "$@"
}

git_rmdir()
{
	local repo="$1"
	shift
	git -C $repo rm -q -r "$@"
}

git_show_head()
{
	local repo="$1"
	git -C $repo show --no-patch --pretty='format:%H'
}

git_show_branch_head()
{
	local repo="$1"
	local branch="$2"
	git -C $repo show --no-patch --pretty='format:%H' $branch
}


git_show_author_time()
{
	local repo="$1"
	local object="$2"
	git -C $repo show --no-patch --pretty='format:%at' $object
}

git_show_tagger_time()
{
	local repo="$1"
	local tag="$2"
	git -C $repo cat-file tag $tag | grep ^tagger | \
		sed -e "s/^tagger $GOT_AUTHOR//" | cut -d' ' -f2
}

git_show_parent_commit()
{
	local repo="$1"
	local commit="$2"
	git -C $repo show --no-patch --pretty='format:%P' $commit
}

git_show_tree()
{
	local repo="$1"
	git -C $repo show --no-patch --pretty='format:%T'
}

trim_obj_id()
{
	local trimcount=$1
	local id=$2

	local pat=""
	while [ "$trimcount" -gt 0 ]; do
		pat="[0-9a-f]$pat"
		trimcount=$((trimcount - 1))
	done

	echo ${id%$pat}
}

pop_idx()
{
	shift "$1"
	printf '%s' "${1:-index-out-of-bounds}"
}

git_commit_tree()
{
	local repo="$1"
	local msg="$2"
	local tree="$3"
	git -C $repo commit-tree -m "$msg" "$tree"
}

git_fsck()
{
	local testroot="$1"
	local repo="$2"

	git -C $repo fsck --strict \
		> $testroot/fsck.stdout 2> $testroot/fsck.stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo -n "git fsck: "
		cat $testroot/fsck.stderr
		echo "git fsck failed; leaving test data in $testroot"
		return 1
	fi

	return 0
}

make_test_tree()
{
	repo="$1"

	echo alpha > $repo/alpha
	echo beta > $repo/beta
	mkdir $repo/gamma
	echo delta > $repo/gamma/delta
	mkdir $repo/epsilon
	echo zeta > $repo/epsilon/zeta
}

make_single_file_repo()
{
	repo="$1"
	file="$2"

	mkdir $repo
	git_init $repo
	echo "this is file $file" > $repo/$file
	git -C $repo add .
	git_commit $repo -m "intialize $repo with file $file"
}

get_loose_object_path()
{
	local repo="$1"
	local id="$2"
	local id0=`trim_obj_id 38 $id`
	local idrest=`echo ${id#[0-9a-f][0-9a-f]}`
	echo "$repo/.git/objects/$id0/$idrest"
}

get_blob_id()
{
	repo="$1"
	tree_path="$2"
	filename="$3"

	got tree -r $repo -i $tree_path | grep "[0-9a-f] ${filename}$" | \
		cut -d' ' -f 1
}

test_init()
{
	local testname="$1"
	local no_tree="$2"
	if [ -z "$testname" ]; then
		echo "No test name provided" >&2
		return 1
	fi
	local testroot=`mktemp -d \
	    "$GOT_TEST_ROOT/got-test-$testname-XXXXXXXXXX"`
	mkdir $testroot/repo
	git_init $testroot/repo
	if [ -z "$no_tree" ]; then
		make_test_tree $testroot/repo
		git -C $repo add .
		git_commit $testroot/repo -m "adding the test tree"
	fi
	touch $testroot/repo/.git/git-daemon-export-ok
	echo "$testroot"
}

test_cleanup()
{
	local testroot="$1"

	git_fsck $testroot $testroot/repo
	ret=$?
	if [ $ret -ne 0 ]; then
		return $ret
	fi

	rm -rf "$testroot"
}

test_parseargs()
{
	while getopts qr: flag; do
		case $flag in
		q)	export GOT_TEST_QUIET=1
			;;
		r)	export GOT_TEST_ROOT=${OPTARG%/}
			;;
		?)	echo "Supported options:"
			echo "  -q: quiet mode"
			echo "  -r PATH: use PATH as test data root directory"
			exit 2
			;;
		esac
	done
	shift $(($OPTIND - 1))
	regress_run_only="$@"
} >&2

run_test()
{
	testfunc="$1"

	if [ -n "$regress_run_only" ]; then
		case "$regress_run_only" in
		*$testfunc*) ;;
		*) return ;;
		esac
	fi

	if [ -z "$GOT_TEST_QUIET" ]; then
		echo -n "$testfunc "
	fi
	$testfunc
}

test_done()
{
	local testroot="$1"
	local result="$2"
	if [ "$result" = "0" ]; then
		test_cleanup "$testroot" || return 1
		if [ -z "$GOT_TEST_QUIET" ]; then
			echo "ok"
		fi
	elif echo "$result" | grep -q "^xfail"; then
		# expected test failure; test reproduces an unfixed bug
		echo "$result"
		test_cleanup "$testroot" || return 1
	else
		echo "test failed; leaving test data in $testroot"
	fi
}

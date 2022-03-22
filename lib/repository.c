/*
 * Copyright (c) 2018, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <ctype.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <errno.h>
#include <libgen.h>
#include <stdint.h>

#include "bloom.h"

#include "got_error.h"
#include "got_reference.h"
#include "got_repository.h"
#include "got_path.h"
#include "got_cancel.h"
#include "got_object.h"

#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_object_parse.h"
#include "got_lib_object_create.h"
#include "got_lib_pack.h"
#include "got_lib_privsep.h"
#include "got_lib_sha1.h"
#include "got_lib_object_cache.h"
#include "got_lib_repository.h"
#include "got_lib_gotconfig.h"

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

RB_PROTOTYPE(got_packidx_bloom_filter_tree, got_packidx_bloom_filter, entry,
    got_packidx_bloom_filter_cmp);

const char *
got_repo_get_path(struct got_repository *repo)
{
	return repo->path;
}

const char *
got_repo_get_path_git_dir(struct got_repository *repo)
{
	return repo->path_git_dir;
}

int
got_repo_get_fd(struct got_repository *repo)
{
	return repo->gitdir_fd;
}

const char *
got_repo_get_gitconfig_author_name(struct got_repository *repo)
{
	return repo->gitconfig_author_name;
}

const char *
got_repo_get_gitconfig_author_email(struct got_repository *repo)
{
	return repo->gitconfig_author_email;
}

const char *
got_repo_get_global_gitconfig_author_name(struct got_repository *repo)
{
	return repo->global_gitconfig_author_name;
}

const char *
got_repo_get_global_gitconfig_author_email(struct got_repository *repo)
{
	return repo->global_gitconfig_author_email;
}

const char *
got_repo_get_gitconfig_owner(struct got_repository *repo)
{
	return repo->gitconfig_owner;
}

void
got_repo_get_gitconfig_extensions(char ***extensions, int *nextensions,
    struct got_repository *repo)
{
	*extensions = repo->extensions;
	*nextensions = repo->nextensions;
}

int
got_repo_is_bare(struct got_repository *repo)
{
	return (strcmp(repo->path, repo->path_git_dir) == 0);
}

static char *
get_path_git_child(struct got_repository *repo, const char *basename)
{
	char *path_child;

	if (asprintf(&path_child, "%s/%s", repo->path_git_dir,
	    basename) == -1)
		return NULL;

	return path_child;
}

char *
got_repo_get_path_objects(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_OBJECTS_DIR);
}

char *
got_repo_get_path_objects_pack(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_OBJECTS_PACK_DIR);
}

char *
got_repo_get_path_refs(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_REFS_DIR);
}

char *
got_repo_get_path_packed_refs(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_PACKED_REFS_FILE);
}

static char *
get_path_head(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_HEAD_FILE);
}

char *
got_repo_get_path_gitconfig(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_GITCONFIG);
}

char *
got_repo_get_path_gotconfig(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_GOTCONFIG_FILENAME);
}

const struct got_gotconfig *
got_repo_get_gotconfig(struct got_repository *repo)
{
	return repo->gotconfig;
}

void
got_repo_get_gitconfig_remotes(int *nremotes,
    const struct got_remote_repo **remotes, struct got_repository *repo)
{
	*nremotes = repo->ngitconfig_remotes;
	*remotes = repo->gitconfig_remotes;
}

static int
is_git_repo(struct got_repository *repo)
{
	const char *path_git = got_repo_get_path_git_dir(repo);
	char *path_objects = got_repo_get_path_objects(repo);
	char *path_refs = got_repo_get_path_refs(repo);
	char *path_head = get_path_head(repo);
	int ret = 0;
	struct stat sb;
	struct got_reference *head_ref;

	if (lstat(path_git, &sb) == -1)
		goto done;
	if (!S_ISDIR(sb.st_mode))
		goto done;

	if (lstat(path_objects, &sb) == -1)
		goto done;
	if (!S_ISDIR(sb.st_mode))
		goto done;

	if (lstat(path_refs, &sb) == -1)
		goto done;
	if (!S_ISDIR(sb.st_mode))
		goto done;

	if (lstat(path_head, &sb) == -1)
		goto done;
	if (!S_ISREG(sb.st_mode))
		goto done;

	/* Check if the HEAD reference can be opened. */
	if (got_ref_open(&head_ref, repo, GOT_REF_HEAD, 0) != NULL)
		goto done;
	got_ref_close(head_ref);

	ret = 1;
done:
	free(path_objects);
	free(path_refs);
	free(path_head);
	return ret;

}

const struct got_error *
got_repo_cache_object(struct got_repository *repo, struct got_object_id *id,
    struct got_object *obj)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = got_object_cache_add(&repo->objcache, id, obj);
	if (err) {
		if (err->code == GOT_ERR_OBJ_EXISTS ||
		    err->code == GOT_ERR_OBJ_TOO_LARGE)
			err = NULL;
		return err;
	}
	obj->refcnt++;
#endif
	return NULL;
}

struct got_object *
got_repo_get_cached_object(struct got_repository *repo,
    struct got_object_id *id)
{
	return (struct got_object *)got_object_cache_get(&repo->objcache, id);
}

const struct got_error *
got_repo_cache_tree(struct got_repository *repo, struct got_object_id *id,
    struct got_tree_object *tree)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = got_object_cache_add(&repo->treecache, id, tree);
	if (err) {
		if (err->code == GOT_ERR_OBJ_EXISTS ||
		    err->code == GOT_ERR_OBJ_TOO_LARGE)
			err = NULL;
		return err;
	}
	tree->refcnt++;
#endif
	return NULL;
}

struct got_tree_object *
got_repo_get_cached_tree(struct got_repository *repo,
    struct got_object_id *id)
{
	return (struct got_tree_object *)got_object_cache_get(
	    &repo->treecache, id);
}

const struct got_error *
got_repo_cache_commit(struct got_repository *repo, struct got_object_id *id,
    struct got_commit_object *commit)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = got_object_cache_add(&repo->commitcache, id, commit);
	if (err) {
		if (err->code == GOT_ERR_OBJ_EXISTS ||
		    err->code == GOT_ERR_OBJ_TOO_LARGE)
			err = NULL;
		return err;
	}
	commit->refcnt++;
#endif
	return NULL;
}

struct got_commit_object *
got_repo_get_cached_commit(struct got_repository *repo,
    struct got_object_id *id)
{
	return (struct got_commit_object *)got_object_cache_get(
	    &repo->commitcache, id);
}

const struct got_error *
got_repo_cache_tag(struct got_repository *repo, struct got_object_id *id,
    struct got_tag_object *tag)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = got_object_cache_add(&repo->tagcache, id, tag);
	if (err) {
		if (err->code == GOT_ERR_OBJ_EXISTS ||
		    err->code == GOT_ERR_OBJ_TOO_LARGE)
			err = NULL;
		return err;
	}
	tag->refcnt++;
#endif
	return NULL;
}

struct got_tag_object *
got_repo_get_cached_tag(struct got_repository *repo, struct got_object_id *id)
{
	return (struct got_tag_object *)got_object_cache_get(
	    &repo->tagcache, id);
}

const struct got_error *
got_repo_cache_raw_object(struct got_repository *repo, struct got_object_id *id,
    struct got_raw_object *raw)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = got_object_cache_add(&repo->rawcache, id, raw);
	if (err) {
		if (err->code == GOT_ERR_OBJ_EXISTS ||
		    err->code == GOT_ERR_OBJ_TOO_LARGE)
			err = NULL;
		return err;
	}
	raw->refcnt++;
#endif
	return NULL;
}


struct got_raw_object *
got_repo_get_cached_raw_object(struct got_repository *repo,
    struct got_object_id *id)
{
	return (struct got_raw_object *)got_object_cache_get(&repo->rawcache, id);
}


static const struct got_error *
open_repo(struct got_repository *repo, const char *path)
{
	const struct got_error *err = NULL;

	repo->gitdir_fd = -1;

	/* bare git repository? */
	repo->path_git_dir = strdup(path);
	if (repo->path_git_dir == NULL)
		return got_error_from_errno("strdup");
	if (is_git_repo(repo)) {
		repo->path = strdup(repo->path_git_dir);
		if (repo->path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
		repo->gitdir_fd = open(repo->path_git_dir,
		    O_DIRECTORY | O_CLOEXEC);
		if (repo->gitdir_fd == -1) {
			err = got_error_from_errno2("open",
			    repo->path_git_dir);
			goto done;
		}
		return NULL;
	}

	/* git repository with working tree? */
	free(repo->path_git_dir);
	repo->path_git_dir = NULL;
	if (asprintf(&repo->path_git_dir, "%s/%s", path, GOT_GIT_DIR) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}
	if (is_git_repo(repo)) {
		repo->path = strdup(path);
		if (repo->path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
		repo->gitdir_fd = open(repo->path_git_dir,
		    O_DIRECTORY | O_CLOEXEC);
		if (repo->gitdir_fd == -1) {
			err = got_error_from_errno2("open",
			    repo->path_git_dir);
			goto done;
		}
		return NULL;
	}

	err = got_error(GOT_ERR_NOT_GIT_REPO);
done:
	if (err) {
		free(repo->path);
		repo->path = NULL;
		free(repo->path_git_dir);
		repo->path_git_dir = NULL;
		if (repo->gitdir_fd != -1)
			close(repo->gitdir_fd);
		repo->gitdir_fd = -1;

	}
	return err;
}

static const struct got_error *
parse_gitconfig_file(int *gitconfig_repository_format_version,
    char **gitconfig_author_name, char **gitconfig_author_email,
    struct got_remote_repo **remotes, int *nremotes,
    char **gitconfig_owner, char ***extensions, int *nextensions,
    const char *gitconfig_path)
{
	const struct got_error *err = NULL, *child_err = NULL;
	int fd = -1;
	int imsg_fds[2] = { -1, -1 };
	pid_t pid;
	struct imsgbuf *ibuf;

	*gitconfig_repository_format_version = 0;
	if (extensions)
		*extensions = NULL;
	if (nextensions)
		*nextensions = 0;
	*gitconfig_author_name = NULL;
	*gitconfig_author_email = NULL;
	if (remotes)
		*remotes = NULL;
	if (nremotes)
		*nremotes = 0;
	if (gitconfig_owner)
		*gitconfig_owner = NULL;

	fd = open(gitconfig_path, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		if (errno == ENOENT)
			return NULL;
		return got_error_from_errno2("open", gitconfig_path);
	}

	ibuf = calloc(1, sizeof(*ibuf));
	if (ibuf == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, imsg_fds) == -1) {
		err = got_error_from_errno("socketpair");
		goto done;
	}

	pid = fork();
	if (pid == -1) {
		err = got_error_from_errno("fork");
		goto done;
	} else if (pid == 0) {
		got_privsep_exec_child(imsg_fds, GOT_PATH_PROG_READ_GITCONFIG,
		    gitconfig_path);
		/* not reached */
	}

	if (close(imsg_fds[1]) == -1) {
		err = got_error_from_errno("close");
		goto done;
	}
	imsg_fds[1] = -1;
	imsg_init(ibuf, imsg_fds[0]);

	err = got_privsep_send_gitconfig_parse_req(ibuf, fd);
	if (err)
		goto done;
	fd = -1;

	err = got_privsep_send_gitconfig_repository_format_version_req(ibuf);
	if (err)
		goto done;

	err = got_privsep_recv_gitconfig_int(
	    gitconfig_repository_format_version, ibuf);
	if (err)
		goto done;

	if (extensions && nextensions) {
		err = got_privsep_send_gitconfig_repository_extensions_req(
		    ibuf);
		if (err)
			goto done;
		err = got_privsep_recv_gitconfig_int(nextensions, ibuf);
		if (err)
			goto done;
		if (*nextensions > 0) {
			int i;
			*extensions = calloc(*nextensions, sizeof(char *));
			if (*extensions == NULL) {
				err = got_error_from_errno("calloc");
				goto done;
			}
			for (i = 0; i < *nextensions; i++) {
				char *ext;
				err = got_privsep_recv_gitconfig_str(&ext,
				    ibuf);
				if (err)
					goto done;
				(*extensions)[i] = ext;
			}
		}
	}

	err = got_privsep_send_gitconfig_author_name_req(ibuf);
	if (err)
		goto done;

	err = got_privsep_recv_gitconfig_str(gitconfig_author_name, ibuf);
	if (err)
		goto done;

	err = got_privsep_send_gitconfig_author_email_req(ibuf);
	if (err)
		goto done;

	err = got_privsep_recv_gitconfig_str(gitconfig_author_email, ibuf);
	if (err)
		goto done;

	if (remotes && nremotes) {
		err = got_privsep_send_gitconfig_remotes_req(ibuf);
		if (err)
			goto done;

		err = got_privsep_recv_gitconfig_remotes(remotes,
		    nremotes, ibuf);
		if (err)
			goto done;
	}

	if (gitconfig_owner) {
		err = got_privsep_send_gitconfig_owner_req(ibuf);
		if (err)
			goto done;
		err = got_privsep_recv_gitconfig_str(gitconfig_owner, ibuf);
		if (err)
			goto done;
	}

	err = got_privsep_send_stop(imsg_fds[0]);
	child_err = got_privsep_wait_for_child(pid);
	if (child_err && err == NULL)
		err = child_err;
done:
	if (imsg_fds[0] != -1 && close(imsg_fds[0]) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (imsg_fds[1] != -1 && close(imsg_fds[1]) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno2("close", gitconfig_path);
	free(ibuf);
	return err;
}

static const struct got_error *
read_gitconfig(struct got_repository *repo, const char *global_gitconfig_path)
{
	const struct got_error *err = NULL;
	char *repo_gitconfig_path = NULL;

	if (global_gitconfig_path) {
		/* Read settings from ~/.gitconfig. */
		int dummy_repo_version;
		err = parse_gitconfig_file(&dummy_repo_version,
		    &repo->global_gitconfig_author_name,
		    &repo->global_gitconfig_author_email,
		    NULL, NULL, NULL, NULL, NULL, global_gitconfig_path);
		if (err)
			return err;
	}

	/* Read repository's .git/config file. */
	repo_gitconfig_path = got_repo_get_path_gitconfig(repo);
	if (repo_gitconfig_path == NULL)
		return got_error_from_errno("got_repo_get_path_gitconfig");

	err = parse_gitconfig_file(&repo->gitconfig_repository_format_version,
	    &repo->gitconfig_author_name, &repo->gitconfig_author_email,
	    &repo->gitconfig_remotes, &repo->ngitconfig_remotes,
	    &repo->gitconfig_owner, &repo->extensions, &repo->nextensions,
	    repo_gitconfig_path);
	if (err)
		goto done;
done:
	free(repo_gitconfig_path);
	return err;
}

static const struct got_error *
read_gotconfig(struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *gotconfig_path;

	gotconfig_path = got_repo_get_path_gotconfig(repo);
	if (gotconfig_path == NULL)
		return got_error_from_errno("got_repo_get_path_gotconfig");

	err = got_gotconfig_read(&repo->gotconfig, gotconfig_path);
	free(gotconfig_path);
	return err;
}

/* Supported repository format extensions. */
static const char *repo_extensions[] = {
	"noop",			/* Got supports repository format version 1. */
	"preciousObjects",	/* Supported by gotadmin cleanup. */
	"worktreeConfig",	/* Got does not care about Git work trees. */
};

const struct got_error *
got_repo_open(struct got_repository **repop, const char *path,
    const char *global_gitconfig_path)
{
	struct got_repository *repo = NULL;
	const struct got_error *err = NULL;
	char *repo_path = NULL;
	size_t i;
	struct rlimit rl;

	*repop = NULL;

	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		return got_error_from_errno("getrlimit");

	repo = calloc(1, sizeof(*repo));
	if (repo == NULL)
		return got_error_from_errno("calloc");

	RB_INIT(&repo->packidx_bloom_filters);
	TAILQ_INIT(&repo->packidx_paths);

	for (i = 0; i < nitems(repo->privsep_children); i++) {
		memset(&repo->privsep_children[i], 0,
		    sizeof(repo->privsep_children[0]));
		repo->privsep_children[i].imsg_fd = -1;
	}

	err = got_object_cache_init(&repo->objcache,
	    GOT_OBJECT_CACHE_TYPE_OBJ);
	if (err)
		goto done;
	err = got_object_cache_init(&repo->treecache,
	    GOT_OBJECT_CACHE_TYPE_TREE);
	if (err)
		goto done;
	err = got_object_cache_init(&repo->commitcache,
	    GOT_OBJECT_CACHE_TYPE_COMMIT);
	if (err)
		goto done;
	err = got_object_cache_init(&repo->tagcache,
	    GOT_OBJECT_CACHE_TYPE_TAG);
	if (err)
		goto done;
	err = got_object_cache_init(&repo->rawcache,
	    GOT_OBJECT_CACHE_TYPE_RAW);
	if (err)
		goto done;

	repo->pack_cache_size = GOT_PACK_CACHE_SIZE;
	if (repo->pack_cache_size > rl.rlim_cur / 8)
		repo->pack_cache_size = rl.rlim_cur / 8;

	repo_path = realpath(path, NULL);
	if (repo_path == NULL) {
		err = got_error_from_errno2("realpath", path);
		goto done;
	}

	for (;;) {
		char *parent_path;

		err = open_repo(repo, repo_path);
		if (err == NULL)
			break;
		if (err->code != GOT_ERR_NOT_GIT_REPO)
			goto done;
		if (repo_path[0] == '/' && repo_path[1] == '\0') {
			err = got_error(GOT_ERR_NOT_GIT_REPO);
			goto done;
		}
		err = got_path_dirname(&parent_path, repo_path);
		if (err)
			goto done;
		free(repo_path);
		repo_path = parent_path;
	}

	err = read_gotconfig(repo);
	if (err)
		goto done;

	err = read_gitconfig(repo, global_gitconfig_path);
	if (err)
		goto done;
	if (repo->gitconfig_repository_format_version != 0)
		err = got_error_path(path, GOT_ERR_GIT_REPO_FORMAT);
	for (i = 0; i < repo->nextensions; i++) {
		char *ext = repo->extensions[i];
		int j, supported = 0;
		for (j = 0; j < nitems(repo_extensions); j++) {
			if (strcmp(ext, repo_extensions[j]) == 0) {
				supported = 1;
				break;
			}
		}
		if (!supported) {
			err = got_error_path(ext, GOT_ERR_GIT_REPO_EXT);
			goto done;
		}
	}

	err = got_repo_list_packidx(&repo->packidx_paths, repo);
done:
	if (err)
		got_repo_close(repo);
	else
		*repop = repo;
	free(repo_path);
	return err;
}

const struct got_error *
got_repo_close(struct got_repository *repo)
{
	const struct got_error *err = NULL, *child_err;
	struct got_packidx_bloom_filter *bf;
	struct got_pathlist_entry *pe;
	size_t i;

	for (i = 0; i < repo->pack_cache_size; i++) {
		if (repo->packidx_cache[i] == NULL)
			break;
		got_packidx_close(repo->packidx_cache[i]);
	}

	while ((bf = RB_MIN(got_packidx_bloom_filter_tree,
	    &repo->packidx_bloom_filters))) {
		RB_REMOVE(got_packidx_bloom_filter_tree,
		    &repo->packidx_bloom_filters, bf);
		free(bf->bloom);
		free(bf);
	}

	for (i = 0; i < repo->pack_cache_size; i++) {
		if (repo->packs[i].path_packfile == NULL)
			break;
		got_pack_close(&repo->packs[i]);
	}

	free(repo->path);
	free(repo->path_git_dir);

	got_object_cache_close(&repo->objcache);
	got_object_cache_close(&repo->treecache);
	got_object_cache_close(&repo->commitcache);
	got_object_cache_close(&repo->tagcache);
	got_object_cache_close(&repo->rawcache);

	for (i = 0; i < nitems(repo->privsep_children); i++) {
		if (repo->privsep_children[i].imsg_fd == -1)
			continue;
		imsg_clear(repo->privsep_children[i].ibuf);
		free(repo->privsep_children[i].ibuf);
		err = got_privsep_send_stop(repo->privsep_children[i].imsg_fd);
		child_err = got_privsep_wait_for_child(
		    repo->privsep_children[i].pid);
		if (child_err && err == NULL)
			err = child_err;
		if (close(repo->privsep_children[i].imsg_fd) == -1 &&
		    err == NULL)
			err = got_error_from_errno("close");
	}

	if (repo->gitdir_fd != -1 && close(repo->gitdir_fd) == -1 &&
	    err == NULL)
		err = got_error_from_errno("close");

	if (repo->gotconfig)
		got_gotconfig_free(repo->gotconfig);
	free(repo->gitconfig_author_name);
	free(repo->gitconfig_author_email);
	for (i = 0; i < repo->ngitconfig_remotes; i++)
		got_repo_free_remote_repo_data(&repo->gitconfig_remotes[i]);
	free(repo->gitconfig_remotes);
	for (i = 0; i < repo->nextensions; i++)
		free(repo->extensions[i]);
	free(repo->extensions);

	TAILQ_FOREACH(pe, &repo->packidx_paths, entry)
		free((void *)pe->path);
	got_pathlist_free(&repo->packidx_paths);
	free(repo);

	return err;
}

void
got_repo_free_remote_repo_data(struct got_remote_repo *repo)
{
	int i;

	free(repo->name);
	repo->name = NULL;
	free(repo->fetch_url);
	repo->fetch_url = NULL;
	free(repo->send_url);
	repo->send_url = NULL;
	for (i = 0; i < repo->nfetch_branches; i++)
		free(repo->fetch_branches[i]);
	free(repo->fetch_branches);
	repo->fetch_branches = NULL;
	repo->nfetch_branches = 0;
	for (i = 0; i < repo->nsend_branches; i++)
		free(repo->send_branches[i]);
	free(repo->send_branches);
	repo->send_branches = NULL;
	repo->nsend_branches = 0;
}

const struct got_error *
got_repo_map_path(char **in_repo_path, struct got_repository *repo,
    const char *input_path)
{
	const struct got_error *err = NULL;
	const char *repo_abspath = NULL;
	size_t repolen, len;
	char *canonpath, *path = NULL;

	*in_repo_path = NULL;

	canonpath = strdup(input_path);
	if (canonpath == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}
	err = got_canonpath(input_path, canonpath, strlen(canonpath) + 1);
	if (err)
		goto done;

	repo_abspath = got_repo_get_path(repo);

	if (canonpath[0] == '\0') {
		path = strdup(canonpath);
		if (path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	} else {
		path = realpath(canonpath, NULL);
		if (path == NULL) {
			if (errno != ENOENT) {
				err = got_error_from_errno2("realpath",
				    canonpath);
				goto done;
			}
			/*
			 * Path is not on disk.
			 * Assume it is already relative to repository root.
			 */
			path = strdup(canonpath);
			if (path == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		}

		repolen = strlen(repo_abspath);
		len = strlen(path);


		if (strcmp(path, repo_abspath) == 0) {
			free(path);
			path = strdup("");
			if (path == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		} else if (len > repolen &&
		    got_path_is_child(path, repo_abspath, repolen)) {
			/* Matched an on-disk path inside repository. */
			if (got_repo_is_bare(repo)) {
				/*
				 * Matched an on-disk path inside repository
				 * database. Treat input as repository-relative.
				 */
				free(path);
				path = canonpath;
				canonpath = NULL;
			} else {
				char *child;
				/* Strip common prefix with repository path. */
				err = got_path_skip_common_ancestor(&child,
				    repo_abspath, path);
				if (err)
					goto done;
				free(path);
				path = child;
			}
		} else {
			/*
			 * Matched unrelated on-disk path.
			 * Treat input as repository-relative.
			 */
			free(path);
			path = canonpath;
			canonpath = NULL;
		}
	}

	/* Make in-repository path absolute */
	if (path[0] != '/') {
		char *abspath;
		if (asprintf(&abspath, "/%s", path) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		free(path);
		path = abspath;
	}

done:
	free(canonpath);
	if (err)
		free(path);
	else
		*in_repo_path = path;
	return err;
}

static const struct got_error *
cache_packidx(struct got_repository *repo, struct got_packidx *packidx,
    const char *path_packidx)
{
	const struct got_error *err = NULL;
	size_t i;

	for (i = 0; i < repo->pack_cache_size; i++) {
		if (repo->packidx_cache[i] == NULL)
			break;
		if (strcmp(repo->packidx_cache[i]->path_packidx,
		    path_packidx) == 0) {
			return got_error(GOT_ERR_CACHE_DUP_ENTRY);
		}
	}
	if (i == repo->pack_cache_size) {
		i = repo->pack_cache_size - 1;
		err = got_packidx_close(repo->packidx_cache[i]);
		if (err)
			return err;
	}

	repo->packidx_cache[i] = packidx;

	return NULL;
}

int
got_repo_is_packidx_filename(const char *name, size_t len)
{
	if (len != GOT_PACKIDX_NAMELEN)
		return 0;

	if (strncmp(name, GOT_PACK_PREFIX, strlen(GOT_PACK_PREFIX)) != 0)
		return 0;

	if (strcmp(name + strlen(GOT_PACK_PREFIX) +
	    SHA1_DIGEST_STRING_LENGTH - 1, GOT_PACKIDX_SUFFIX) != 0)
		return 0;

	return 1;
}

static struct got_packidx_bloom_filter *
get_packidx_bloom_filter(struct got_repository *repo,
    const char *path, size_t path_len)
{
	struct got_packidx_bloom_filter key;

	if (strlcpy(key.path, path, sizeof(key.path)) >= sizeof(key.path))
		return NULL; /* XXX */
	key.path_len = path_len;

	return RB_FIND(got_packidx_bloom_filter_tree,
	    &repo->packidx_bloom_filters, &key);
}

int
got_repo_check_packidx_bloom_filter(struct got_repository *repo,
    const char *path_packidx, struct got_object_id *id)
{
	struct got_packidx_bloom_filter *bf;

	bf = get_packidx_bloom_filter(repo, path_packidx, strlen(path_packidx));
	if (bf)
		return bloom_check(bf->bloom, id->sha1, sizeof(id->sha1));

	/* No bloom filter means this pack index must be searched. */
	return 1;
}

static const struct got_error *
add_packidx_bloom_filter(struct got_repository *repo,
    struct got_packidx *packidx, const char *path_packidx)
{
	int i, nobjects = be32toh(packidx->hdr.fanout_table[0xff]);
	struct got_packidx_bloom_filter *bf;
	size_t len;

	/*
	 * Don't use bloom filters for very large pack index files.
	 * Large pack files will contain a relatively large fraction
	 * of our objects so we will likely need to visit them anyway.
	 * The more objects a pack file contains the higher the probability
	 * of a false-positive match from the bloom filter. And reading
	 * all object IDs from a large pack index file can be expensive.
	 */
	if (nobjects > 100000) /* cut-off at about 2MB, at 20 bytes per ID */
		return NULL;

	/* Do we already have a filter for this pack index? */
	if (get_packidx_bloom_filter(repo, path_packidx,
	    strlen(path_packidx)) != NULL)
		return NULL;

	bf = calloc(1, sizeof(*bf));
	if (bf == NULL)
		return got_error_from_errno("calloc");
	bf->bloom = calloc(1, sizeof(*bf->bloom));
	if (bf->bloom == NULL) {
		free(bf);
		return got_error_from_errno("calloc");
	}
	
	len = strlcpy(bf->path, path_packidx, sizeof(bf->path));
	if (len >= sizeof(bf->path)) {
		free(bf->bloom);
		free(bf);
		return got_error(GOT_ERR_NO_SPACE);
	}
	bf->path_len = len;

	/* Minimum size supported by our bloom filter is 1000 entries. */
	bloom_init(bf->bloom, nobjects < 1000 ? 1000 : nobjects, 0.1);
	for (i = 0; i < nobjects; i++) {
		struct got_packidx_object_id *id;
		id = &packidx->hdr.sorted_ids[i];
		bloom_add(bf->bloom, id->sha1, sizeof(id->sha1));
	}

	RB_INSERT(got_packidx_bloom_filter_tree,
	    &repo->packidx_bloom_filters, bf);
	return NULL;
}

const struct got_error *
got_repo_search_packidx(struct got_packidx **packidx, int *idx,
    struct got_repository *repo, struct got_object_id *id)
{
	const struct got_error *err;
	struct got_pathlist_entry *pe;
	size_t i;

	/* Search pack index cache. */
	for (i = 0; i < repo->pack_cache_size; i++) {
		if (repo->packidx_cache[i] == NULL)
			break;
		if (!got_repo_check_packidx_bloom_filter(repo,
		    repo->packidx_cache[i]->path_packidx, id))
			continue; /* object will not be found in this index */
		*idx = got_packidx_get_object_idx(repo->packidx_cache[i], id);
		if (*idx != -1) {
			*packidx = repo->packidx_cache[i];
			/*
			 * Move this cache entry to the front. Repeatedly
			 * searching a wrong pack index can be expensive.
			 */
			if (i > 0) {
				memmove(&repo->packidx_cache[1],
				    &repo->packidx_cache[0],
				    i * sizeof(repo->packidx_cache[0]));
				repo->packidx_cache[0] = *packidx;
			}
			return NULL;
		}
	}
	/* No luck. Search the filesystem. */

	TAILQ_FOREACH(pe, &repo->packidx_paths, entry) {
		const char *path_packidx = pe->path;
		int is_cached = 0;

		if (!got_repo_check_packidx_bloom_filter(repo,
		    pe->path, id))
			continue; /* object will not be found in this index */

		for (i = 0; i < repo->pack_cache_size; i++) {
			if (repo->packidx_cache[i] == NULL)
				break;
			if (strcmp(repo->packidx_cache[i]->path_packidx,
			    path_packidx) == 0) {
				is_cached = 1;
				break;
			}
		}
		if (is_cached)
			continue; /* already searched */

		err = got_packidx_open(packidx, got_repo_get_fd(repo),
		    path_packidx, 0);
		if (err)
			goto done;

		err = add_packidx_bloom_filter(repo, *packidx, path_packidx);
		if (err)
			goto done;

		err = cache_packidx(repo, *packidx, path_packidx);
		if (err)
			goto done;

		*idx = got_packidx_get_object_idx(*packidx, id);
		if (*idx != -1) {
			err = NULL; /* found the object */
			goto done;
		}
	}

	err = got_error_no_obj(id);
done:
	return err;
}

const struct got_error *
got_repo_list_packidx(struct got_pathlist_head *packidx_paths,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	DIR *packdir = NULL;
	struct dirent *dent;
	char *path_packidx = NULL;
	int packdir_fd;

	packdir_fd = openat(got_repo_get_fd(repo),
	    GOT_OBJECTS_PACK_DIR, O_DIRECTORY | O_CLOEXEC);
	if (packdir_fd == -1) {
		return got_error_from_errno_fmt("openat: %s/%s",
		    got_repo_get_path_git_dir(repo),
		    GOT_OBJECTS_PACK_DIR);
	}

	packdir = fdopendir(packdir_fd);
	if (packdir == NULL) {
		err = got_error_from_errno("fdopendir");
		goto done;
	}

	while ((dent = readdir(packdir)) != NULL) {
		if (!got_repo_is_packidx_filename(dent->d_name,
		    strlen(dent->d_name)))
			continue;

		if (asprintf(&path_packidx, "%s/%s", GOT_OBJECTS_PACK_DIR,
		    dent->d_name) == -1) {
			err = got_error_from_errno("asprintf");
			path_packidx = NULL;
			break;
		}

		err = got_pathlist_append(packidx_paths, path_packidx, NULL);
		if (err)
			break;
	}
done:
	if (err)
		free(path_packidx);
	if (packdir && closedir(packdir) != 0 && err == NULL)
		err = got_error_from_errno("closedir");
	return err;
}

const struct got_error *
got_repo_get_packidx(struct got_packidx **packidx, const char *path_packidx,
    struct got_repository *repo)
{
	const struct got_error *err;
	size_t i;

	*packidx = NULL;

	/* Search pack index cache. */
	for (i = 0; i < repo->pack_cache_size; i++) {
		if (repo->packidx_cache[i] == NULL)
			break;
		if (strcmp(repo->packidx_cache[i]->path_packidx,
		    path_packidx) == 0) {
			*packidx = repo->packidx_cache[i];
			return NULL;
		}
	}
	/* No luck. Search the filesystem. */

	err = got_packidx_open(packidx, got_repo_get_fd(repo),
	    path_packidx, 0);
	if (err)
		return err;

	err = add_packidx_bloom_filter(repo, *packidx, path_packidx);
	if (err)
		goto done;

	err = cache_packidx(repo, *packidx, path_packidx);
done:
	if (err) {
		got_packidx_close(*packidx);
		*packidx = NULL;
	}
	return err;
}

static const struct got_error *
read_packfile_hdr(int fd, struct got_packidx *packidx)
{
	const struct got_error *err = NULL;
	uint32_t totobj = be32toh(packidx->hdr.fanout_table[0xff]);
	struct got_packfile_hdr hdr;
	ssize_t n;

	n = read(fd, &hdr, sizeof(hdr));
	if (n < 0)
		return got_error_from_errno("read");
	if (n != sizeof(hdr))
		return got_error(GOT_ERR_BAD_PACKFILE);

	if (be32toh(hdr.signature) != GOT_PACKFILE_SIGNATURE ||
	    be32toh(hdr.version) != GOT_PACKFILE_VERSION ||
	    be32toh(hdr.nobjects) != totobj)
		err = got_error(GOT_ERR_BAD_PACKFILE);

	return err;
}

static const struct got_error *
open_packfile(int *fd, struct got_repository *repo,
    const char *relpath, struct got_packidx *packidx)
{
	const struct got_error *err = NULL;

	*fd = openat(got_repo_get_fd(repo), relpath,
	    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (*fd == -1)
		return got_error_from_errno_fmt("openat: %s/%s",
		    got_repo_get_path_git_dir(repo), relpath);

	if (packidx) {
		err = read_packfile_hdr(*fd, packidx);
		if (err) {
			close(*fd);
			*fd = -1;
		}
	}

	return err;
}

const struct got_error *
got_repo_cache_pack(struct got_pack **packp, struct got_repository *repo,
    const char *path_packfile, struct got_packidx *packidx)
{
	const struct got_error *err = NULL;
	struct got_pack *pack = NULL;
	struct stat sb;
	size_t i;

	if (packp)
		*packp = NULL;

	for (i = 0; i < repo->pack_cache_size; i++) {
		pack = &repo->packs[i];
		if (pack->path_packfile == NULL)
			break;
		if (strcmp(pack->path_packfile, path_packfile) == 0)
			return got_error(GOT_ERR_CACHE_DUP_ENTRY);
	}

	if (i == repo->pack_cache_size) {
		err = got_pack_close(&repo->packs[i - 1]);
		if (err)
			return err;
		memmove(&repo->packs[1], &repo->packs[0],
		    sizeof(repo->packs) - sizeof(repo->packs[0]));
		i = 0;
	}

	pack = &repo->packs[i];

	pack->path_packfile = strdup(path_packfile);
	if (pack->path_packfile == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}

	err = open_packfile(&pack->fd, repo, path_packfile, packidx);
	if (err)
		goto done;

	if (fstat(pack->fd, &sb) != 0) {
		err = got_error_from_errno("fstat");
		goto done;
	}
	pack->filesize = sb.st_size;

	pack->privsep_child = NULL;

#ifndef GOT_PACK_NO_MMAP
	pack->map = mmap(NULL, pack->filesize, PROT_READ, MAP_PRIVATE,
	    pack->fd, 0);
	if (pack->map == MAP_FAILED) {
		if (errno != ENOMEM) {
			err = got_error_from_errno("mmap");
			goto done;
		}
		pack->map = NULL; /* fall back to read(2) */
	}
#endif
done:
	if (err) {
		if (pack) {
			free(pack->path_packfile);
			memset(pack, 0, sizeof(*pack));
		}
	} else if (packp)
		*packp = pack;
	return err;
}

struct got_pack *
got_repo_get_cached_pack(struct got_repository *repo, const char *path_packfile)
{
	struct got_pack *pack = NULL;
	size_t i;

	for (i = 0; i < repo->pack_cache_size; i++) {
		pack = &repo->packs[i];
		if (pack->path_packfile == NULL)
			break;
		if (strcmp(pack->path_packfile, path_packfile) == 0)
			return pack;
	}

	return NULL;
}

const struct got_error *
got_repo_init(const char *repo_path)
{
	const struct got_error *err = NULL;
	const char *dirnames[] = {
		GOT_OBJECTS_DIR,
		GOT_OBJECTS_PACK_DIR,
		GOT_REFS_DIR,
	};
	const char *description_str = "Unnamed repository; "
	    "edit this file 'description' to name the repository.";
	const char *headref_str = "ref: refs/heads/main";
	const char *gitconfig_str = "[core]\n"
	    "\trepositoryformatversion = 0\n"
	    "\tfilemode = true\n"
	    "\tbare = true\n";
	char *path;
	size_t i;

	if (!got_path_dir_is_empty(repo_path))
		return got_error(GOT_ERR_DIR_NOT_EMPTY);

	for (i = 0; i < nitems(dirnames); i++) {
		if (asprintf(&path, "%s/%s", repo_path, dirnames[i]) == -1) {
			return got_error_from_errno("asprintf");
		}
		err = got_path_mkdir(path);
		free(path);
		if (err)
			return err;
	}

	if (asprintf(&path, "%s/%s", repo_path, "description") == -1)
		return got_error_from_errno("asprintf");
	err = got_path_create_file(path, description_str);
	free(path);
	if (err)
		return err;

	if (asprintf(&path, "%s/%s", repo_path, GOT_HEAD_FILE) == -1)
		return got_error_from_errno("asprintf");
	err = got_path_create_file(path, headref_str);
	free(path);
	if (err)
		return err;

	if (asprintf(&path, "%s/%s", repo_path, "config") == -1)
		return got_error_from_errno("asprintf");
	err = got_path_create_file(path, gitconfig_str);
	free(path);
	if (err)
		return err;

	return NULL;
}

static const struct got_error *
match_packed_object(struct got_object_id **unique_id,
    struct got_repository *repo, const char *id_str_prefix, int obj_type)
{
	const struct got_error *err = NULL;
	struct got_object_id_queue matched_ids;
	struct got_pathlist_entry *pe;

	STAILQ_INIT(&matched_ids);

	TAILQ_FOREACH(pe, &repo->packidx_paths, entry) {
		const char *path_packidx = pe->path;
		struct got_packidx *packidx;
		struct got_object_qid *qid;

		err = got_packidx_open(&packidx, got_repo_get_fd(repo),
		    path_packidx, 0);
		if (err)
			break;

		err = got_packidx_match_id_str_prefix(&matched_ids,
		    packidx, id_str_prefix);
		if (err) {
			got_packidx_close(packidx);
			break;
		}
		err = got_packidx_close(packidx);
		if (err)
			break;

		STAILQ_FOREACH(qid, &matched_ids, entry) {
			if (obj_type != GOT_OBJ_TYPE_ANY) {
				int matched_type;
				err = got_object_get_type(&matched_type, repo,
				    qid->id);
				if (err)
					goto done;
				if (matched_type != obj_type)
					continue;
			}
			if (*unique_id == NULL) {
				*unique_id = got_object_id_dup(qid->id);
				if (*unique_id == NULL) {
					err = got_error_from_errno("malloc");
					goto done;
				}
			} else {
				if (got_object_id_cmp(*unique_id, qid->id) == 0)
					continue; /* packed multiple times */
				err = got_error(GOT_ERR_AMBIGUOUS_ID);
				goto done;
			}
		}
	}
done:
	got_object_id_queue_free(&matched_ids);
	if (err) {
		free(*unique_id);
		*unique_id = NULL;
	}
	return err;
}

static const struct got_error *
match_loose_object(struct got_object_id **unique_id, const char *path_objects,
    const char *object_dir, const char *id_str_prefix, int obj_type,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *path;
	DIR *dir = NULL;
	struct dirent *dent;
	struct got_object_id id;

	if (asprintf(&path, "%s/%s", path_objects, object_dir) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	dir = opendir(path);
	if (dir == NULL) {
		if (errno == ENOENT) {
			err = NULL;
			goto done;
		}
		err = got_error_from_errno2("opendir", path);
		goto done;
	}
	while ((dent = readdir(dir)) != NULL) {
		char *id_str;
		int cmp;

		if (strcmp(dent->d_name, ".") == 0 ||
		    strcmp(dent->d_name, "..") == 0)
			continue;

		if (asprintf(&id_str, "%s%s", object_dir, dent->d_name) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}

		if (!got_parse_sha1_digest(id.sha1, id_str))
			continue;

		/*
		 * Directory entries do not necessarily appear in
		 * sorted order, so we must iterate over all of them.
		 */
		cmp = strncmp(id_str, id_str_prefix, strlen(id_str_prefix));
		if (cmp != 0) {
			free(id_str);
			continue;
		}

		if (*unique_id == NULL) {
			if (obj_type != GOT_OBJ_TYPE_ANY) {
				int matched_type;
				err = got_object_get_type(&matched_type, repo,
				    &id);
				if (err)
					goto done;
				if (matched_type != obj_type)
					continue;
			}
			*unique_id = got_object_id_dup(&id);
			if (*unique_id == NULL) {
				err = got_error_from_errno("got_object_id_dup");
				free(id_str);
				goto done;
			}
		} else {
			if (got_object_id_cmp(*unique_id, &id) == 0)
				continue; /* both packed and loose */
			err = got_error(GOT_ERR_AMBIGUOUS_ID);
			free(id_str);
			goto done;
		}
	}
done:
	if (dir && closedir(dir) != 0 && err == NULL)
		err = got_error_from_errno("closedir");
	if (err) {
		free(*unique_id);
		*unique_id = NULL;
	}
	free(path);
	return err;
}

const struct got_error *
got_repo_match_object_id_prefix(struct got_object_id **id,
    const char *id_str_prefix, int obj_type, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *path_objects = got_repo_get_path_objects(repo);
	char *object_dir = NULL;
	size_t len;
	int i;

	*id = NULL;

	len = strlen(id_str_prefix);
	if (len > SHA1_DIGEST_STRING_LENGTH - 1)
		return got_error_path(id_str_prefix, GOT_ERR_BAD_OBJ_ID_STR);

	for (i = 0; i < len; i++) {
		if (isxdigit((unsigned char)id_str_prefix[i]))
			continue;
		return got_error_path(id_str_prefix, GOT_ERR_BAD_OBJ_ID_STR);
	}

	if (len >= 2) {
		err = match_packed_object(id, repo, id_str_prefix, obj_type);
		if (err)
			goto done;
		object_dir = strndup(id_str_prefix, 2);
		if (object_dir == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
		err = match_loose_object(id, path_objects, object_dir,
		    id_str_prefix, obj_type, repo);
	} else if (len == 1) {
		int i;
		for (i = 0; i < 0xf; i++) {
			if (asprintf(&object_dir, "%s%.1x", id_str_prefix, i)
			    == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
			err = match_packed_object(id, repo, object_dir,
			    obj_type);
			if (err)
				goto done;
			err = match_loose_object(id, path_objects, object_dir,
			    id_str_prefix, obj_type, repo);
			if (err)
				goto done;
		}
	} else {
		err = got_error_path(id_str_prefix, GOT_ERR_BAD_OBJ_ID_STR);
		goto done;
	}
done:
	free(object_dir);
	if (err) {
		free(*id);
		*id = NULL;
	} else if (*id == NULL) {
		switch (obj_type) {
		case GOT_OBJ_TYPE_BLOB:
			err = got_error_fmt(GOT_ERR_NO_OBJ, "%s %s",
			    GOT_OBJ_LABEL_BLOB, id_str_prefix);
			break;
		case GOT_OBJ_TYPE_TREE:
			err = got_error_fmt(GOT_ERR_NO_OBJ, "%s %s",
			    GOT_OBJ_LABEL_TREE, id_str_prefix);
			break;
		case GOT_OBJ_TYPE_COMMIT:
			err = got_error_fmt(GOT_ERR_NO_OBJ, "%s %s",
			    GOT_OBJ_LABEL_COMMIT, id_str_prefix);
			break;
		case GOT_OBJ_TYPE_TAG:
			err = got_error_fmt(GOT_ERR_NO_OBJ, "%s %s",
			    GOT_OBJ_LABEL_TAG, id_str_prefix);
			break;
		default:
			err = got_error_path(id_str_prefix, GOT_ERR_NO_OBJ);
			break;
		}
	}

	return err;
}

const struct got_error *
got_repo_match_object_id(struct got_object_id **id, char **label,
    const char *id_str, int obj_type, struct got_reflist_head *refs,
    struct got_repository *repo)
{
	const struct got_error *err;
	struct got_tag_object *tag;
	struct got_reference *ref = NULL;

	*id = NULL;
	if (label)
		*label = NULL;

	if (refs) {
		err = got_repo_object_match_tag(&tag, id_str, obj_type,
		    refs, repo);
		if (err == NULL) {
			*id = got_object_id_dup(
			    got_object_tag_get_object_id(tag));
			if (*id == NULL)
				err = got_error_from_errno("got_object_id_dup");
			else if (label && asprintf(label, "refs/tags/%s",
			    got_object_tag_get_name(tag)) == -1) {
				err = got_error_from_errno("asprintf");
				free(*id);
				*id = NULL;
			}
			got_object_tag_close(tag);
			return err;
		} else if (err->code != GOT_ERR_OBJ_TYPE &&
		    err->code != GOT_ERR_NO_OBJ)
			return err;
	}

	err = got_repo_match_object_id_prefix(id, id_str, obj_type, repo);
	if (err) {
		if (err->code != GOT_ERR_BAD_OBJ_ID_STR)
			return err;
		err = got_ref_open(&ref, repo, id_str, 0);
		if (err != NULL)
			goto done;
		if (label) {
			*label = strdup(got_ref_get_name(ref));
			if (*label == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		}
		err = got_ref_resolve(id, repo, ref);
	} else if (label) {
		err = got_object_id_str(label, *id);
		if (*label == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}
done:
	if (ref)
		got_ref_close(ref);
	return err;
}

const struct got_error *
got_repo_object_match_tag(struct got_tag_object **tag, const char *name,
    int obj_type, struct got_reflist_head *refs, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_reflist_entry *re;
	struct got_object_id *tag_id;
	int name_is_absolute = (strncmp(name, "refs/", 5) == 0);

	*tag = NULL;

	TAILQ_FOREACH(re, refs, entry) {
		const char *refname;
		refname = got_ref_get_name(re->ref);
		if (got_ref_is_symbolic(re->ref))
			continue;
		if (strncmp(refname, "refs/tags/", 10) != 0)
			continue;
		if (!name_is_absolute)
			refname += strlen("refs/tags/");
		if (strcmp(refname, name) != 0)
			continue;
		err = got_ref_resolve(&tag_id, repo, re->ref);
		if (err)
			break;
		err = got_object_open_as_tag(tag, repo, tag_id);
		free(tag_id);
		if (err)
			break;
		if (obj_type == GOT_OBJ_TYPE_ANY ||
		    got_object_tag_get_object_type(*tag) == obj_type)
			break;
		got_object_tag_close(*tag);
		*tag = NULL;
	}

	if (err == NULL && *tag == NULL)
		err = got_error_fmt(GOT_ERR_NO_OBJ, "%s %s",
		    GOT_OBJ_LABEL_TAG, name);
	return err;
}

static const struct got_error *
alloc_added_blob_tree_entry(struct got_tree_entry **new_te,
    const char *name, mode_t mode, struct got_object_id *blob_id)
{
	const struct got_error *err = NULL;

	 *new_te = NULL;

	*new_te = calloc(1, sizeof(**new_te));
	if (*new_te == NULL)
		return got_error_from_errno("calloc");

	if (strlcpy((*new_te)->name, name, sizeof((*new_te)->name)) >=
	    sizeof((*new_te)->name)) {
		err = got_error(GOT_ERR_NO_SPACE);
		goto done;
	}

	if (S_ISLNK(mode)) {
		(*new_te)->mode = S_IFLNK;
	} else {
		(*new_te)->mode = S_IFREG;
		(*new_te)->mode |= (mode & (S_IRWXU | S_IRWXG | S_IRWXO));
	}
	memcpy(&(*new_te)->id, blob_id, sizeof((*new_te)->id));
done:
	if (err && *new_te) {
		free(*new_te);
		*new_te = NULL;
	}
	return err;
}

static const struct got_error *
import_file(struct got_tree_entry **new_te, struct dirent *de,
    const char *path, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_object_id *blob_id = NULL;
	char *filepath;
	struct stat sb;

	if (asprintf(&filepath, "%s%s%s", path,
	    path[0] == '\0' ? "" : "/", de->d_name) == -1)
		return got_error_from_errno("asprintf");

	if (lstat(filepath, &sb) != 0) {
		err = got_error_from_errno2("lstat", path);
		goto done;
	}

	err = got_object_blob_create(&blob_id, filepath, repo);
	if (err)
		goto done;

	err = alloc_added_blob_tree_entry(new_te, de->d_name, sb.st_mode,
	    blob_id);
done:
	free(filepath);
	if (err)
		free(blob_id);
	return err;
}

static const struct got_error *
insert_tree_entry(struct got_tree_entry *new_te,
    struct got_pathlist_head *paths)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *new_pe;

	err = got_pathlist_insert(&new_pe, paths, new_te->name, new_te);
	if (err)
		return err;
	if (new_pe == NULL)
		return got_error(GOT_ERR_TREE_DUP_ENTRY);
	return NULL;
}

static const struct got_error *write_tree(struct got_object_id **,
    const char *, struct got_pathlist_head *, struct got_repository *,
    got_repo_import_cb progress_cb, void *progress_arg);

static const struct got_error *
import_subdir(struct got_tree_entry **new_te, struct dirent *de,
    const char *path, struct got_pathlist_head *ignores,
    struct got_repository *repo,
    got_repo_import_cb progress_cb, void *progress_arg)
{
	const struct got_error *err;
	struct got_object_id *id = NULL;
	char *subdirpath;

	if (asprintf(&subdirpath, "%s%s%s", path,
	    path[0] == '\0' ? "" : "/", de->d_name) == -1)
		return got_error_from_errno("asprintf");

	(*new_te) = calloc(1, sizeof(**new_te));
	if (*new_te == NULL)
		return got_error_from_errno("calloc");
	(*new_te)->mode = S_IFDIR;
	if (strlcpy((*new_te)->name, de->d_name, sizeof((*new_te)->name)) >=
	    sizeof((*new_te)->name)) {
		err = got_error(GOT_ERR_NO_SPACE);
		goto done;
	}
	err = write_tree(&id, subdirpath, ignores,  repo,
	    progress_cb, progress_arg);
	if (err)
		goto done;
	memcpy(&(*new_te)->id, id, sizeof((*new_te)->id));
	
done:
	free(id);
	free(subdirpath);
	if (err) {
		free(*new_te);
		*new_te = NULL;
	}
	return err;
}

static const struct got_error *
write_tree(struct got_object_id **new_tree_id, const char *path_dir,
    struct got_pathlist_head *ignores, struct got_repository *repo,
    got_repo_import_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	DIR *dir;
	struct dirent *de;
	int nentries;
	struct got_tree_entry *new_te = NULL;
	struct got_pathlist_head paths;
	struct got_pathlist_entry *pe;

	*new_tree_id = NULL;

	TAILQ_INIT(&paths);

	dir = opendir(path_dir);
	if (dir == NULL) {
		err = got_error_from_errno2("opendir", path_dir);
		goto done;
	}

	nentries = 0;
	while ((de = readdir(dir)) != NULL) {
		int ignore = 0;
		int type;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;

		TAILQ_FOREACH(pe, ignores, entry) {
			if (fnmatch(pe->path, de->d_name, 0) == 0) {
				ignore = 1;
				break;
			}
		}
		if (ignore)
			continue;

		err = got_path_dirent_type(&type, path_dir, de);
		if (err)
			goto done;

		if (type == DT_DIR) {
			err = import_subdir(&new_te, de, path_dir,
			    ignores, repo, progress_cb, progress_arg);
			if (err) {
				if (err->code != GOT_ERR_NO_TREE_ENTRY)
					goto done;
				err = NULL;
				continue;
			}
		} else if (type == DT_REG || type == DT_LNK) {
			err = import_file(&new_te, de, path_dir, repo);
			if (err)
				goto done;
		} else
			continue;

		err = insert_tree_entry(new_te, &paths);
		if (err)
			goto done;
		nentries++;
	}

	if (TAILQ_EMPTY(&paths)) {
		err = got_error_msg(GOT_ERR_NO_TREE_ENTRY,
		    "cannot create tree without any entries");
		goto done;
	}

	TAILQ_FOREACH(pe, &paths, entry) {
		struct got_tree_entry *te = pe->data;
		char *path;
		if (!S_ISREG(te->mode) && !S_ISLNK(te->mode))
			continue;
		if (asprintf(&path, "%s/%s", path_dir, pe->path) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		err = (*progress_cb)(progress_arg, path);
		free(path);
		if (err)
			goto done;
	}

	err = got_object_tree_create(new_tree_id, &paths, nentries, repo);
done:
	if (dir)
		closedir(dir);
	got_pathlist_free(&paths);
	return err;
}

const struct got_error *
got_repo_import(struct got_object_id **new_commit_id, const char *path_dir,
    const char *logmsg, const char *author, struct got_pathlist_head *ignores,
    struct got_repository *repo, got_repo_import_cb progress_cb,
    void *progress_arg)
{
	const struct got_error *err;
	struct got_object_id *new_tree_id;

	err = write_tree(&new_tree_id, path_dir, ignores, repo,
	    progress_cb, progress_arg);
	if (err)
		return err;

	err = got_object_commit_create(new_commit_id, new_tree_id, NULL, 0,
	    author, time(NULL), author, time(NULL), logmsg, repo);
	free(new_tree_id);
	return err;
}

const struct got_error *
got_repo_get_loose_object_info(int *nobjects, off_t *ondisk_size,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *path_objects = NULL, *path = NULL;
	DIR *dir = NULL;
	struct got_object_id id;
	int i;

	*nobjects = 0;
	*ondisk_size = 0;

	path_objects = got_repo_get_path_objects(repo);
	if (path_objects == NULL)
		return got_error_from_errno("got_repo_get_path_objects");

	for (i = 0; i <= 0xff; i++) {
		struct dirent *dent;

		if (asprintf(&path, "%s/%.2x", path_objects, i) == -1) {
			err = got_error_from_errno("asprintf");
			break;
		}

		dir = opendir(path);
		if (dir == NULL) {
			if (errno == ENOENT) {
				err = NULL;
				continue;
			}
			err = got_error_from_errno2("opendir", path);
			break;
		}

		while ((dent = readdir(dir)) != NULL) {
			char *id_str;
			int fd;
			struct stat sb;

			if (strcmp(dent->d_name, ".") == 0 ||
			    strcmp(dent->d_name, "..") == 0)
				continue;

			if (asprintf(&id_str, "%.2x%s", i, dent->d_name) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}

			if (!got_parse_sha1_digest(id.sha1, id_str)) {
				free(id_str);
				continue;
			}
			free(id_str);

			err = got_object_open_loose_fd(&fd, &id, repo);
			if (err)
				goto done;

			if (fstat(fd, &sb) == -1) {
				err = got_error_from_errno("fstat");
				close(fd);
				goto done;
			}
			(*nobjects)++;
			(*ondisk_size) += sb.st_size;

			if (close(fd) == -1) {
				err = got_error_from_errno("close");
				goto done;
			}
		}

		if (closedir(dir) != 0) {
			err = got_error_from_errno("closedir");
			goto done;
		}
		dir = NULL;

		free(path);
		path = NULL;
	}
done:
	if (dir && closedir(dir) != 0 && err == NULL)
		err = got_error_from_errno("closedir");

	if (err) {
		*nobjects = 0;
		*ondisk_size = 0;
	}
	free(path_objects);
	free(path);
	return err;
}

const struct got_error *
got_repo_get_packfile_info(int *npackfiles, int *nobjects,
    off_t *total_packsize, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	DIR *packdir = NULL;
	struct dirent *dent;
	struct got_packidx *packidx = NULL;
	char *path_packidx;
	char *path_packfile;
	int packdir_fd;
	struct stat sb;

	*npackfiles = 0;
	*nobjects = 0;
	*total_packsize = 0;

	packdir_fd = openat(got_repo_get_fd(repo),
	    GOT_OBJECTS_PACK_DIR, O_DIRECTORY);
	if (packdir_fd == -1) {
		return got_error_from_errno_fmt("openat: %s/%s",
		    got_repo_get_path_git_dir(repo),
		    GOT_OBJECTS_PACK_DIR);
	}

	packdir = fdopendir(packdir_fd);
	if (packdir == NULL) {
		err = got_error_from_errno("fdopendir");
		goto done;
	}

	while ((dent = readdir(packdir)) != NULL) {
		if (!got_repo_is_packidx_filename(dent->d_name,
		    strlen(dent->d_name)))
			continue;

		if (asprintf(&path_packidx, "%s/%s", GOT_OBJECTS_PACK_DIR,
		    dent->d_name) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}

		err = got_packidx_open(&packidx, got_repo_get_fd(repo),
		    path_packidx, 0);
		free(path_packidx);
		if (err)
			goto done;

		if (fstat(packidx->fd, &sb) == -1)
			goto done;
		*total_packsize += sb.st_size;

		err = got_packidx_get_packfile_path(&path_packfile,
		    packidx->path_packidx);
		if (err)
			goto done;

		if (fstatat(got_repo_get_fd(repo), path_packfile, &sb,
		    0) == -1) {
			free(path_packfile);
			goto done;
		}
		free(path_packfile);
		*total_packsize += sb.st_size;

		*nobjects += be32toh(packidx->hdr.fanout_table[0xff]);

		(*npackfiles)++;

		got_packidx_close(packidx);
		packidx = NULL;
	}
done:
	if (packidx)
		got_packidx_close(packidx);
	if (packdir && closedir(packdir) != 0 && err == NULL)
		err = got_error_from_errno("closedir");
	if (err) {
		*npackfiles = 0;
		*nobjects = 0;
		*total_packsize = 0;
	}
	return err;
}

RB_GENERATE(got_packidx_bloom_filter_tree, got_packidx_bloom_filter, entry,
    got_packidx_bloom_filter_cmp);

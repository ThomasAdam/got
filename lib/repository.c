/*
 * Copyright (c) 2018 Stefan Sperling <stsp@openbsd.org>
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

#include <sys/queue.h>
#include <sys/stat.h>

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <sha1.h>
#include <string.h>
#include <zlib.h>
#include <errno.h>
#include <libgen.h>

#include "got_error.h"
#include "got_reference.h"
#include "got_repository.h"
#include "got_worktree.h"
#include "got_object.h"

#include "got_lib_path.h"
#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_pack.h"
#include "got_lib_repository.h"
#include "got_lib_worktree.h"
#include "got_lib_object_idcache.h"

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

#define GOT_GIT_DIR	".git"

/* Mandatory files and directories inside the git directory. */
#define GOT_OBJECTS_DIR		"objects"
#define GOT_REFS_DIR		"refs"
#define GOT_HEAD_FILE		"HEAD"

/* Other files and directories inside the git directory. */
#define GOT_FETCH_HEAD_FILE	"FETCH_HEAD"
#define GOT_ORIG_HEAD_FILE	"ORIG_HEAD"
#define GOT_OBJECTS_PACK_DIR	"objects/pack"

char *
got_repo_get_path(struct got_repository *repo)
{
	return strdup(repo->path);
}

char *
got_repo_get_path_git_dir(struct got_repository *repo)
{
	return strdup(repo->path_git_dir);
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

static char *
get_path_head(struct got_repository *repo)
{
	return get_path_git_child(repo, GOT_HEAD_FILE);
}

static int
is_git_repo(struct got_repository *repo)
{
	char *path_git = got_repo_get_path_git_dir(repo);
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
	if (got_ref_open(&head_ref, repo, GOT_REF_HEAD) != NULL)
		goto done;
	got_ref_close(head_ref);

	ret = 1;
done:
	free(path_git);
	free(path_objects);
	free(path_refs);
	free(path_head);
	return ret;

}

#ifndef GOT_NO_OBJ_CACHE
static const struct got_error *
cache_add(struct got_object_cache *cache, struct got_object_id *id, void *item)
{
	const struct got_error *err = NULL;
	struct got_object_cache_entry *ce;
	int nelem;

	nelem = got_object_idcache_num_elements(cache->idcache);
	if (nelem >= cache->size) {
		err = got_object_idcache_remove_least_used((void **)&ce,
		    cache->idcache);
		if (err)
			return err;
		switch (cache->type) {
		case GOT_OBJECT_CACHE_TYPE_OBJ:
			got_object_close(ce->data.obj);
			break;
		case GOT_OBJECT_CACHE_TYPE_TREE:
			got_object_tree_close(ce->data.tree);
			break;
		case GOT_OBJECT_CACHE_TYPE_COMMIT:
			got_object_commit_close(ce->data.commit);
			break;
		}
		free(ce);
	}

	ce = calloc(1, sizeof(*ce));
	if (ce == NULL)
		return got_error_from_errno();
	memcpy(&ce->id, id, sizeof(ce->id));
	switch (cache->type) {
	case GOT_OBJECT_CACHE_TYPE_OBJ:
		ce->data.obj = (struct got_object *)item;
		break;
	case GOT_OBJECT_CACHE_TYPE_TREE:
		ce->data.tree = (struct got_tree_object *)item;
		break;
	case GOT_OBJECT_CACHE_TYPE_COMMIT:
		ce->data.commit = (struct got_commit_object *)item;
		break;
	}

	err = got_object_idcache_add(cache->idcache, id, ce);
	if (err) {
		if (err->code == GOT_ERR_OBJ_EXISTS) {
			free(ce);
			err = NULL;
		}
	}
	return err;
}
#endif

const struct got_error *
got_repo_cache_object(struct got_repository *repo, struct got_object_id *id,
    struct got_object *obj)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = cache_add(&repo->objcache, id, obj);
	if (err)
		return err;
	obj->refcnt++;
#endif
	return NULL;
}

struct got_object *
got_repo_get_cached_object(struct got_repository *repo,
    struct got_object_id *id)
{
	struct got_object_cache_entry *ce;

	ce = got_object_idcache_get(repo->objcache.idcache, id);
	if (ce) {
		repo->objcache.cache_hit++;
		return ce->data.obj;
	}

	repo->objcache.cache_miss++;
	return NULL;
}

const struct got_error *
got_repo_cache_tree(struct got_repository *repo, struct got_object_id *id,
    struct got_tree_object *tree)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = cache_add(&repo->treecache, id, tree);
	if (err)
		return err;
	tree->refcnt++;
#endif
	return NULL;
}

struct got_tree_object *
got_repo_get_cached_tree(struct got_repository *repo,
    struct got_object_id *id)
{
	struct got_object_cache_entry *ce;

	ce = got_object_idcache_get(repo->treecache.idcache, id);
	if (ce) {
		repo->treecache.cache_hit++;
		return ce->data.tree;
	}

	repo->treecache.cache_miss++;
	return NULL;
}

const struct got_error *
got_repo_cache_commit(struct got_repository *repo, struct got_object_id *id,
    struct got_commit_object *commit)
{
#ifndef GOT_NO_OBJ_CACHE
	const struct got_error *err = NULL;
	err = cache_add(&repo->commitcache, id, commit);
	if (err)
		return err;

	commit->refcnt++;
#endif
	return NULL;
}

struct got_commit_object *
got_repo_get_cached_commit(struct got_repository *repo,
    struct got_object_id *id)
{
	struct got_object_cache_entry *ce;

	ce = got_object_idcache_get(repo->commitcache.idcache, id);
	if (ce) {
		repo->commitcache.cache_hit++;
		return ce->data.commit;
	}

	repo->commitcache.cache_miss++;
	return NULL;
}

const struct got_error *
open_repo(struct got_repository *repo, const char *path)
{
	const struct got_error *err = NULL;
	struct got_worktree *worktree = NULL;

	/* bare git repository? */
	repo->path_git_dir = strdup(path);
	if (repo->path_git_dir == NULL) {
		err = got_error_from_errno();
		goto done;
	}
	if (is_git_repo(repo)) {
		repo->path = strdup(repo->path_git_dir);
		if (repo->path == NULL) {
			err = got_error_from_errno();
			goto done;
		}
		return NULL;
	}

	/* git repository with working tree? */
	free(repo->path_git_dir);
	if (asprintf(&repo->path_git_dir, "%s/%s", path, GOT_GIT_DIR) == -1) {
		err = got_error_from_errno();
		goto done;
	}
	if (is_git_repo(repo)) {
		repo->path = strdup(path);
		if (repo->path == NULL) {
			err = got_error_from_errno();
			goto done;
		}
		return NULL;
	}

	/* got work tree checked out from bare git repository? */
	free(repo->path_git_dir);
	repo->path_git_dir = NULL;
	err = got_worktree_open(&worktree, path);
	if (err) {
		if (err->code == GOT_ERR_ERRNO && errno == ENOENT)
			err = got_error(GOT_ERR_NOT_GIT_REPO);
		goto done;
	}
	repo->path_git_dir = strdup(worktree->repo_path);
	if (repo->path_git_dir == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	/* got work tree checked out from git repository with working tree? */
	if (!is_git_repo(repo)) {
		free(repo->path_git_dir);
		if (asprintf(&repo->path_git_dir, "%s/%s", worktree->repo_path,
		    GOT_GIT_DIR) == -1) {
			err = got_error_from_errno();
			repo->path_git_dir = NULL;
			goto done;
		}
		if (!is_git_repo(repo)) {
			err = got_error(GOT_ERR_NOT_GIT_REPO);
			goto done;
		}
		repo->path = strdup(worktree->repo_path);
		if (repo->path == NULL) {
			err = got_error_from_errno();
			goto done;
		}
	} else {
		repo->path = strdup(repo->path_git_dir);
		if (repo->path == NULL) {
			err = got_error_from_errno();
			goto done;
		}
	}
done:
	if (worktree)
		got_worktree_close(worktree);
	return err;
}

const struct got_error *
got_repo_open(struct got_repository **repop, const char *path)
{
	struct got_repository *repo = NULL;
	const struct got_error *err = NULL;
	char *abspath, *normpath = NULL;
	int tried_root = 0;

	*repop = NULL;

	if (got_path_is_absolute(path))
		abspath = strdup(path);
	else
		abspath = got_path_get_absolute(path);
	if (abspath == NULL)
		return got_error(GOT_ERR_BAD_PATH);

	repo = calloc(1, sizeof(*repo));
	if (repo == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	repo->objcache.type = GOT_OBJECT_CACHE_TYPE_OBJ;
	repo->objcache.size = GOT_OBJECT_CACHE_SIZE_OBJ;
	repo->objcache.idcache = got_object_idcache_alloc(repo->objcache.size);
	if (repo->objcache.idcache == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	repo->treecache.type = GOT_OBJECT_CACHE_TYPE_TREE;
	repo->treecache.size = GOT_OBJECT_CACHE_SIZE_TREE;
	repo->treecache.idcache =
	    got_object_idcache_alloc(repo->treecache.size);
	if (repo->treecache.idcache == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	repo->commitcache.type = GOT_OBJECT_CACHE_TYPE_COMMIT;
	repo->commitcache.size = GOT_OBJECT_CACHE_SIZE_COMMIT;
	repo->commitcache.idcache =
	    got_object_idcache_alloc(repo->commitcache.size);
	if (repo->commitcache.idcache == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	normpath = got_path_normalize(abspath);
	if (normpath == NULL) {
		err = got_error(GOT_ERR_BAD_PATH);
		goto done;
	}

	path = normpath;
	do {
		err = open_repo(repo, path);
		if (err == NULL)
			break;
		if (err->code != GOT_ERR_NOT_GIT_REPO)
			break;
		if (path[0] == '/' && path[1] == '\0') {
			if (tried_root) {
				err = got_error(GOT_ERR_NOT_GIT_REPO);
				break;
			}
			tried_root = 1;
		}
		path = dirname(path);
		if (path == NULL)
			err = got_error_from_errno();
	} while (path);
done:
	if (err)
		got_repo_close(repo);
	else
		*repop = repo;
	free(abspath);
	free(normpath);
	return err;
}

#if 0
static void
print_cache_stats(struct got_object_cache *cache, const char *name)
{
	fprintf(stderr, "%s cache: %d elements, %d hits, %d missed\n",
	    name, got_object_idcache_num_elements(cache->idcache),
	    cache->cache_hit, cache->cache_miss);
}

void check_refcount(struct got_object_id *id, void *data, void *arg)
{
	struct got_object_cache *cache = arg;
	struct got_object_cache_entry *ce = data;
	struct got_object *obj;
	struct got_tree_object *tree;
	struct got_commit_object *commit;
	char *id_str;

	if (got_object_id_str(&id_str, id) != NULL)
		return;

	switch (cache->type) {
	case GOT_OBJECT_CACHE_TYPE_OBJ:
		obj = ce->data.obj;
		if (obj->refcnt == 1)
			break;
		fprintf(stderr, "object %s has %d unclaimed references\n",
		    id_str, obj->refcnt - 1);
		break;
	case GOT_OBJECT_CACHE_TYPE_TREE:
		tree = ce->data.tree;
		if (tree->refcnt == 1)
			break;
		fprintf(stderr, "tree %s has %d unclaimed references\n",
		    id_str, tree->refcnt - 1);
		break;
	case GOT_OBJECT_CACHE_TYPE_COMMIT:
		commit = ce->data.commit;
		if (commit->refcnt == 1)
			break;
		fprintf(stderr, "commit %s has %d unclaimed references\n",
		    id_str, commit->refcnt);
		break;
	}
	free(id_str);
}
#endif

void
got_repo_close(struct got_repository *repo)
{
	int i;

	for (i = 0; i < nitems(repo->packidx_cache); i++) {
		if (repo->packidx_cache[i] == NULL)
			break;
		got_packidx_close(repo->packidx_cache[i]);
	}

	for (i = 0; i < nitems(repo->packs); i++) {
		if (repo->packs[i].path_packfile == NULL)
			break;
		got_pack_close(&repo->packs[i]);
	}

	free(repo->path);
	free(repo->path_git_dir);

#if 0
	print_cache_stats(&repo->objcache, "object");
	print_cache_stats(&repo->treecache, "tree");
	print_cache_stats(&repo->commitcache, "commit");
	got_object_idcache_for_each(repo->objcache.idcache, check_refcount,
	    &repo->objcache);
	got_object_idcache_for_each(repo->treecache.idcache, check_refcount,
	    &repo->treecache);
	got_object_idcache_for_each(repo->commitcache.idcache, check_refcount,
	    &repo->commitcache);
#endif

	if (repo->objcache.idcache)
		got_object_idcache_free(repo->objcache.idcache);
	if (repo->treecache.idcache)
		got_object_idcache_free(repo->treecache.idcache);
	if (repo->commitcache.idcache)
		got_object_idcache_free(repo->commitcache.idcache);
	free(repo);
}

const struct got_error *
got_repo_map_path(char **in_repo_path, struct got_repository *repo,
    const char *input_path)
{
	const struct got_error *err = NULL;
	char *repo_abspath = NULL, *cwd = NULL;
	struct stat sb;
	size_t repolen, cwdlen, len;
	char *canonpath, *path;

	*in_repo_path = NULL;

	cwd = getcwd(NULL, 0);
	if (cwd == NULL)
		return got_error_from_errno();

	canonpath = strdup(input_path);
	if (canonpath == NULL) {
		err = got_error_from_errno();
		goto done;
	}
	err = got_canonpath(input_path, canonpath, strlen(canonpath) + 1);
	if (err)
		goto done;

	repo_abspath = got_repo_get_path(repo);
	if (repo_abspath == NULL) {
		err = got_error_from_errno();
		goto done;
	}

	/* TODO: Call "get in-repository path of work-tree node" API. */

	if (lstat(canonpath, &sb) != 0) {
		if (errno != ENOENT) {
			err = got_error_from_errno();
			goto done;
		}
		/*
		 * Path is not on disk.
		 * Assume it is already relative to repository root.
		 */
		path = strdup(canonpath);
	} else {
		int is_repo_child = 0, is_cwd_child = 0;

		path = realpath(canonpath, NULL);
		if (path == NULL) {
			err = got_error_from_errno();
			goto done;
		}

		repolen = strlen(repo_abspath);
		cwdlen = strlen(cwd);
		len = strlen(path);

		if (len > repolen && strncmp(path, repo_abspath, repolen) == 0)
			is_repo_child = 1;
		if (len > cwdlen && strncmp(path, cwd, cwdlen) == 0)
			is_cwd_child = 1;

		if (strcmp(path, repo_abspath) == 0) {
			free(path);
			path = strdup("");
			if (path == NULL) {
				err = got_error_from_errno();
				goto done;
			}
		} else if (is_repo_child && is_cwd_child) {
			char *child;
			/* TODO: Is path inside a got worktree? */
			/* Strip common prefix with repository path. */
			err = got_path_skip_common_ancestor(&child,
			    repo_abspath, path);
			if (err)
				goto done;
			free(path);
			path = child;
		} else if (is_repo_child) {
			/* Matched an on-disk path inside repository. */
			if (got_repo_is_bare(repo)) {
				/*
				 * Matched an on-disk path inside repository
				 * database. Treat as repository-relative.
				 */
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
		} else if (is_cwd_child) {
			char *child;
			/* TODO: Is path inside a got worktree? */
			/* Strip common prefix with cwd. */
			err = got_path_skip_common_ancestor(&child, cwd,
			    path);
			if (err)
				goto done;
			free(path);
			path = child;
		} else {
			/*
			 * Matched unrelated on-disk path.
			 * Treat it as repository-relative.
			 */
		}
	}

	/* Make in-repository path absolute */
	if (path[0] != '/') {
		char *abspath;
		if (asprintf(&abspath, "/%s", path) == -1) {
			err = got_error_from_errno();
			goto done;
		}
		free(path);
		path = abspath;
	}

done:
	free(repo_abspath);
	free(cwd);
	free(canonpath);
	if (err)
		free(path);
	else
		*in_repo_path = path;
	return err;
}

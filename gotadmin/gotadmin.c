/*
 * Copyright (c) 2021 Stefan Sperling <stsp@openbsd.org>
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
#include <sys/types.h>

#include <ctype.h>
#include <getopt.h>
#include <err.h>
#include <errno.h>
#include <locale.h>
#include <inttypes.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#include "got_version.h"
#include "got_error.h"
#include "got_object.h"
#include "got_reference.h"
#include "got_cancel.h"
#include "got_repository.h"
#include "got_repository_admin.h"
#include "got_gotconfig.h"
#include "got_path.h"
#include "got_privsep.h"
#include "got_opentemp.h"

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

static volatile sig_atomic_t sigint_received;
static volatile sig_atomic_t sigpipe_received;

static void
catch_sigint(int signo)
{
	sigint_received = 1;
}

static void
catch_sigpipe(int signo)
{
	sigpipe_received = 1;
}

static const struct got_error *
check_cancelled(void *arg)
{
	if (sigint_received || sigpipe_received)
		return got_error(GOT_ERR_CANCELLED);
	return NULL;
}

struct gotadmin_cmd {
	const char	*cmd_name;
	const struct got_error *(*cmd_main)(int, char *[]);
	void		(*cmd_usage)(void);
	const char	*cmd_alias;
};

__dead static void	usage(int, int);
__dead static void	usage_info(void);
__dead static void	usage_pack(void);
__dead static void	usage_indexpack(void);
__dead static void	usage_listpack(void);
__dead static void	usage_cleanup(void);

static const struct got_error*		cmd_info(int, char *[]);
static const struct got_error*		cmd_pack(int, char *[]);
static const struct got_error*		cmd_indexpack(int, char *[]);
static const struct got_error*		cmd_listpack(int, char *[]);
static const struct got_error*		cmd_cleanup(int, char *[]);

static struct gotadmin_cmd gotadmin_commands[] = {
	{ "info",	cmd_info,	usage_info,	"" },
	{ "pack",	cmd_pack,	usage_pack,	"" },
	{ "indexpack",	cmd_indexpack,	usage_indexpack,"ix" },
	{ "listpack",	cmd_listpack,	usage_listpack,	"ls" },
	{ "cleanup",	cmd_cleanup,	usage_cleanup,	"cl" },
};

static void
list_commands(FILE *fp)
{
	size_t i;

	fprintf(fp, "commands:");
	for (i = 0; i < nitems(gotadmin_commands); i++) {
		struct gotadmin_cmd *cmd = &gotadmin_commands[i];
		fprintf(fp, " %s", cmd->cmd_name);
	}
	fputc('\n', fp);
}

int
main(int argc, char *argv[])
{
	struct gotadmin_cmd *cmd;
	size_t i;
	int ch;
	int hflag = 0, Vflag = 0;
	static struct option longopts[] = {
	    { "version", no_argument, NULL, 'V' },
	    { NULL, 0, NULL, 0 }
	};

	setlocale(LC_CTYPE, "");

	while ((ch = getopt_long(argc, argv, "+hV", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			hflag = 1;
			break;
		case 'V':
			Vflag = 1;
			break;
		default:
			usage(hflag, 1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;
	optind = 1;
	optreset = 1;

	if (Vflag) {
		got_version_print_str();
		return 0;
	}

	if (argc <= 0)
		usage(hflag, hflag ? 0 : 1);

	signal(SIGINT, catch_sigint);
	signal(SIGPIPE, catch_sigpipe);

	for (i = 0; i < nitems(gotadmin_commands); i++) {
		const struct got_error *error;

		cmd = &gotadmin_commands[i];

		if (strcmp(cmd->cmd_name, argv[0]) != 0 &&
		    strcmp(cmd->cmd_alias, argv[0]) != 0)
			continue;

		if (hflag)
			gotadmin_commands[i].cmd_usage();

		error = gotadmin_commands[i].cmd_main(argc, argv);
		if (error && error->code != GOT_ERR_CANCELLED &&
		    error->code != GOT_ERR_PRIVSEP_EXIT &&
		    !(sigpipe_received &&
		      error->code == GOT_ERR_ERRNO && errno == EPIPE) &&
		    !(sigint_received &&
		      error->code == GOT_ERR_ERRNO && errno == EINTR)) {
			fprintf(stderr, "%s: %s\n", getprogname(), error->msg);
			return 1;
		}

		return 0;
	}

	fprintf(stderr, "%s: unknown command '%s'\n", getprogname(), argv[0]);
	list_commands(stderr);
	return 1;
}

__dead static void
usage(int hflag, int status)
{
	FILE *fp = (status == 0) ? stdout : stderr;

	fprintf(fp, "usage: %s [-h] [-V | --version] command [arg ...]\n",
	    getprogname());
	if (hflag)
		list_commands(fp);
	exit(status);
}

static const struct got_error *
apply_unveil(const char *repo_path, int repo_read_only)
{
	const struct got_error *err;

#ifdef PROFILE
	if (unveil("gmon.out", "rwc") != 0)
		return got_error_from_errno2("unveil", "gmon.out");
#endif
	if (repo_path && unveil(repo_path, repo_read_only ? "r" : "rwc") != 0)
		return got_error_from_errno2("unveil", repo_path);

	if (unveil(GOT_TMPDIR_STR, "rwc") != 0)
		return got_error_from_errno2("unveil", GOT_TMPDIR_STR);

	err = got_privsep_unveil_exec_helpers();
	if (err != NULL)
		return err;

	if (unveil(NULL, NULL) != 0)
		return got_error_from_errno("unveil");

	return NULL;
}

__dead static void
usage_info(void)
{
	fprintf(stderr, "usage: %s info [-r repository-path]\n",
	    getprogname());
	exit(1);
}

static const struct got_error *
cmd_info(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	char *cwd = NULL, *repo_path = NULL;
	struct got_repository *repo = NULL;
	const struct got_gotconfig *gotconfig = NULL;
	int ch, npackfiles, npackedobj, nobj;
	off_t packsize, loose_size;
	char scaled[FMT_SCALED_STRSIZE];

	while ((ch = getopt(argc, argv, "r:")) != -1) {
		switch (ch) {
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			got_path_strip_trailing_slashes(repo_path);
			break;
		default:
			usage_info();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

#ifndef PROFILE
	if (pledge("stdio rpath wpath flock proc exec sendfd unveil",
	    NULL) == -1)
		err(1, "pledge");
#endif
	cwd = getcwd(NULL, 0);
	if (cwd == NULL) {
		error = got_error_from_errno("getcwd");
		goto done;
	}

	error = got_repo_open(&repo, repo_path ? repo_path : cwd, NULL);
	if (error)
		goto done;

	error = apply_unveil(got_repo_get_path_git_dir(repo), 1);
	if (error)
		goto done;

	printf("repository: %s\n", got_repo_get_path_git_dir(repo));

	gotconfig = got_repo_get_gotconfig(repo);
	if (gotconfig) {
		const struct got_remote_repo *remotes;
		int i, nremotes;
		if (got_gotconfig_get_author(gotconfig)) {
			printf("default author: %s\n",
			    got_gotconfig_get_author(gotconfig));
		}
		got_gotconfig_get_remotes(&nremotes, &remotes, gotconfig);
		for (i = 0; i < nremotes; i++) {
			printf("remote \"%s\": %s\n", remotes[i].name,
			    remotes[i].fetch_url);
		}
	}

	error = got_repo_get_packfile_info(&npackfiles, &npackedobj,
	    &packsize, repo);
	if (error)
		goto done;
	printf("pack files: %d\n", npackfiles);
	if (npackfiles > 0) {
		if (fmt_scaled(packsize, scaled) == -1) {
			error = got_error_from_errno("fmt_scaled");
			goto done;
		}
		printf("packed objects: %d\n", npackedobj);
		printf("packed total size: %s\n", scaled);
	}

	error = got_repo_get_loose_object_info(&nobj, &loose_size, repo);
	if (error)
		goto done;
	printf("loose objects: %d\n", nobj);
	if (nobj > 0) {
		if (fmt_scaled(loose_size, scaled) == -1) {
			error = got_error_from_errno("fmt_scaled");
			goto done;
		}
		printf("loose total size: %s\n", scaled);
	}
done:
	if (repo)
		got_repo_close(repo);
	free(cwd);
	return error;
}

__dead static void
usage_pack(void)
{
	fprintf(stderr, "usage: %s pack [-a] [-r repository-path] "
	    "[-x reference] [reference ...]\n",
	    getprogname());
	exit(1);
}

struct got_pack_progress_arg {
	char last_scaled_size[FMT_SCALED_STRSIZE];
	int last_ncommits;
	int last_nobj_total;
	int last_p_deltify;
	int last_p_written;
	int last_p_indexed;
	int last_p_resolved;
	int verbosity;
	int printed_something;
};

static const struct got_error *
pack_progress(void *arg, off_t packfile_size, int ncommits,
    int nobj_total, int nobj_deltify, int nobj_written)
{
	struct got_pack_progress_arg *a = arg;
	char scaled_size[FMT_SCALED_STRSIZE];
	int p_deltify, p_written;
	int print_searching = 0, print_total = 0;
	int print_deltify = 0, print_written = 0;

	if (a->verbosity < 0)
		return NULL;

	if (fmt_scaled(packfile_size, scaled_size) == -1)
		return got_error_from_errno("fmt_scaled");

	if (a->last_ncommits != ncommits) {
		print_searching = 1;
		a->last_ncommits = ncommits;
	}

	if (a->last_nobj_total != nobj_total) {
		print_searching = 1;
		print_total = 1;
		a->last_nobj_total = nobj_total;
	}

	if (packfile_size > 0 && (a->last_scaled_size[0] == '\0' ||
	    strcmp(scaled_size, a->last_scaled_size)) != 0) {
		if (strlcpy(a->last_scaled_size, scaled_size,
		    FMT_SCALED_STRSIZE) >= FMT_SCALED_STRSIZE)
			return got_error(GOT_ERR_NO_SPACE);
	}

	if (nobj_deltify > 0 || nobj_written > 0) {
		if (nobj_deltify > 0) {
			p_deltify = (nobj_deltify * 100) / nobj_total;
			if (p_deltify != a->last_p_deltify) {
				a->last_p_deltify = p_deltify;
				print_searching = 1;
				print_total = 1;
				print_deltify = 1;
			}
		}
		if (nobj_written > 0) {
			p_written = (nobj_written * 100) / nobj_total;
			if (p_written != a->last_p_written) {
				a->last_p_written = p_written;
				print_searching = 1;
				print_total = 1;
				print_deltify = 1;
				print_written = 1;
			}
		}
	}

	if (print_searching || print_total || print_deltify || print_written)
		printf("\r");
	if (print_searching)
		printf("packing %d reference%s", ncommits,
		    ncommits == 1 ? "" : "s");
	if (print_total)
		printf("; %d object%s", nobj_total,
		    nobj_total == 1 ? "" : "s");
	if (print_deltify)
		printf("; deltify: %d%%", p_deltify);
	if (print_written)
		printf("; writing pack: %*s %d%%", FMT_SCALED_STRSIZE,
		    scaled_size, p_written);
	if (print_searching || print_total || print_deltify ||
	    print_written) {
		a->printed_something = 1;
		fflush(stdout);
	}
	return NULL;
}

static const struct got_error *
pack_index_progress(void *arg, off_t packfile_size, int nobj_total,
    int nobj_indexed, int nobj_loose, int nobj_resolved)
{
	struct got_pack_progress_arg *a = arg;
	char scaled_size[FMT_SCALED_STRSIZE];
	int p_indexed, p_resolved;
	int print_size = 0, print_indexed = 0, print_resolved = 0;

	if (a->verbosity < 0)
		return NULL;

	if (packfile_size > 0 || nobj_indexed > 0) {
		if (fmt_scaled(packfile_size, scaled_size) == 0 &&
		    (a->last_scaled_size[0] == '\0' ||
		    strcmp(scaled_size, a->last_scaled_size)) != 0) {
			print_size = 1;
			if (strlcpy(a->last_scaled_size, scaled_size,
			    FMT_SCALED_STRSIZE) >= FMT_SCALED_STRSIZE)
				return got_error(GOT_ERR_NO_SPACE);
		}
		if (nobj_indexed > 0) {
			p_indexed = (nobj_indexed * 100) / nobj_total;
			if (p_indexed != a->last_p_indexed) {
				a->last_p_indexed = p_indexed;
				print_indexed = 1;
				print_size = 1;
			}
		}
		if (nobj_resolved > 0) {
			p_resolved = (nobj_resolved * 100) /
			    (nobj_total - nobj_loose);
			if (p_resolved != a->last_p_resolved) {
				a->last_p_resolved = p_resolved;
				print_resolved = 1;
				print_indexed = 1;
				print_size = 1;
			}
		}

	}
	if (print_size || print_indexed || print_resolved)
		printf("\r");
	if (print_size)
		printf("%*s packed", FMT_SCALED_STRSIZE, scaled_size);
	if (print_indexed)
		printf("; indexing %d%%", p_indexed);
	if (print_resolved)
		printf("; resolving deltas %d%%", p_resolved);
	if (print_size || print_indexed || print_resolved)
		fflush(stdout);

	return NULL;
}

static const struct got_error *
add_ref(struct got_reflist_entry **new, struct got_reflist_head *refs,
    const char *refname, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_reference *ref;

	*new = NULL;

	err = got_ref_open(&ref, repo, refname, 0);
	if (err) {
		if (err->code != GOT_ERR_NOT_REF)
			return err;

		/* Treat argument as a reference prefix. */
		err = got_ref_list(refs, repo, refname,
		    got_ref_cmp_by_name, NULL);
	} else {
		err = got_reflist_insert(new, refs, ref,
		    got_ref_cmp_by_name, NULL);
		if (err || *new == NULL /* duplicate */)
			got_ref_close(ref);
	}

	return err;
}

static const struct got_error *
cmd_pack(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	char *cwd = NULL, *repo_path = NULL;
	struct got_repository *repo = NULL;
	int ch, i, loose_obj_only = 1;
	struct got_object_id *pack_hash = NULL;
	char *id_str = NULL;
	struct got_pack_progress_arg ppa;
	FILE *packfile = NULL;
	struct got_pathlist_head exclude_args;
	struct got_pathlist_entry *pe;
	struct got_reflist_head exclude_refs;
	struct got_reflist_head include_refs;
	struct got_reflist_entry *re, *new;

	TAILQ_INIT(&exclude_args);
	TAILQ_INIT(&exclude_refs);
	TAILQ_INIT(&include_refs);

	while ((ch = getopt(argc, argv, "ar:x:")) != -1) {
		switch (ch) {
		case 'a':
			loose_obj_only = 0;
			break;
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			got_path_strip_trailing_slashes(repo_path);
			break;
		case 'x':
			got_path_strip_trailing_slashes(optarg);
			error = got_pathlist_append(&exclude_args,
			    optarg, NULL);
			if (error)
				return error;
			break;
		default:
			usage_pack();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

#ifndef PROFILE
	if (pledge("stdio rpath wpath cpath fattr flock proc exec sendfd unveil",
	    NULL) == -1)
		err(1, "pledge");
#endif
	cwd = getcwd(NULL, 0);
	if (cwd == NULL) {
		error = got_error_from_errno("getcwd");
		goto done;
	}

	error = got_repo_open(&repo, repo_path ? repo_path : cwd, NULL);
	if (error)
		goto done;

	error = apply_unveil(got_repo_get_path_git_dir(repo), 0);
	if (error)
		goto done;

	TAILQ_FOREACH(pe, &exclude_args, entry) {
		const char *refname = pe->path;
		error = add_ref(&new, &exclude_refs, refname, repo);
		if (error)
			goto done;

	}

	if (argc == 0) {
		error = got_ref_list(&include_refs, repo, "",
		    got_ref_cmp_by_name, NULL);
		if (error)
			goto done;
	} else {
		for (i = 0; i < argc; i++) {
			const char *refname;
			got_path_strip_trailing_slashes(argv[i]);
			refname = argv[i];
			error = add_ref(&new, &include_refs, refname, repo);
			if (error)
				goto done;
		}
	}

	/* Ignore references in the refs/got/ namespace. */
	TAILQ_FOREACH_SAFE(re, &include_refs, entry, new) {
		const char *refname = got_ref_get_name(re->ref);
		if (strncmp("refs/got/", refname, 9) != 0)
			continue;
		TAILQ_REMOVE(&include_refs, re, entry);
		got_ref_close(re->ref);
		free(re);
	}

	memset(&ppa, 0, sizeof(ppa));
	ppa.last_scaled_size[0] = '\0';
	ppa.last_p_indexed = -1;
	ppa.last_p_resolved = -1;

	error = got_repo_pack_objects(&packfile, &pack_hash,
	    &include_refs, &exclude_refs, repo, loose_obj_only,
	    pack_progress, &ppa, check_cancelled, NULL);
	if (error) {
		if (ppa.printed_something)
			printf("\n");
		goto done;
	}

	error = got_object_id_str(&id_str, pack_hash);
	if (error)
		goto done;
	printf("\nWrote %s.pack\n", id_str);

	error = got_repo_index_pack(packfile, pack_hash, repo,
	    pack_index_progress, &ppa, check_cancelled, NULL);
	if (error)
		goto done;
	printf("\nIndexed %s.pack\n", id_str);
done:
	got_pathlist_free(&exclude_args);
	got_ref_list_free(&exclude_refs);
	got_ref_list_free(&include_refs);
	free(id_str);
	free(pack_hash);
	free(cwd);
	return error;
}

__dead static void
usage_indexpack(void)
{
	fprintf(stderr, "usage: %s indexpack packfile-path\n",
	    getprogname());
	exit(1);
}

static const struct got_error *
cmd_indexpack(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	struct got_repository *repo = NULL;
	int ch;
	struct got_object_id *pack_hash = NULL;
	char *packfile_path = NULL;
	char *id_str = NULL;
	struct got_pack_progress_arg ppa;
	FILE *packfile = NULL;

	while ((ch = getopt(argc, argv, "")) != -1) {
		switch (ch) {
		default:
			usage_indexpack();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage_indexpack();

	packfile_path = realpath(argv[0], NULL);
	if (packfile_path == NULL)
		return got_error_from_errno2("realpath", argv[0]);

#ifndef PROFILE
	if (pledge("stdio rpath wpath cpath fattr flock proc exec sendfd unveil",
	    NULL) == -1)
		err(1, "pledge");
#endif

	error = got_repo_open(&repo, packfile_path, NULL);
	if (error)
		goto done;

	error = apply_unveil(got_repo_get_path_git_dir(repo), 1);
	if (error)
		goto done;

	memset(&ppa, 0, sizeof(ppa));
	ppa.last_scaled_size[0] = '\0';
	ppa.last_p_indexed = -1;
	ppa.last_p_resolved = -1;

	error = got_repo_find_pack(&packfile, &pack_hash, repo,
	    packfile_path);
	if (error)
		goto done;

	error = got_object_id_str(&id_str, pack_hash);
	if (error)
		goto done;

	error = got_repo_index_pack(packfile, pack_hash, repo,
	    pack_index_progress, &ppa, check_cancelled, NULL);
	if (error)
		goto done;
	printf("\nIndexed %s.pack\n", id_str);
done:
	free(id_str);
	free(pack_hash);
	return error;
}

__dead static void
usage_listpack(void)
{
	fprintf(stderr, "usage: %s listpack [-h] [-s] packfile-path\n",
	    getprogname());
	exit(1);
}

struct gotadmin_list_pack_cb_args {
	int nblobs;
	int ntrees;
	int ncommits;
	int ntags;
	int noffdeltas;
	int nrefdeltas;
	int human_readable;
};

static const struct got_error *
list_pack_cb(void *arg, struct got_object_id *id, int type, off_t offset,
    off_t size, off_t base_offset, struct got_object_id *base_id)
{
	const struct got_error *err;
	struct gotadmin_list_pack_cb_args *a = arg;
	char *id_str, *delta_str = NULL, *base_id_str = NULL;
	const char *type_str;

	err = got_object_id_str(&id_str, id);	
	if (err)
		return err;

	switch (type) {
	case GOT_OBJ_TYPE_BLOB:
		type_str = GOT_OBJ_LABEL_BLOB;
		a->nblobs++;
		break;
	case GOT_OBJ_TYPE_TREE:
		type_str = GOT_OBJ_LABEL_TREE;
		a->ntrees++;
		break;
	case GOT_OBJ_TYPE_COMMIT:
		type_str = GOT_OBJ_LABEL_COMMIT;
		a->ncommits++;
		break;
	case GOT_OBJ_TYPE_TAG:
		type_str = GOT_OBJ_LABEL_TAG;
		a->ntags++;
		break;
	case GOT_OBJ_TYPE_OFFSET_DELTA:
		type_str = "offset-delta";
		if (asprintf(&delta_str, " base-offset %llu",
		    base_offset) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		a->noffdeltas++;
		break;
	case GOT_OBJ_TYPE_REF_DELTA:
		type_str = "ref-delta";
		err = got_object_id_str(&base_id_str, base_id);	
		if (err)
			goto done;
		if (asprintf(&delta_str, " base-id %s", base_id_str) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		a->nrefdeltas++;
		break;
	default:
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}
	if (a->human_readable) {
		char scaled[FMT_SCALED_STRSIZE];
		char *s;;
		if (fmt_scaled(size, scaled) == -1) {
			err = got_error_from_errno("fmt_scaled");
			goto done;
		}
		s = scaled;
		while (isspace((unsigned char)*s))
			s++;
		printf("%s %s at %llu size %s%s\n", id_str, type_str, offset,
		    s, delta_str ? delta_str : "");
	} else {
		printf("%s %s at %llu size %llu%s\n", id_str, type_str, offset,
		    size, delta_str ? delta_str : "");
	}
done:
	free(id_str);
	free(base_id_str);
	free(delta_str);
	return err;
}

static const struct got_error *
cmd_listpack(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	struct got_repository *repo = NULL;
	int ch;
	struct got_object_id *pack_hash = NULL;
	char *packfile_path = NULL;
	char *id_str = NULL;
	struct gotadmin_list_pack_cb_args lpa;
	FILE *packfile = NULL;
	int show_stats = 0, human_readable = 0;

	while ((ch = getopt(argc, argv, "hs")) != -1) {
		switch (ch) {
		case 'h':
			human_readable = 1;
			break;
		case 's':
			show_stats = 1;
			break;
		default:
			usage_listpack();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage_listpack();
	packfile_path = realpath(argv[0], NULL);
	if (packfile_path == NULL)
		return got_error_from_errno2("realpath", argv[0]);

#ifndef PROFILE
	if (pledge("stdio rpath wpath flock proc exec sendfd unveil",
	    NULL) == -1)
		err(1, "pledge");
#endif
	error = got_repo_open(&repo, packfile_path, NULL);
	if (error)
		goto done;

	error = apply_unveil(got_repo_get_path_git_dir(repo), 1);
	if (error)
		goto done;

	error = got_repo_find_pack(&packfile, &pack_hash, repo,
	    packfile_path);
	if (error)
		goto done;
	error = got_object_id_str(&id_str, pack_hash);
	if (error)
		goto done;

	memset(&lpa, 0, sizeof(lpa));
	lpa.human_readable = human_readable;
	error = got_repo_list_pack(packfile, pack_hash, repo,
	    list_pack_cb, &lpa, check_cancelled, NULL);
	if (error)
		goto done;
	if (show_stats) {
		printf("objects: %d\n  blobs: %d\n  trees: %d\n  commits: %d\n"
		    "  tags: %d\n  offset-deltas: %d\n  ref-deltas: %d\n",
		    lpa.nblobs + lpa.ntrees + lpa.ncommits + lpa.ntags +
		    lpa.noffdeltas + lpa.nrefdeltas,
		    lpa.nblobs, lpa.ntrees, lpa.ncommits, lpa.ntags,
		    lpa.noffdeltas, lpa.nrefdeltas);
	}
done:
	free(id_str);
	free(pack_hash);
	free(packfile_path);
	return error;
}

__dead static void
usage_cleanup(void)
{
	fprintf(stderr, "usage: %s cleanup [-a] [-p] [-n] [-r repository-path] "
	    "[-q]\n", getprogname());
	exit(1);
}

struct got_cleanup_progress_arg {
	int last_nloose;
	int last_ncommits;
	int last_npurged;
	int verbosity;
	int printed_something;
	int dry_run;
};

static const struct got_error *
cleanup_progress(void *arg, int nloose, int ncommits, int npurged)
{
	struct got_cleanup_progress_arg *a = arg;
	int print_loose = 0, print_commits = 0, print_purged = 0;

	if (a->last_nloose != nloose) {
		print_loose = 1;
		a->last_nloose = nloose;
	}
	if (a->last_ncommits != ncommits) {
		print_loose = 1;
		print_commits = 1;
		a->last_ncommits = ncommits;
	}
	if (a->last_npurged != npurged) {
		print_loose = 1;
		print_commits = 1;
		print_purged = 1;
		a->last_npurged = npurged;
	}

	if (a->verbosity < 0)
		return NULL;

	if (print_loose || print_commits || print_purged)
		printf("\r");
	if (print_loose)
		printf("%d loose object%s", nloose, nloose == 1 ? "" : "s");
	if (print_commits)
		printf("; %d commit%s scanned", ncommits,
		    ncommits == 1 ? "" : "s");
	if (print_purged) {
		if (a->dry_run) {
			printf("; %d object%s could be purged", npurged,
			    npurged == 1 ? "" : "s");
		} else {
			printf("; %d object%s purged", npurged,
			    npurged == 1 ? "" : "s");
		}
	}
	if (print_loose || print_commits || print_purged) {
		a->printed_something = 1;
		fflush(stdout);
	}
	return NULL;
}

struct got_lonely_packidx_progress_arg {
	int verbosity;
	int printed_something;
	int dry_run;
};

static const struct got_error *
lonely_packidx_progress(void *arg, const char *path)
{
	struct got_lonely_packidx_progress_arg *a = arg;

	if (a->verbosity < 0)
		return NULL;

	if (a->dry_run)
		printf("%s could be removed\n", path);
	else
		printf("%s removed\n", path);

	a->printed_something = 1;
	return NULL;
}

static const struct got_error *
cmd_cleanup(int argc, char *argv[])
{
	const struct got_error *error = NULL;
	char *cwd = NULL, *repo_path = NULL;
	struct got_repository *repo = NULL;
	int ch, dry_run = 0, npacked = 0, verbosity = 0;
	int remove_lonely_packidx = 0, ignore_mtime = 0;
	struct got_cleanup_progress_arg cpa;
	struct got_lonely_packidx_progress_arg lpa;
	off_t size_before, size_after;
	char scaled_before[FMT_SCALED_STRSIZE];
	char scaled_after[FMT_SCALED_STRSIZE];
	char scaled_diff[FMT_SCALED_STRSIZE];
	char **extensions;
	int nextensions, i;

	while ((ch = getopt(argc, argv, "apr:nq")) != -1) {
		switch (ch) {
		case 'a':
			ignore_mtime = 1;
			break;
		case 'p':
			remove_lonely_packidx = 1;
			break;
		case 'r':
			repo_path = realpath(optarg, NULL);
			if (repo_path == NULL)
				return got_error_from_errno2("realpath",
				    optarg);
			got_path_strip_trailing_slashes(repo_path);
			break;
		case 'n':
			dry_run = 1;
			break;
		case 'q':
			verbosity = -1;
			break;
		default:
			usage_cleanup();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

#ifndef PROFILE
	if (pledge("stdio rpath wpath cpath flock proc exec sendfd unveil",
	    NULL) == -1)
		err(1, "pledge");
#endif
	cwd = getcwd(NULL, 0);
	if (cwd == NULL) {
		error = got_error_from_errno("getcwd");
		goto done;
	}

	error = got_repo_open(&repo, repo_path ? repo_path : cwd, NULL);
	if (error)
		goto done;

	error = apply_unveil(got_repo_get_path_git_dir(repo), 0);
	if (error)
		goto done;

	got_repo_get_gitconfig_extensions(&extensions, &nextensions,
	    repo);
	for (i = 0; i < nextensions; i++) {
		if (strcasecmp(extensions[i], "preciousObjects") == 0) {
			error = got_error_msg(GOT_ERR_GIT_REPO_EXT,
			    "the preciousObjects Git extension is enabled; "
			    "this implies that objects must not be deleted");
			goto done;
		}
	}

	if (remove_lonely_packidx) {
		memset(&lpa, 0, sizeof(lpa));
		lpa.dry_run = dry_run;
		lpa.verbosity = verbosity;
		error = got_repo_remove_lonely_packidx(repo, dry_run,
		    lonely_packidx_progress, &lpa, check_cancelled, NULL);
		goto done;
	}

	memset(&cpa, 0, sizeof(cpa));
	cpa.last_ncommits = -1;
	cpa.last_npurged = -1;
	cpa.dry_run = dry_run;
	cpa.verbosity = verbosity;
	error = got_repo_purge_unreferenced_loose_objects(repo,
	    &size_before, &size_after, &npacked, dry_run, ignore_mtime,
	    cleanup_progress, &cpa, check_cancelled, NULL);
	if (cpa.printed_something)
		printf("\n");
	if (error)
		goto done;
	if (cpa.printed_something) {
		if (fmt_scaled(size_before, scaled_before) == -1) {
			error = got_error_from_errno("fmt_scaled");
			goto done;
		}
		if (fmt_scaled(size_after, scaled_after) == -1) {
			error = got_error_from_errno("fmt_scaled");
			goto done;
		}
		if (fmt_scaled(size_before - size_after, scaled_diff) == -1) {
			error = got_error_from_errno("fmt_scaled");
			goto done;
		}
		printf("loose total size before: %s\n", scaled_before);
		printf("loose total size after: %s\n", scaled_after);
		if (dry_run) {
			printf("disk space which would be freed: %s\n",
			    scaled_diff);
		} else
			printf("disk space freed: %s\n", scaled_diff);
		printf("loose objects also found in pack files: %d\n", npacked);
	}
done:
	if (repo)
		got_repo_close(repo);
	free(cwd);
	return error;
}

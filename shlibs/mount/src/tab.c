/*
 * Copyright (C) 2008 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 *
 * Note:
 *	mnt_tab_find_* functions are mount(8) compatible. It means it tries
 *	to found an entry in more iterations where the first attempt is always
 *	based on comparison with unmodified (non-canonicalized or un-evaluated)
 *	paths or tags. For example fstab with two entries:
 *
 *		LABEL=foo	/foo	auto   rw
 *		/dev/foo	/foo	auto   rw
 *
 *	where both lines are used for the *same* device, then
 *
 *		mnt_tab_find_source(tb, "/dev/foo", &fs);
 *
 *	will returns the second line, and
 *
 *		mnt_tab_find_source(tb, "LABEL=foo", &fs);
 *
 *	will returns the first entry, and
 *
 *		mnt_tab_find_source(tb, "UUID=<anyuuid>", &fs);
 *
 *	will returns the first entry (if UUID matches with the device).
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <blkid/blkid.h>

#include "nls.h"
#include "mountP.h"

/**
 * mnt_new_tab:
 * @filename: file name or NULL
 *
 * The tab is a container for mnt_fs entries that usually represents a fstab,
 * mtab or mountinfo file from your system.
 *
 * Note that this function does not parse the file. See also
 * mnt_tab_parse_file().
 *
 * Returns newly allocated tab struct.
 */
mnt_tab *mnt_new_tab(const char *filename)
{
	mnt_tab *tb = NULL;

	tb = calloc(1, sizeof(struct _mnt_tab));
	if (!tb)
		goto err;

	if (filename) {
		tb->filename = strdup(filename);
		if (!tb->filename)
			goto err;
	}
	INIT_LIST_HEAD(&tb->ents);
	return tb;
err:
	free(tb);
	return NULL;
}

/**
 * mnt_free_tab:
 * @tab: tab pointer
 *
 * Deallocates tab struct and all entries.
 */
void mnt_free_tab(mnt_tab *tb)
{
	if (!tb)
		return;
	free(tb->filename);

	while (!list_empty(&tb->ents)) {
		mnt_fs *fs = list_entry(tb->ents.next, mnt_fs, ents);
		mnt_free_fs(fs);
	}

	free(tb);
}

/**
 * mnt_tab_get_nents:
 * @tb: pointer to tab
 *
 * Returns number of valid entries in tab.
 */
int mnt_tab_get_nents(mnt_tab *tb)
{
	assert(tb);
	return tb ? tb->nents : 0;
}

/**
 * mnt_tab_set_cache:
 * @tb: pointer to tab
 * @mpc: pointer to mnt_cache instance
 *
 * Setups a cache for canonicalized paths and evaluated tags (LABEL/UUID). The
 * cache is recommended for mnt_tab_find_*() functions.
 *
 * The cache could be shared between more tabs. Be careful when you share the
 * same cache between more threads -- currently the cache does not provide any
 * locking method.
 *
 * See also mnt_new_cache().
 *
 * Returns 0 on success or -1 in case of error.
 */
int mnt_tab_set_cache(mnt_tab *tb, mnt_cache *mpc)
{
	assert(tb);
	if (!tb)
		return -1;
	tb->cache = mpc;
	return 0;
}

/**
 * mnt_tab_get_cache:
 * @tb: pointer to tab
 *
 * Returns pointer to mnt_cache instance or NULL.
 */
mnt_cache *mnt_tab_get_cache(mnt_tab *tb)
{
	assert(tb);
	return tb ? tb->cache : NULL;
}

/**
 * mnt_tab_get_name:
 * @tb: tab pointer
 *
 * Returns tab filename or NULL.
 */
const char *mnt_tab_get_name(mnt_tab *tb)
{
	assert(tb);
	return tb ? tb->filename : NULL;
}

/**
 * mnt_tab_add_fs:
 * @tb: tab pointer
 * @fs: new entry
 *
 * Adds a new entry to tab.
 *
 * Returns 0 on success or -1 in case of error.
 */
int mnt_tab_add_fs(mnt_tab *tb, mnt_fs *fs)
{
	assert(tb);
	assert(fs);

	if (!tb || !fs)
		return -1;

	list_add_tail(&fs->ents, &tb->ents);

	DBG(DEBUG_TAB, fprintf(stderr,
		"libmount: %s: add entry: %s %s\n",
		tb->filename, mnt_fs_get_source(fs),
		mnt_fs_get_target(fs)));

	if (fs->flags & MNT_FS_ERROR)
		tb->nerrs++;
	else
		tb->nents++;
	return 0;
}

/**
 * mnt_tab_remove_fs:
 * @tb: tab pointer
 * @fs: new entry
 *
 * Returns 0 on success or -1 in case of error.
 */
int mnt_tab_remove_fs(mnt_tab *tb, mnt_fs *fs)
{
	assert(tb);
	assert(fs);

	if (!tb || !fs)
		return -1;

	list_del(&fs->ents);

	if (fs->flags & MNT_FS_ERROR)
		tb->nerrs--;
	else
		tb->nents--;
	return 0;
}

/**
 * mnt_tab_next_fs:
 * @tb: tab pointer
 * @itr: iterator
 * @fs: returns the next tab entry
 *
 * Returns 0 on success, -1 in case of error or 1 at end of list.
 *
 * Example (list all mountpoints from fstab in backward order):
 *
 *	mnt_fs *fs;
 *	mnt_tab *tb = mnt_new_tab("/etc/fstab");
 *	mnt_iter *itr = mnt_new_iter(MNT_ITER_BACKWARD);
 *
 *	mnt_tab_parse_file(tb);
 *
 *	while(mnt_tab_next_fs(tb, itr, &fs) == 0) {
 *		const char *dir = mnt_fs_get_target(fs);
 *		printf("mount point: %s\n", dir);
 *	}
 *	mnt_free_tab(fi);
 */
int mnt_tab_next_fs(mnt_tab *tb, mnt_iter *itr, mnt_fs **fs)
{
	assert(tb);
	assert(itr);
	assert(fs);

	if (!tb || !itr || !fs)
		return -1;
again:
	if (!itr->head)
		MNT_ITER_INIT(itr, &tb->ents);
	if (itr->p != itr->head) {
		MNT_ITER_ITERATE(itr, *fs, struct _mnt_fs, ents);
		return 0;
	}

	/* ignore broken entries */
	if (*fs && ((*fs)->flags & MNT_FS_ERROR))
		goto again;

	return 1;
}

/**
 * mnt_tab_set_iter:
 * @tb: tab pointer
 * @itr: iterator
 * @fs: tab entry
 *
 * Sets @iter to the position of @fs in the file @tb.
 *
 * Returns 0 on success, -1 in case of error.
 */
int mnt_tab_set_iter(mnt_tab *tb, mnt_iter *itr, mnt_fs *fs)
{
	assert(tb);
	assert(itr);
	assert(fs);

	if (!tb || !itr || !fs)
		return -1;

	MNT_ITER_INIT(itr, &tb->ents);
	itr->p = &fs->ents;

	return 0;
}

/**
 * mnt_tab_find_target:
 * @tb: tab pointer
 * @path: mountpoint directory
 * @direction: MNT_ITER_{FORWARD,BACKWARD}
 *
 * Try to lookup an entry in given tab, possible are three iterations, first
 * with @path, second with realpath(@path) and third with realpath(@path)
 * against realpath(fs->target). The 2nd and 3rd iterations are not performed
 * when @tb cache is not set (see mnt_tab_set_cache()).
 *
 * Returns a tab entry or NULL.
 */
mnt_fs *mnt_tab_find_target(mnt_tab *tb, const char *path, int direction)
{
	mnt_iter itr;
	mnt_fs *fs = NULL;
	char *cn;

	assert(tb);
	assert(path);

	DBG(DEBUG_TAB, fprintf(stderr,
		"libmount: %s: lookup target: %s\n", tb->filename, path));

	/* native @target */
	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0)
		if (fs->target && strcmp(fs->target, path) == 0)
			return fs;

	if (!tb->cache || !(cn = mnt_resolve_path(path, tb->cache)))
		return NULL;

	/* canonicalized paths in mnt_tab */
	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		if (fs->target && strcmp(fs->target, cn) == 0)
			return fs;
	}

	/* non-canonicaled path in mnt_tab */
	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		char *p;
		if (!fs->target)
		       continue;
		p = mnt_resolve_path(fs->target, tb->cache);
		if (strcmp(cn, p) == 0)
			return fs;
	}
	return NULL;
}

/**
 * mnt_tab_find_srcpath:
 * @tb: tab pointer
 * @path: source path (devname or dirname)
 * @direction: MNT_ITER_{FORWARD,BACKWARD}
 *
 * Try to lookup an entry in given tab, possible are four iterations, first
 * with @path, second with realpath(@path), third with tags (LABEL, UUID, ..)
 * from @path and fourth with realpath(@path) against realpath(entry->srcpath).
 *
 * The 2nd, 3rd and 4th iterations are not performed when @tb cache is not
 * set (see mnt_tab_set_cache()).
 *
 * Returns a tab entry or NULL.
 */
mnt_fs *mnt_tab_find_srcpath(mnt_tab *tb, const char *path, int direction)
{
	mnt_iter itr;
	mnt_fs *fs = NULL;
	int ntags = 0;
	char *cn;
	const char *p;

	assert(tb);
	assert(path);

	DBG(DEBUG_TAB, fprintf(stderr,
		"libmount: %s: lookup srcpath: %s\n", tb->filename, path));

	/* native paths */
	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		p = mnt_fs_get_srcpath(fs);
		if (p && strcmp(p, path) == 0)
			return fs;
		if (!p)
			/* mnt_fs_get_srcpath() returs nothing, it's TAG */
			ntags++;
	}

	if (!tb->cache || !(cn = mnt_resolve_path(path, tb->cache)))
		return NULL;

	/* canonicalized paths in mnt_tab */
	if (ntags < mnt_tab_get_nents(tb)) {
		mnt_reset_iter(&itr, direction);
		while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
			p = mnt_fs_get_srcpath(fs);
			if (p && strcmp(p, cn) == 0)
				return fs;
		}
	}

	/* evaluated tag */
	if (ntags && mnt_cache_read_tags(tb->cache, cn) > 0) {
		mnt_reset_iter(&itr, direction);
		while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
			const char *t, *v;

			if (mnt_fs_get_tag(fs, &t, &v))
				continue;

			if (mnt_cache_device_has_tag(tb->cache, cn, t, v))
				return fs;
		}
	}

	/* non-canonicalized paths in mnt_tab */
	if (ntags <= mnt_tab_get_nents(tb)) {
		mnt_reset_iter(&itr, direction);
		while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
			p = mnt_fs_get_srcpath(fs);
			if (p)
				p = mnt_resolve_path(p, tb->cache);
			if (p && strcmp(cn, p) == 0)
				return fs;
		}
	}

	return NULL;
}


/**
 * mnt_tab_find_tag:
 * @tb: tab pointer
 * @tag: tag name (e.g "LABEL", "UUID", ...)
 * @val: tag value
 * @direction: MNT_ITER_{FORWARD,BACKWARD}
 *
 * Try to lookup an entry in given tab, first attempt is lookup by @tag and
 * @val, for the second attempt the tag is evaluated (converted to the device
 * name) and mnt_tab_find_srcpath() is preformed. The second attempt is not
 * performed when @tb cache is not set (see mnt_tab_set_cache()).

 * Returns a tab entry or NULL.
 */
mnt_fs *mnt_tab_find_tag(mnt_tab *tb, const char *tag,
			const char *val, int direction)
{
	mnt_iter itr;
	mnt_fs *fs = NULL;

	assert(tb);
	assert(tag);
	assert(val);

	if (!tb || !tag || !val)
		return NULL;

	DBG(DEBUG_TAB, fprintf(stderr,
		"libmount: %s: lookup by TAG: %s %s\n", tb->filename, tag, val));

	/* look up by TAG */
	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		if (fs->tagname && fs->tagval &&
		    strcmp(fs->tagname, tag) == 0 &&
		    strcmp(fs->tagval, val) == 0)
			return fs;
	}

	if (tb->cache) {
		/* look up by device name */
		char *cn = mnt_resolve_tag(tag, val, tb->cache);
		if (cn)
			return mnt_tab_find_srcpath(tb, cn, direction);
	}
	return NULL;
}

/**
 * mnt_tab_find_source:
 * @tb: tab pointer
 * @source: TAG or path
 *
 * This is high-level API for mnt_tab_find_{srcpath,tag}. You needn't to care
 * about @source format (device, LABEL, UUID, ...). This function parses @source
 * and calls mnt_tab_find_tag() or mnt_tab_find_srcpath().
 *
 * Returns a tab entry or NULL.
 */
mnt_fs *mnt_tab_find_source(mnt_tab *tb, const char *source, int direction)
{
	mnt_fs *fs = NULL;

	assert(tb);
	assert(source);

	if (!tb || !source)
		return NULL;

	DBG(DEBUG_TAB, fprintf(stderr,
		"libmount: %s: lookup SOURCE: %s\n", tb->filename, source));

	if (strchr(source, '=')) {
		char *tag, *val;

		if (blkid_parse_tag_string(source, &tag, &val) == 0) {

			fs = mnt_tab_find_tag(tb, tag, val, direction);

			free(tag);
			free(val);
		}
	} else
		fs = mnt_tab_find_srcpath(tb, source, direction);

	return fs;
}

/**
 * mnt_tab_find_pair:
 * @tb: tab pointer
 * @srcpath: canonicalized source path (devname or dirname)
 * @target: canonicalized mountpoint
 * @direction: MNT_ITER_{FORWARD,BACKWARD}
 *
 * Returns a tab entry or NULL.
 */
mnt_fs *mnt_tab_find_pair(mnt_tab *tb, const char *srcpath,
			const char *target, int direction)
{
	mnt_iter itr;
	mnt_fs *fs;
	int has_tags = -1;
	const char *p;

	assert(tb);
	assert(srcpath);
	assert(target);

	DBG(DEBUG_TAB, fprintf(stderr,
		"libmount: %s: lookup pair: srcpath(%s) target(%s)\n",
		tb->filename, srcpath, target));

	/* native paths */
	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		if (!fs->target || strcmp(fs->target, target))
			continue;
		p = mnt_fs_get_srcpath(fs);
		if (p && strcmp(p, srcpath) == 0)
			return fs;
	}

	if (!tb->cache)
		return NULL;

	mnt_reset_iter(&itr, direction);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		const char *src;

		if (!fs->target)
			continue;

		/* canonicalized or non-canonicalied target */
		if (strcmp(fs->target, target)) {
			p = mnt_resolve_path(fs->target, tb->cache);
			if (!p || strcmp(p, target))
				continue;
		}

		src = mnt_fs_get_srcpath(fs);
		if (src) {
			/* canonicalized or non-canonicalied srcpath */
			if (strcmp(src, srcpath)) {
				p = mnt_resolve_path(src, tb->cache);
				if (!p || strcmp(p, srcpath))
					continue;
			}
		} else if (has_tags != 0) {
			/* entry source is tag */
			const char *t, *v;

			if (mnt_fs_get_tag(fs, &t, &v))
				continue;
			if (has_tags == -1 &&
			    mnt_cache_read_tags(tb->cache, srcpath)) {
				has_tags = 0;
				continue;
			}
			has_tags = 1;
			if (!mnt_cache_device_has_tag(tb->cache, srcpath, t, v))
				continue;
		} else
			continue;

		return fs;
	}
	return NULL;
}

/**
 * mnt_tab_fprintf:
 * @f: FILE
 * @fmt: per line printf-like format string (see MNT_MFILE_PRINTFMT)
 * @tb: tab pointer
 *
 * Returns 0 on success, -1 in case of error.
 */
int mnt_tab_fprintf(mnt_tab *tb, FILE *f, const char *fmt)
{
	mnt_iter itr;
	mnt_fs *fs;

	assert(f);
	assert(fmt);
	assert(tb);

	if (!f || !fmt || !tb)
		return -1;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);
	while(mnt_tab_next_fs(tb, &itr, &fs) == 0) {
		if (mnt_fs_fprintf(fs, f, fmt) == -1)
			return -1;
	}

	return 0;
}

/**
 * mnt_tab_update_file
 * @tb: tab pointer
 *
 * Writes tab to disk. Don't forget to lock the file (see mnt_lock()).
 *
 * Returns 0 on success, -1 in case of error.
 */
int mnt_tab_update_file(mnt_tab *tb)
{
	FILE *f = NULL;
	char tmpname[PATH_MAX];
	const char *filename;
	struct stat st;
	int fd;

	assert(tb);
	if (!tb)
		goto error;

	filename = mnt_tab_get_name(tb);
	if (!filename)
		goto error;

	if (snprintf(tmpname, sizeof(tmpname), "%s.tmp", filename)
						>= sizeof(tmpname))
		goto error;

	f = fopen(tmpname, "w");
	if (!f)
		goto error;

	if (mnt_tab_fprintf(tb, f, MNT_MFILE_PRINTFMT) != 0)
		goto error;

	fd = fileno(f);

	if (fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH) < 0)
		goto error;

	/* Copy uid/gid from the present file before renaming. */
	if (stat(filename, &st) == 0) {
		if (fchown(fd, st.st_uid, st.st_gid) < 0)
			goto error;
	}

	fclose(f);
	f = NULL;

	if (rename(tmpname, filename) < 0)
		goto error;

	return 0;
error:
	if (f)
		fclose(f);
	return -1;
}

#ifdef TEST_PROGRAM
int test_strerr(struct mtest *ts, int argc, char *argv[])
{
	char buf[BUFSIZ];
	mnt_tab *tb;
	int i;

	tb = mnt_new_tab("-test-");
	if (!tb)
		goto err;

	for (i = 0; i < 10; i++) {
		mnt_fs *fs = mnt_new_fs();
		if (!fs)
			goto err;
		if (i % 2)
			fs->flags |= MNT_FS_ERROR;	/* mark entry as broken */
		fs->lineno = i+1;
		mnt_tab_add_fs(tb, fs);
	}

	printf("\tadded %d valid lines\n", mnt_tab_get_nents(tb));
	printf("\tadded %d broken lines\n", mnt_tab_get_nerrs(tb));

	if (!mnt_tab_get_nerrs(tb))		/* report broken entries */
		goto err;
	mnt_tab_strerror(tb, buf, sizeof(buf));
	printf("\t%s\n", buf);

	mnt_free_tab(tb);
	return 0;
err:
	return -1;
}

mnt_tab *create_tab(const char *file)
{
	mnt_tab *tb;

	if (!file)
		return NULL;
	tb = mnt_new_tab(file);
	if (!tb)
		goto err;
	if (mnt_tab_parse_file(tb) != 0)
		goto err;
	if (mnt_tab_get_nerrs(tb)) {
		char buf[BUFSIZ];
		mnt_tab_strerror(tb, buf, sizeof(buf));
		fprintf(stderr, "%s\n", buf);
		goto err;
	}
	return tb;
err:
	mnt_free_tab(tb);
	return NULL;
}

int test_parse(struct mtest *ts, int argc, char *argv[])
{
	mnt_tab *tb;

	tb = create_tab(argv[1]);
	if (!tb)
		return -1;

	mnt_tab_fprintf(tb, stdout, MNT_MFILE_PRINTFMT);
	mnt_free_tab(tb);
	return 0;
}

int test_find(struct mtest *ts, int argc, char *argv[], int dr)
{
	mnt_tab *tb;
	mnt_fs *fs = NULL;
	mnt_cache *mpc;
	const char *file, *find, *what;

	if (argc != 4) {
		fprintf(stderr, "try --help\n");
		goto err;
	}

	file = argv[1], find = argv[2], what = argv[3];

	tb = create_tab(file);
	if (!tb)
		goto err;

	/* create a cache for canonicalized paths */
	mpc = mnt_new_cache();
	if (!mpc)
		goto err;
	mnt_tab_set_cache(tb, mpc);

	if (strcasecmp(find, "source") == 0)
		fs = mnt_tab_find_source(tb, what, dr);
	else if (strcasecmp(find, "target") == 0)
		fs = mnt_tab_find_target(tb, what, dr);

	if (!fs)
		fprintf(stderr, "%s: not found %s '%s'\n", file, find, what);
	else {
		const char *s = mnt_fs_get_srcpath(fs);
		if (s)
			printf("%s", s);
		else {
			const char *tag, *val;
			mnt_fs_get_tag(fs, &tag, &val);
			printf("%s=%s", tag, val);
		}
		printf("|%s|%s\n", mnt_fs_get_target(fs),
				mnt_fs_get_optstr(fs));
	}
	mnt_free_tab(tb);
	mnt_free_cache(mpc);
	return 0;
err:
	return -1;
}

int test_find_bw(struct mtest *ts, int argc, char *argv[])
{
	return test_find(ts, argc, argv, MNT_ITER_BACKWARD);
}

int test_find_fw(struct mtest *ts, int argc, char *argv[])
{
	return test_find(ts, argc, argv, MNT_ITER_FORWARD);
}

int main(int argc, char *argv[])
{
	struct mtest tss[] = {
	{ "--strerror", test_strerr,       "        test tab error reporting" },
	{ "--parse",    test_parse,        "<file>  parse and print tab" },
	{ "--find-forward",  test_find_fw, "<file> <source|target> <string>" },
	{ "--find-backward", test_find_bw, "<file> <source|target> <string>" },
	{ NULL }
	};

	return mnt_run_test(tss, argc, argv);
}

#endif /* TEST_PROGRAM */

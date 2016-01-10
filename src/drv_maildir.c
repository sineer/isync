/*
 * mbsync - mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2006,2010-2013 Oswald Buddenhagen <ossi@users.sf.net>
 * Copyright (C) 2004 Theodore Y. Ts'o <tytso@mit.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, mbsync may be linked with the OpenSSL library,
 * despite that library's more restrictive license.
 */

#include "driver.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <utime.h>

#if !defined(_POSIX_SYNCHRONIZED_IO) || _POSIX_SYNCHRONIZED_IO <= 0
# define fdatasync fsync
#endif

#ifdef USE_DB
#include <db.h>
#endif /* USE_DB */

#define SUB_UNSET      0
#define SUB_VERBATIM   1
#define SUB_MAILDIRPP  2
#define SUB_LEGACY     3

typedef struct maildir_store_conf {
	store_conf_t gen;
	char *inbox;
#ifdef USE_DB
	int alt_map;
#endif /* USE_DB */
	char info_delimiter;
	char sub_style;
	char failed;
	char *info_prefix, *info_stop; /* precalculated from info_delimiter */
} maildir_store_conf_t;

typedef struct maildir_message {
	message_t gen;
	char *base;
} maildir_message_t;

typedef struct maildir_store {
	store_t gen;
	int uvfd, uvok, nuid, is_inbox, fresh[3];
	int minuid, maxuid, newuid, nexcs, *excs;
	char *trash;
#ifdef USE_DB
	DB *db;
	char *usedb;
#endif /* USE_DB */
	wakeup_t lcktmr;
} maildir_store_t;

#ifdef USE_DB
static DBT key, value; /* no need to be reentrant, and this saves lots of memset()s */
#endif /* USE_DB */
static struct flock lck;

static int MaildirCount;

static void ATTR_PRINTFLIKE(1, 2)
debug( const char *msg, ... )
{
	va_list va;

	va_start( va, msg );
	vdebug( DEBUG_SYNC, msg, va );
	va_end( va );
}

static const char Flags[] = { 'D', 'F', 'R', 'S', 'T' };

static uchar
maildir_parse_flags( const char *info_prefix, const char *base )
{
	const char *s;
	uint i;
	uchar flags;

	flags = 0;
	if ((s = strstr( base, info_prefix )))
		for (s += 3, i = 0; i < as(Flags); i++)
			if (strchr( s, Flags[i] ))
				flags |= (1 << i);
	return flags;
}

static char *
maildir_join_path( maildir_store_conf_t *conf, const char *prefix, const char *box )
{
	char *out, *p;
	int pl, bl, n, sub = 0;
	char c;

	pl = strlen( prefix );
	for (bl = 0, n = 0; (c = box[bl]); bl++)
		if (c == '/') {
			if (conf->sub_style == SUB_UNSET) {
				error( "Maildir error: accessing subfolder '%s', but store '%s' does not specify SubFolders style\n",
				       box, conf->gen.name );
				return 0;
			}
			n++;
		} else if (c == '.' && conf->sub_style == SUB_MAILDIRPP) {
			error( "Maildir error: store '%s', folder '%s': SubFolders style Maildir++ does not support dots in mailbox names\n",
			       conf->gen.name, box );
			return 0;
		}
	out = nfmalloc( pl + bl + n + 1 );
	memcpy( out, prefix, pl );
	p = out + pl;
	while ((c = *box++)) {
		if (c == '/') {
			switch (conf->sub_style) {
			case SUB_VERBATIM:
				*p++ = c;
				break;
			case SUB_MAILDIRPP:
				if (!sub) {
					sub = 1;
					*p++ = c;
				}
				*p++ = '.';
				break;
			case SUB_LEGACY:
				*p++ = c;
				*p++ = '.';
				break;
			}
		} else {
			*p++ = c;
		}
	}
	*p = 0;
	return out;
}

static int
maildir_ensure_path( maildir_store_conf_t *conf )
{
	if (!conf->gen.path) {
		error( "Maildir error: store '%s' has no Path\n", conf->gen.name );
		conf->failed = FAIL_FINAL;
		return -1;
	}
	return 0;
}

static int
maildir_validate_path( maildir_store_conf_t *conf )
{
	struct stat st;

	if (stat( conf->gen.path, &st ) || !S_ISDIR(st.st_mode)) {
		error( "Maildir error: cannot open store '%s'\n", conf->gen.path );
		conf->failed = FAIL_FINAL;
		return -1;
	}
	return 0;
}

static void lcktmr_timeout( void *aux );

static store_t *
maildir_alloc_store( store_conf_t *gconf, const char *label ATTR_UNUSED )
{
	maildir_store_t *ctx;

	ctx = nfcalloc( sizeof(*ctx) );
	ctx->gen.conf = gconf;
	ctx->uvfd = -1;
	init_wakeup( &ctx->lcktmr, lcktmr_timeout, ctx );
	return &ctx->gen;
}

static void
maildir_connect_store( store_t *gctx,
                       void (*cb)( int sts, void *aux ), void *aux )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	maildir_store_conf_t *conf = (maildir_store_conf_t *)ctx->gen.conf;

	if (conf->gen.path && maildir_validate_path( conf ) < 0) {
		cb( DRV_STORE_BAD, aux );
		return;
	}
	if (conf->gen.trash) {
		if (maildir_ensure_path( conf ) < 0) {
			cb( DRV_STORE_BAD, aux );
			return;
		}
		if (!(ctx->trash = maildir_join_path( conf, conf->gen.path, conf->gen.trash ))) {
			cb( DRV_STORE_BAD, aux );
			return;
		}
	}
	cb( DRV_OK, aux );
}

static void
free_maildir_messages( message_t *msg )
{
	message_t *tmsg;

	for (; (tmsg = msg); msg = tmsg) {
		tmsg = msg->next;
		free( ((maildir_message_t *)msg)->base );
		free( msg );
	}
}

static void
maildir_cleanup( store_t *gctx )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;

	free_maildir_messages( gctx->msgs );
#ifdef USE_DB
	if (ctx->db)
		ctx->db->close( ctx->db, 0 );
	free( ctx->usedb );
#endif /* USE_DB */
	free( gctx->path );
	free( ctx->excs );
	if (ctx->uvfd >= 0)
		close( ctx->uvfd );
	conf_wakeup( &ctx->lcktmr, -1 );
}

static void
maildir_free_store( store_t *gctx )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;

	maildir_cleanup( gctx );
	wipe_wakeup( &ctx->lcktmr );
	free( ctx->trash );
	free_string_list( gctx->boxes );
	free( gctx );
}

static void
maildir_cleanup_drv( void )
{
}

static void
maildir_invoke_bad_callback( store_t *ctx )
{
	ctx->bad_callback( ctx->bad_callback_aux );
}

static int maildir_list_inbox( store_t *gctx, int flags, const char *basePath );
static int maildir_list_path( store_t *gctx, int flags, const char *inbox );

static int
maildir_list_recurse( store_t *gctx, int isBox, int flags,
                      const char *inbox, int inboxLen, const char *basePath, int basePathLen,
                      char *path, int pathLen, char *name, int nameLen )
{
	DIR *dir;
	int style = ((maildir_store_conf_t *)gctx->conf)->sub_style;
	int pl, nl, i;
	struct dirent *de;
	struct stat st;

	if (!(dir = opendir( path ))) {
		if (isBox && (errno == ENOENT || errno == ENOTDIR))
			return 0;
		sys_error( "Maildir error: cannot list %s", path );
		return -1;
	}
	if (isBox > 1 && style == SUB_UNSET) {
		error( "Maildir error: found subfolder '%.*s', but store '%s' does not specify SubFolders style\n",
		       nameLen - 1, name, gctx->conf->name );
		closedir( dir );
		return -1;
	}
	while ((de = readdir( dir ))) {
		const char *ent = de->d_name;
		if (ent[0] == '.' && (!ent[1] || (ent[1] == '.' && !ent[2])))
			continue;
		pl = nfsnprintf( path + pathLen, _POSIX_PATH_MAX - pathLen, "%s", ent );
		if (pl == 3 && (!memcmp( ent, "cur", 3 ) || !memcmp( ent, "new", 3 ) || !memcmp( ent, "tmp", 3 )))
			continue;
		pl += pathLen;
		if (inbox && equals( path, pl, inbox, inboxLen )) {
			/* Inbox nested into Path. List now if it won't be listed separately anyway. */
			if (!(flags & LIST_INBOX) && maildir_list_inbox( gctx, flags, 0 ) < 0) {
				closedir( dir );
				return -1;
			}
		} else if (basePath && equals( path, pl, basePath, basePathLen )) {
			/* Path nested into Inbox. List now if it won't be listed separately anyway. */
			if (!(flags & (LIST_PATH | LIST_PATH_MAYBE)) && maildir_list_path( gctx, flags, 0 ) < 0) {
				closedir( dir );
				return -1;
			}
		} else {
			if (style == SUB_MAILDIRPP || style == SUB_LEGACY) {
				if (*ent == '.') {
					if (!isBox)
						continue;
					ent++;
				} else {
					if (isBox)
						continue;
				}
			}
			nl = nameLen + nfsnprintf( name + nameLen, _POSIX_PATH_MAX - nameLen, "%s", ent );
			if (style == SUB_MAILDIRPP && isBox) {
				for (i = nameLen; i < nl; i++) {
					if (name[i] == '.')
						name[i] = '/';
				}
			}
			if (!nameLen && equals( name, nl, "INBOX", 5 ) && (!name[5] || name[5] == '/')) {
				path[pathLen] = 0;
				warn( "Maildir warning: ignoring INBOX in %s\n", path );
				continue;
			}
			path[pl++] = '/';
			nfsnprintf( path + pl, _POSIX_PATH_MAX - pl, "cur" );
			if (!stat( path, &st ) && S_ISDIR(st.st_mode))
				add_string_list( &gctx->boxes, name );
			if (style == SUB_MAILDIRPP && isBox) {
				/* Maildir++ subfolder - don't recurse further. */
				continue;
			}
			path[pl] = 0;
			name[nl++] = '/';
			if (maildir_list_recurse( gctx, isBox + 1, flags, inbox, inboxLen, basePath, basePathLen, path, pl, name, nl ) < 0) {
				closedir( dir );
				return -1;
			}
		}
	}
	closedir (dir);
	return 0;
}

static int
maildir_list_inbox( store_t *gctx, int flags, const char *basePath )
{
	char path[_POSIX_PATH_MAX], name[_POSIX_PATH_MAX];

	add_string_list( &gctx->boxes, "INBOX" );
	return maildir_list_recurse(
	        gctx, 1, flags, 0, 0, basePath, basePath ? strlen( basePath ) - 1 : 0,
	        path, nfsnprintf( path, _POSIX_PATH_MAX, "%s/", ((maildir_store_conf_t *)gctx->conf)->inbox ),
	        name, nfsnprintf( name, _POSIX_PATH_MAX, "INBOX/" ) );
}

static int
maildir_list_path( store_t *gctx, int flags, const char *inbox )
{
	char path[_POSIX_PATH_MAX], name[_POSIX_PATH_MAX];

	if (maildir_ensure_path( (maildir_store_conf_t *)gctx->conf ) < 0)
		return -1;
	return maildir_list_recurse(
	        gctx, 0, flags, inbox, inbox ? strlen( inbox ) : 0, 0, 0,
	        path, nfsnprintf( path, _POSIX_PATH_MAX, "%s", gctx->conf->path ),
	        name, 0 );
}

static void
maildir_list_store( store_t *gctx, int flags,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	if ((((flags & LIST_PATH) || ((flags & LIST_PATH_MAYBE) && gctx->conf->path))
	     && maildir_list_path( gctx, flags, ((maildir_store_conf_t *)gctx->conf)->inbox ) < 0) ||
	    ((flags & LIST_INBOX) && maildir_list_inbox( gctx, flags, gctx->conf->path ) < 0)) {
		maildir_invoke_bad_callback( gctx );
		cb( DRV_CANCELED, aux );
	} else {
		cb( DRV_OK, aux );
	}
}

static const char *subdirs[] = { "cur", "new", "tmp" };

typedef struct {
	char *base;
	int size;
	uint uid:31, recent:1;
	char tuid[TUIDL];
} msg_t;

typedef struct {
	msg_t *ents;
	int nents, nalloc;
} msglist_t;

static void
maildir_free_scan( msglist_t *msglist )
{
	int i;

	if (msglist->ents) {
		for (i = 0; i < msglist->nents; i++)
			free( msglist->ents[i].base );
		free( msglist->ents );
	}
}

#define _24_HOURS (3600 * 24)

static int
maildir_clear_tmp( char *buf, int bufsz, int bl )
{
	DIR *dirp;
	struct dirent *entry;
	time_t now;
	struct stat st;

	memcpy( buf + bl, "tmp/", 5 );
	bl += 4;
	if (!(dirp = opendir( buf ))) {
		sys_error( "Maildir error: cannot list %s", buf );
		return DRV_BOX_BAD;
	}
	time( &now );
	while ((entry = readdir( dirp ))) {
		nfsnprintf( buf + bl, bufsz - bl, "%s", entry->d_name );
		if (stat( buf, &st )) {
			if (errno != ENOENT)
				sys_error( "Maildir error: cannot access %s", buf );
		} else if (S_ISREG(st.st_mode) && now - st.st_ctime >= _24_HOURS) {
			/* This should happen infrequently enough that it won't be
			 * bothersome to the user to display when it occurs.
			 */
			notice( "Maildir notice: removing stale file %s\n", buf );
			if (unlink( buf ) && errno != ENOENT)
				sys_error( "Maildir error: cannot remove %s", buf );
		}
	}
	closedir( dirp );
	return DRV_OK;
}

static int
make_box_dir( char *buf, int bl )
{
	char *p;

	if (!mkdir( buf, 0700 ) || errno == EEXIST)
		return 0;
	p = memrchr( buf, '/', bl - 1 );
	*p = 0;
	if (make_box_dir( buf, (int)(p - buf) ))
		return -1;
	*p = '/';
	return mkdir( buf, 0700 );
}

static int
maildir_validate( const char *box, int create, maildir_store_t *ctx )
{
	int i, bl, ret;
	struct stat st;
	char buf[_POSIX_PATH_MAX];

	bl = nfsnprintf( buf, sizeof(buf) - 4, "%s/", box );
	if (stat( buf, &st )) {
		if (errno != ENOENT) {
			sys_error( "Maildir error: cannot access mailbox '%s'", box );
			return DRV_BOX_BAD;
		}
		if (!create)
			return DRV_BOX_BAD;
		if (make_box_dir( buf, bl )) {
			sys_error( "Maildir error: cannot create mailbox '%s'", box );
			((maildir_store_conf_t *)ctx->gen.conf)->failed = FAIL_FINAL;
			maildir_invoke_bad_callback( &ctx->gen );
			return DRV_CANCELED;
		}
	} else if (!S_ISDIR(st.st_mode)) {
	  notdir:
		error( "Maildir error: '%s' is no valid mailbox\n", box );
		return DRV_BOX_BAD;
	}
	for (i = 0; i < 3; i++) {
		memcpy( buf + bl, subdirs[i], 4 );
		if (stat( buf, &st )) {
			/* We always create new/ and tmp/ if they are missing. cur/ is the presence indicator. */
			if (!i && !create)
				return DRV_BOX_BAD;
			if (mkdir( buf, 0700 )) {
				sys_error( "Maildir error: cannot create directory %s", buf );
				return DRV_BOX_BAD;
			}
			ctx->fresh[i] = 1;
		} else if (!S_ISDIR(st.st_mode)) {
			goto notdir;
		} else {
			if (i == 2) {
				if ((ret = maildir_clear_tmp( buf, sizeof(buf), bl )) != DRV_OK)
					return ret;
			}
		}
	}
	return DRV_OK;
}

#ifdef USE_DB
static void
make_key( const char *info_stop, DBT *tkey, char *name )
{
	char *u = strpbrk( name, info_stop );
	tkey->data = name;
	tkey->size = u ? (size_t)(u - name) : strlen( name );
}
#endif /* USE_DB */

static int
maildir_store_uidval( maildir_store_t *ctx )
{
	int n;
#ifdef USE_DB
	int ret, uv[2];
#endif
	char buf[128];

#ifdef USE_DB
	if (ctx->db) {
		key.data = (void *)"UIDVALIDITY";
		key.size = 11;
		uv[0] = ctx->gen.uidvalidity;
		uv[1] = ctx->nuid;
		value.data = uv;
		value.size = sizeof(uv);
		if ((ret = ctx->db->put( ctx->db, 0, &key, &value, 0 ))) {
			ctx->db->err( ctx->db, ret, "Maildir error: db->put()" );
			return DRV_BOX_BAD;
		}
		if ((ret = ctx->db->sync( ctx->db, 0 ))) {
			ctx->db->err( ctx->db, ret, "Maildir error: db->sync()" );
			return DRV_BOX_BAD;
		}
	} else
#endif /* USE_DB */
	{
		n = sprintf( buf, "%d\n%d\n", ctx->gen.uidvalidity, ctx->nuid );
		lseek( ctx->uvfd, 0, SEEK_SET );
		if (write( ctx->uvfd, buf, n ) != n || ftruncate( ctx->uvfd, n ) || (UseFSync && fdatasync( ctx->uvfd ))) {
			error( "Maildir error: cannot write UIDVALIDITY.\n" );
			return DRV_BOX_BAD;
		}
	}
	conf_wakeup( &ctx->lcktmr, 2 );
	return DRV_OK;
}

static int
maildir_init_uidval( maildir_store_t *ctx )
{
	ctx->gen.uidvalidity = time( 0 );
	ctx->nuid = 0;
	ctx->uvok = 0;
#ifdef USE_DB
	if (ctx->db) {
		u_int32_t count;
		ctx->db->truncate( ctx->db, 0, &count, 0 );
	}
#endif /* USE_DB */
	return maildir_store_uidval( ctx );
}

static int
maildir_init_uidval_new( maildir_store_t *ctx )
{
	notice( "Maildir notice: no UIDVALIDITY, creating new.\n" );
	return maildir_init_uidval( ctx );
}

static int
maildir_uidval_lock( maildir_store_t *ctx )
{
	int n;
#ifdef USE_DB
	int ret;
	struct stat st;
#endif
	char buf[128];

	if (pending_wakeup( &ctx->lcktmr )) {
		/* The unlock timer is active, so we are obviously already locked. */
		return DRV_OK;
	}
	/* This (theoretically) works over NFS. Let's hope nobody else did
	   the same in the opposite order, as we'd deadlock then. */
#if SEEK_SET != 0
	lck.l_whence = SEEK_SET;
#endif
	lck.l_type = F_WRLCK;
	if (fcntl( ctx->uvfd, F_SETLKW, &lck )) {
		error( "Maildir error: cannot fcntl lock UIDVALIDITY.\n" );
		return DRV_BOX_BAD;
	}

#ifdef USE_DB
	if (ctx->usedb) {
		if (fstat( ctx->uvfd, &st )) {
			sys_error( "Maildir error: cannot fstat UID database" );
			return DRV_BOX_BAD;
		}
		if (db_create( &ctx->db, 0, 0 )) {
			fputs( "Maildir error: db_create() failed\n", stderr );
			return DRV_BOX_BAD;
		}
		if ((ret = (ctx->db->open)( ctx->db, 0, ctx->usedb, 0, DB_HASH,
		                            st.st_size ? 0 : DB_CREATE | DB_TRUNCATE, 0 ))) {
			ctx->db->err( ctx->db, ret, "Maildir error: db->open(%s)", ctx->usedb );
			return DRV_BOX_BAD;
		}
		key.data = (void *)"UIDVALIDITY";
		key.size = 11;
		if ((ret = ctx->db->get( ctx->db, 0, &key, &value, 0 ))) {
			if (ret != DB_NOTFOUND) {
				ctx->db->err( ctx->db, ret, "Maildir error: db->get()" );
				return DRV_BOX_BAD;
			}
			return maildir_init_uidval_new( ctx );
		}
		ctx->gen.uidvalidity = ((int *)value.data)[0];
		ctx->nuid = ((int *)value.data)[1];
	} else
#endif
	{
		lseek( ctx->uvfd, 0, SEEK_SET );
		if ((n = read( ctx->uvfd, buf, sizeof(buf) - 1 )) <= 0 ||
			(buf[n] = 0, sscanf( buf, "%d\n%d", &ctx->gen.uidvalidity, &ctx->nuid ) != 2)) {
#if 1
			/* In a generic driver, resetting the UID validity would be the right thing.
			 * But this would mess up the sync state completely. So better bail out and
			 * give the user a chance to fix the mailbox. */
			if (n) {
				error( "Maildir error: cannot read UIDVALIDITY.\n" );
				return DRV_BOX_BAD;
			}
#endif
			return maildir_init_uidval_new( ctx );
		}
	}
	ctx->uvok = 1;
	conf_wakeup( &ctx->lcktmr, 2 );
	return DRV_OK;
}

static void
maildir_uidval_unlock( maildir_store_t *ctx )
{
#ifdef USE_DB
	if (ctx->db) {
		ctx->db->close( ctx->db, 0 );
		ctx->db = 0;
	}
#endif /* USE_DB */
	lck.l_type = F_UNLCK;
	fcntl( ctx->uvfd, F_SETLK, &lck );
}

static void
lcktmr_timeout( void *aux )
{
	maildir_uidval_unlock( (maildir_store_t *)aux );
}

static int
maildir_obtain_uid( maildir_store_t *ctx, int *uid )
{
	int ret;

	if ((ret = maildir_uidval_lock( ctx )) != DRV_OK)
		return ret;
	*uid = ++ctx->nuid;
	return maildir_store_uidval( ctx );
}

#ifdef USE_DB
static int
maildir_set_uid( maildir_store_t *ctx, const char *name, int *uid )
{
	int ret;

	if ((ret = maildir_uidval_lock( ctx )) != DRV_OK)
		return ret;
	*uid = ++ctx->nuid;

	make_key( ((maildir_store_conf_t *)ctx->gen.conf)->info_stop, &key, (char *)name );
	value.data = uid;
	value.size = sizeof(*uid);
	if ((ret = ctx->db->put( ctx->db, 0, &key, &value, 0 ))) {
		ctx->db->err( ctx->db, ret, "Maildir error: db->put()" );
		return DRV_BOX_BAD;
	}
	return maildir_store_uidval( ctx );
}
#endif

static int
maildir_compare( const void *l, const void *r )
{
	msg_t *lm = (msg_t *)l, *rm = (msg_t *)r;
	char *ldot, *rdot, *ldot2, *rdot2, *lseq, *rseq;
	int ret, llen, rlen;

	if ((ret = lm->uid - rm->uid))
		return ret;

	/* No UID, so sort by arrival date. We should not do this, but we rely
	   on the suggested unique file name scheme - we have no choice. */
	/* The first field are always the seconds. Alphabetical sort should be
	   faster than numeric. */
	if (!(ldot = strchr( lm->base, '.' )) || !(rdot = strchr( rm->base, '.' )))
		goto stronly; /* Should never happen ... */
	llen = ldot - lm->base, rlen = rdot - rm->base;
	/* The shorter number is smaller. Really. This won't trigger with any
	   mail created after Sep 9 2001 anyway. */
	if ((ret = llen - rlen))
		return ret;
	if ((ret = memcmp( lm->base, rm->base, llen )))
		return ret;

	ldot++, rdot++;

	if ((llen = strtol( ldot, &ldot2, 10 ))) {
		if (!(rlen = strtol( rdot, &rdot2, 10 )))
			goto stronly; /* Comparing apples to oranges ... */
		/* Classical PID specs */
		if ((ret = llen - rlen)) {
		  retpid:
			/* Handle PID wraparound. This works only on systems
			   where PIDs are not reused too fast */
			if (ret > 20000 || ret < -20000)
				ret = -ret;
			return ret;
		}
		return (*ldot2 != '_' ? 0 : atoi( ldot2 + 1 )) -
		       (*rdot2 != '_' ? 0 : atoi( rdot2 + 1 ));
	}

	if (!(ldot2 = strchr( ldot, '.' )) || !(rdot2 = strchr( rdot, '.' )))
		goto stronly; /* Should never happen ... */
	llen = ldot2 - ldot, rlen = rdot2 - rdot;

	if (((lseq = memchr( ldot, '#', llen )) && (rseq = memchr( rdot, '#', rlen ))) ||
	    ((lseq = memchr( ldot, 'M', llen )) && (rseq = memchr( rdot, 'M', rlen ))))
		return atoi( lseq + 1 ) - atoi( rseq + 1 );

	if ((lseq = memchr( ldot, 'P', llen )) && (rseq = memchr( rdot, 'P', rlen ))) {
		if ((ret = atoi( lseq + 1 ) - atoi( rseq + 1 )))
			goto retpid;
		if ((lseq = memchr( ldot, 'Q', llen )) && (rseq = memchr( rdot, 'Q', rlen )))
			return atoi( lseq + 1 ) - atoi( rseq + 1 );
	}

  stronly:
	/* Fall-back, so the sort order is defined at all */
	return strcmp( lm->base, rm->base );
}

static int
maildir_scan( maildir_store_t *ctx, msglist_t *msglist )
{
	maildir_store_conf_t *conf = (maildir_store_conf_t *)ctx->gen.conf;
	DIR *d;
	FILE *f;
	struct dirent *e;
	const char *u, *ru;
#ifdef USE_DB
	DB *tdb;
	DBC *dbc;
#endif /* USE_DB */
	msg_t *entry;
	int i, j, uid, bl, fnl, ret;
	time_t now, stamps[2];
	struct stat st;
	char buf[_POSIX_PATH_MAX], nbuf[_POSIX_PATH_MAX];

  again:
	msglist->ents = 0;
	msglist->nents = msglist->nalloc = 0;
	ctx->gen.count = ctx->gen.recent = 0;
	if (ctx->uvok || ctx->maxuid == INT_MAX) {
#ifdef USE_DB
		if (ctx->usedb) {
			if (db_create( &tdb, 0, 0 )) {
				fputs( "Maildir error: db_create() failed\n", stderr );
				return DRV_BOX_BAD;
			}
			if ((tdb->open)( tdb, 0, 0, 0, DB_HASH, DB_CREATE, 0 )) {
				fputs( "Maildir error: tdb->open() failed\n", stderr );
			  bork:
				tdb->close( tdb, 0 );
				return DRV_BOX_BAD;
			}
		}
#endif /* USE_DB */
		bl = nfsnprintf( buf, sizeof(buf) - 4, "%s/", ctx->gen.path );
	  restat:
		now = time( 0 );
		for (i = 0; i < 2; i++) {
			memcpy( buf + bl, subdirs[i], 4 );
			if (stat( buf, &st )) {
				sys_error( "Maildir error: cannot stat %s", buf );
				goto dfail;
			}
			if (st.st_mtime == now && !(DFlags & ZERODELAY) && !ctx->fresh[i]) {
				/* If the modification happened during this second, we wouldn't be able to
				 * tell if there were further modifications during this second. So wait.
				 * This has the nice side effect that we wait for "batches" of changes to
				 * complete. On the downside, it can potentially block indefinitely. */
				notice( "Maildir notice: sleeping due to recent directory modification.\n" );
				sleep( 1 ); /* FIXME: should make this async */
				goto restat;
			}
			stamps[i] = st.st_mtime;
		}
		for (i = 0; i < 2; i++) {
			memcpy( buf + bl, subdirs[i], 4 );
			if (!(d = opendir( buf ))) {
				sys_error( "Maildir error: cannot list %s", buf );
			  rfail:
				maildir_free_scan( msglist );
			  dfail:
#ifdef USE_DB
				if (ctx->usedb)
					tdb->close( tdb, 0 );
#endif /* USE_DB */
				return DRV_BOX_BAD;
			}
			while ((e = readdir( d ))) {
				if (*e->d_name == '.')
					continue;
				ctx->gen.count++;
				ctx->gen.recent += i;
#ifdef USE_DB
				if (ctx->usedb) {
					if (maildir_uidval_lock( ctx ) != DRV_OK)
						goto mbork;
					make_key( conf->info_stop, &key, e->d_name );
					if ((ret = ctx->db->get( ctx->db, 0, &key, &value, 0 ))) {
						if (ret != DB_NOTFOUND) {
							ctx->db->err( ctx->db, ret, "Maildir error: db->get()" );
						  mbork:
							maildir_free_scan( msglist );
							closedir( d );
							goto bork;
						}
						uid = INT_MAX;
					} else {
						value.size = 0;
						if ((ret = tdb->put( tdb, 0, &key, &value, 0 ))) {
							tdb->err( tdb, ret, "Maildir error: tdb->put()" );
							goto mbork;
						}
						uid = *(int *)value.data;
					}
				} else
#endif /* USE_DB */
				{
					uid = (ctx->uvok && (u = strstr( e->d_name, ",U=" ))) ? atoi( u + 3 ) : 0;
					if (!uid)
						uid = INT_MAX;
				}
				if (uid <= ctx->maxuid) {
					if (uid < ctx->minuid) {
						for (j = 0; j < ctx->nexcs; j++)
							if (ctx->excs[j] == uid)
								goto oke;
						continue;
					  oke: ;
					}
					if (msglist->nalloc == msglist->nents) {
						msglist->nalloc = msglist->nalloc * 2 + 100;
						msglist->ents = nfrealloc( msglist->ents, msglist->nalloc * sizeof(msg_t) );
					}
					entry = &msglist->ents[msglist->nents++];
					entry->base = nfstrdup( e->d_name );
					entry->uid = uid;
					entry->recent = i;
					entry->size = 0;
					entry->tuid[0] = 0;
				}
			}
			closedir( d );
		}
		for (i = 0; i < 2; i++) {
			memcpy( buf + bl, subdirs[i], 4 );
			if (stat( buf, &st )) {
				sys_error( "Maildir error: cannot re-stat %s", buf );
				goto rfail;
			}
			if (st.st_mtime != stamps[i]) {
				/* Somebody messed with the mailbox since we started listing it. */
#ifdef USE_DB
				if (ctx->usedb)
					tdb->close( tdb, 0 );
#endif /* USE_DB */
				maildir_free_scan( msglist );
				goto again;
			}
		}
#ifdef USE_DB
		if (ctx->usedb) {
			if (maildir_uidval_lock( ctx ) != DRV_OK)
				;
			else if ((ret = ctx->db->cursor( ctx->db, 0, &dbc, 0 )))
				ctx->db->err( ctx->db, ret, "Maildir error: db->cursor()" );
			else {
				for (;;) {
					if ((ret = dbc->c_get( dbc, &key, &value, DB_NEXT ))) {
						if (ret != DB_NOTFOUND)
							ctx->db->err( ctx->db, ret, "Maildir error: db->c_get()" );
						break;
					}
					if (!equals( key.data, key.size, "UIDVALIDITY", 11 ) &&
					    (ret = tdb->get( tdb, 0, &key, &value, 0 ))) {
						if (ret != DB_NOTFOUND) {
							tdb->err( tdb, ret, "Maildir error: tdb->get()" );
							break;
						}
						if ((ret = dbc->c_del( dbc, 0 ))) {
							ctx->db->err( ctx->db, ret, "Maildir error: db->c_del()" );
							break;
						}
					}
				}
				dbc->c_close( dbc );
			}
			tdb->close( tdb, 0 );
		}
#endif /* USE_DB */
		qsort( msglist->ents, msglist->nents, sizeof(msg_t), maildir_compare );
		for (uid = i = 0; i < msglist->nents; i++) {
			entry = &msglist->ents[i];
			if (entry->uid != INT_MAX) {
				if (uid == entry->uid) {
#if 1
					/* See comment in maildir_uidval_lock() why this is fatal. */
					error( "Maildir error: duplicate UID %d.\n", uid );
					maildir_free_scan( msglist );
					return DRV_BOX_BAD;
#else
					notice( "Maildir notice: duplicate UID; changing UIDVALIDITY.\n");
					if ((ret = maildir_init_uid( ctx )) != DRV_OK) {
						maildir_free_scan( msglist );
						return ret;
					}
					maildir_free_scan( msglist );
					goto again;
#endif
				}
				uid = entry->uid;
				if ((ctx->gen.opts & OPEN_SIZE) || ((ctx->gen.opts & OPEN_FIND) && uid >= ctx->newuid))
					nfsnprintf( buf + bl, sizeof(buf) - bl, "%s/%s", subdirs[entry->recent], entry->base );
#ifdef USE_DB
			} else if (ctx->usedb) {
				if ((ret = maildir_set_uid( ctx, entry->base, &uid )) != DRV_OK) {
					maildir_free_scan( msglist );
					return ret;
				}
				entry->uid = uid;
				if ((ctx->gen.opts & OPEN_SIZE) || ((ctx->gen.opts & OPEN_FIND) && uid >= ctx->newuid))
					nfsnprintf( buf + bl, sizeof(buf) - bl, "%s/%s", subdirs[entry->recent], entry->base );
#endif /* USE_DB */
			} else {
				if ((ret = maildir_obtain_uid( ctx, &uid )) != DRV_OK) {
					maildir_free_scan( msglist );
					return ret;
				}
				entry->uid = uid;
				if ((u = strstr( entry->base, ",U=" )))
					for (ru = u + 3; isdigit( (uchar)*ru ); ru++);
				else
					u = ru = strchr( entry->base, conf->info_delimiter );
				fnl = (u ?
					nfsnprintf( buf + bl, sizeof(buf) - bl, "%s/%.*s,U=%d%s", subdirs[entry->recent], (int)(u - entry->base), entry->base, uid, ru ) :
					nfsnprintf( buf + bl, sizeof(buf) - bl, "%s/%s,U=%d", subdirs[entry->recent], entry->base, uid ))
					+ 1 - 4;
				memcpy( nbuf, buf, bl + 4 );
				nfsnprintf( nbuf + bl + 4, sizeof(nbuf) - bl - 4, "%s", entry->base );
				if (rename( nbuf, buf )) {
					if (errno != ENOENT) {
						sys_error( "Maildir error: cannot rename %s to %s", nbuf, buf );
					  fail:
						maildir_free_scan( msglist );
						return DRV_BOX_BAD;
					}
				  retry:
					maildir_free_scan( msglist );
					goto again;
				}
				free( entry->base );
				entry->base = nfmalloc( fnl );
				memcpy( entry->base, buf + bl + 4, fnl );
			}
			if (ctx->gen.opts & OPEN_SIZE) {
				if (stat( buf, &st )) {
					if (errno != ENOENT) {
						sys_error( "Maildir error: cannot stat %s", buf );
						goto fail;
					}
					goto retry;
				}
				entry->size = st.st_size;
			}
			if ((ctx->gen.opts & OPEN_FIND) && uid >= ctx->newuid) {
				if (!(f = fopen( buf, "r" ))) {
					if (errno != ENOENT) {
						sys_error( "Maildir error: cannot open %s", buf );
						goto fail;
					}
					goto retry;
				}
				while (fgets( nbuf, sizeof(nbuf), f )) {
					if (!nbuf[0] || nbuf[0] == '\n')
						break;
					if (starts_with( nbuf, -1, "X-TUID: ", 8 ) && nbuf[8 + TUIDL] == '\n') {
						memcpy( entry->tuid, nbuf + 8, TUIDL );
						break;
					}
				}
				fclose( f );
			}
		}
		ctx->uvok = 1;
	}
	return DRV_OK;
}

static void
maildir_init_msg( maildir_store_t *ctx, maildir_message_t *msg, msg_t *entry )
{
	msg->base = entry->base;
	entry->base = 0; /* prevent deletion */
	msg->gen.size = entry->size;
	msg->gen.srec = 0;
	strncpy( msg->gen.tuid, entry->tuid, TUIDL );
	if (entry->recent)
		msg->gen.status |= M_RECENT;
	if (ctx->gen.opts & OPEN_FLAGS) {
		msg->gen.status |= M_FLAGS;
		msg->gen.flags = maildir_parse_flags( ((maildir_store_conf_t *)ctx->gen.conf)->info_prefix, msg->base );
	} else
		msg->gen.flags = 0;
}

static void
maildir_app_msg( maildir_store_t *ctx, message_t ***msgapp, msg_t *entry )
{
	maildir_message_t *msg = nfmalloc( sizeof(*msg) );
	msg->gen.next = **msgapp;
	**msgapp = &msg->gen;
	*msgapp = &msg->gen.next;
	msg->gen.uid = entry->uid;
	msg->gen.status = 0;
	maildir_init_msg( ctx, msg, entry );
}

static int
maildir_select_box( store_t *gctx, const char *name )
{
	maildir_store_conf_t *conf = (maildir_store_conf_t *)gctx->conf;
	maildir_store_t *ctx = (maildir_store_t *)gctx;

	maildir_cleanup( gctx );
	gctx->msgs = 0;
	ctx->excs = 0;
	ctx->uvfd = -1;
#ifdef USE_DB
	ctx->db = 0;
	ctx->usedb = 0;
#endif /* USE_DB */
	ctx->fresh[0] = ctx->fresh[1] = 0;
	if (starts_with( name, -1, "INBOX", 5 ) && (!name[5] || name[5] == '/')) {
		gctx->path = maildir_join_path( conf, conf->inbox, name + 5 );
		ctx->is_inbox = !name[5];
	} else {
		if (maildir_ensure_path( conf ) < 0) {
			gctx->path = 0;
			return DRV_CANCELED;
		}
		gctx->path = maildir_join_path( conf, conf->gen.path, name );
		ctx->is_inbox = 0;
	}
	return gctx->path ? DRV_OK : DRV_BOX_BAD;
}

static void
maildir_open_box( store_t *gctx,
                  void (*cb)( int sts, void *aux ), void *aux )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	int ret;
	char uvpath[_POSIX_PATH_MAX];

	if ((ret = maildir_validate( gctx->path, ctx->is_inbox, ctx )) != DRV_OK)
		goto bail;

	nfsnprintf( uvpath, sizeof(uvpath), "%s/.uidvalidity", gctx->path );
#ifndef USE_DB
	if ((ctx->uvfd = open( uvpath, O_RDWR|O_CREAT, 0600 )) < 0) {
		sys_error( "Maildir error: cannot write %s", uvpath );
		cb( DRV_BOX_BAD, aux );
		return;
	}
#else
	ctx->usedb = 0;
	if ((ctx->uvfd = open( uvpath, O_RDWR, 0600 )) < 0) {
		nfsnprintf( uvpath, sizeof(uvpath), "%s/.isyncuidmap.db", gctx->path );
		if ((ctx->uvfd = open( uvpath, O_RDWR, 0600 )) < 0) {
			if (((maildir_store_conf_t *)gctx->conf)->alt_map) {
				if ((ctx->uvfd = open( uvpath, O_RDWR|O_CREAT, 0600 )) >= 0)
					goto dbok;
			} else {
				nfsnprintf( uvpath, sizeof(uvpath), "%s/.uidvalidity", gctx->path );
				if ((ctx->uvfd = open( uvpath, O_RDWR|O_CREAT, 0600 )) >= 0)
					goto fnok;
			}
			sys_error( "Maildir error: cannot write %s", uvpath );
			cb( DRV_BOX_BAD, aux );
			return;
		} else {
		  dbok:
			ctx->usedb = nfstrdup( uvpath );
		}
	}
  fnok:
#endif /* USE_DB */
	ret = maildir_uidval_lock( ctx );

  bail:
	cb( ret, aux );
}

static void
maildir_create_box( store_t *gctx,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	cb( maildir_validate( gctx->path, 1, (maildir_store_t *)gctx ), aux );
}

static int
maildir_confirm_box_empty( store_t *gctx )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	msglist_t msglist;

	ctx->nexcs = ctx->minuid = ctx->maxuid = ctx->newuid = 0;

	if (maildir_scan( ctx, &msglist ) != DRV_OK)
		return DRV_BOX_BAD;
	maildir_free_scan( &msglist );
	return gctx->count ? DRV_BOX_BAD : DRV_OK;
}

static void
maildir_delete_box( store_t *gctx,
                    void (*cb)( int sts, void *aux ), void *aux )
{
	int i, bl, ret = DRV_OK;
	struct stat st;
	char buf[_POSIX_PATH_MAX];

	bl = nfsnprintf( buf, sizeof(buf) - 4, "%s/", gctx->path );
	if (stat( buf, &st )) {
		if (errno != ENOENT) {
			sys_error( "Maildir error: cannot access mailbox '%s'", gctx->path );
			ret = DRV_BOX_BAD;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		error( "Maildir error: '%s' is no valid mailbox\n", gctx->path );
		ret = DRV_BOX_BAD;
	} else if ((ret = maildir_clear_tmp( buf, sizeof(buf), bl )) == DRV_OK) {
		nfsnprintf( buf + bl, sizeof(buf) - bl, ".uidvalidity" );
		if (unlink( buf ) && errno != ENOENT)
			goto badrm;
#ifdef USE_DB
		nfsnprintf( buf + bl, sizeof(buf) - bl, ".isyncuidmap.db" );
		if (unlink( buf ) && errno != ENOENT)
			goto badrm;
#endif
		/* We delete cur/ last, as it is the indicator for a present mailbox.
		 * That way an interrupted operation can be resumed. */
		for (i = 3; --i >= 0; ) {
			memcpy( buf + bl, subdirs[i], 4 );
			if (rmdir( buf ) && errno != ENOENT) {
			  badrm:
				sys_error( "Maildir error: cannot remove '%s'", buf );
				ret = DRV_BOX_BAD;
				break;
			}
		}
	}
	cb( ret, aux );
}

static int
maildir_finish_delete_box( store_t *gctx )
{
	/* Subfolders are not deleted; the deleted folder is only "stripped of its mailboxness".
	 * Consequently, the rmdir may legitimately fail. This behavior follows the IMAP spec. */
	if (rmdir( gctx->path ) && errno != ENOENT && errno != ENOTEMPTY) {
		sys_error( "Maildir warning: cannot remove '%s'", gctx->path );
		return DRV_BOX_BAD;
	}
	return DRV_OK;
}

static void
maildir_prepare_load_box( store_t *gctx, int opts )
{
	if (opts & OPEN_SETFLAGS)
		opts |= OPEN_OLD;
	if (opts & OPEN_EXPUNGE)
		opts |= OPEN_OLD|OPEN_NEW|OPEN_FLAGS;
	gctx->opts = opts;
}

static void
maildir_load_box( store_t *gctx, int minuid, int maxuid, int newuid, int *excs, int nexcs,
                  void (*cb)( int sts, void *aux ), void *aux )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	message_t **msgapp;
	msglist_t msglist;
	int i;

	ctx->minuid = minuid;
	ctx->maxuid = maxuid;
	ctx->newuid = newuid;
	ctx->excs = nfrealloc( excs, nexcs * sizeof(int) );
	ctx->nexcs = nexcs;

	if (maildir_scan( ctx, &msglist ) != DRV_OK) {
		cb( DRV_BOX_BAD, aux );
		return;
	}
	msgapp = &ctx->gen.msgs;
	for (i = 0; i < msglist.nents; i++)
		maildir_app_msg( ctx, &msgapp, msglist.ents + i );
	maildir_free_scan( &msglist );

	cb( DRV_OK, aux );
}

static int
maildir_rescan( maildir_store_t *ctx )
{
	message_t **msgapp;
	maildir_message_t *msg;
	msglist_t msglist;
	int i;

	ctx->fresh[0] = ctx->fresh[1] = 0;
	if (maildir_scan( ctx, &msglist ) != DRV_OK)
		return DRV_BOX_BAD;
	for (msgapp = &ctx->gen.msgs, i = 0;
	     (msg = (maildir_message_t *)*msgapp) || i < msglist.nents; )
	{
		if (!msg) {
#if 0
			debug( "adding new message %d\n", msglist.ents[i].uid );
			maildir_app_msg( ctx, &msgapp, msglist.ents + i );
#else
			debug( "ignoring new message %d\n", msglist.ents[i].uid );
#endif
			i++;
		} else if (i >= msglist.nents) {
			debug( "purging deleted message %d\n", msg->gen.uid );
			msg->gen.status = M_DEAD;
			msgapp = &msg->gen.next;
		} else if (msglist.ents[i].uid < msg->gen.uid) {
			/* this should not happen, actually */
#if 0
			debug( "adding new message %d\n", msglist.ents[i].uid );
			maildir_app_msg( ctx, &msgapp, msglist.ents + i );
#else
			debug( "ignoring new message %d\n", msglist.ents[i].uid );
#endif
			i++;
		} else if (msglist.ents[i].uid > msg->gen.uid) {
			debug( "purging deleted message %d\n", msg->gen.uid );
			msg->gen.status = M_DEAD;
			msgapp = &msg->gen.next;
		} else {
			debug( "updating message %d\n", msg->gen.uid );
			msg->gen.status &= ~(M_FLAGS|M_RECENT);
			free( msg->base );
			maildir_init_msg( ctx, msg, msglist.ents + i );
			i++, msgapp = &msg->gen.next;
		}
	}
	maildir_free_scan( &msglist );
	return DRV_OK;
}

static int
maildir_again( maildir_store_t *ctx, maildir_message_t *msg,
               const char *err, const char *fn, const char *fn2 )
{
	int ret;

	if (errno != ENOENT) {
		sys_error( err, fn, fn2 );
		return DRV_BOX_BAD;
	}
	if ((ret = maildir_rescan( ctx )) != DRV_OK)
		return ret;
	return (msg->gen.status & M_DEAD) ? DRV_MSG_BAD : DRV_OK;
}

static void
maildir_fetch_msg( store_t *gctx, message_t *gmsg, msg_data_t *data,
                   void (*cb)( int sts, void *aux ), void *aux )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	maildir_message_t *msg = (maildir_message_t *)gmsg;
	int fd, ret;
	struct stat st;
	char buf[_POSIX_PATH_MAX];

	for (;;) {
		nfsnprintf( buf, sizeof(buf), "%s/%s/%s", gctx->path, subdirs[gmsg->status & M_RECENT], msg->base );
		if ((fd = open( buf, O_RDONLY )) >= 0)
			break;
		if ((ret = maildir_again( ctx, msg, "Cannot open %s", buf, 0 )) != DRV_OK) {
			cb( ret, aux );
			return;
		}
	}
	fstat( fd, &st );
	data->len = st.st_size;
	if (data->date == -1)
		data->date = st.st_mtime;
	data->data = nfmalloc( data->len );
	if (read( fd, data->data, data->len ) != data->len) {
		sys_error( "Maildir error: cannot read %s", buf );
		close( fd );
		cb( DRV_MSG_BAD, aux );
		return;
	}
	close( fd );
	if (!(gmsg->status & M_FLAGS))
		data->flags = maildir_parse_flags( ((maildir_store_conf_t *)gctx->conf)->info_prefix, msg->base );
	cb( DRV_OK, aux );
}

static int
maildir_make_flags( char info_delimiter, int flags, char *buf )
{
	uint i, d;

	buf[0] = info_delimiter;
	buf[1] = '2';
	buf[2] = ',';
	for (d = 3, i = 0; i < as(Flags); i++)
		if (flags & (1 << i))
			buf[d++] = Flags[i];
	buf[d] = 0;
	return d;
}

static void
maildir_store_msg( store_t *gctx, msg_data_t *data, int to_trash,
                   void (*cb)( int sts, int uid, void *aux ), void *aux )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	const char *box;
	int ret, fd, bl, uid;
	char buf[_POSIX_PATH_MAX], nbuf[_POSIX_PATH_MAX], fbuf[NUM_FLAGS + 3], base[128];

	bl = nfsnprintf( base, sizeof(base), "%ld.%d_%d.%s", (long)time( 0 ), Pid, ++MaildirCount, Hostname );
	if (!to_trash) {
#ifdef USE_DB
		if (ctx->usedb) {
			if ((ret = maildir_set_uid( ctx, base, &uid )) != DRV_OK) {
				free( data->data );
				cb( ret, 0, aux );
				return;
			}
		} else
#endif /* USE_DB */
		{
			if ((ret = maildir_obtain_uid( ctx, &uid )) != DRV_OK) {
				free( data->data );
				cb( ret, 0, aux );
				return;
			}
			nfsnprintf( base + bl, sizeof(base) - bl, ",U=%d", uid );
		}
		box = gctx->path;
	} else {
		uid = 0;
		box = ctx->trash;
	}

	maildir_make_flags( ((maildir_store_conf_t *)gctx->conf)->info_delimiter, data->flags, fbuf );
	nfsnprintf( buf, sizeof(buf), "%s/tmp/%s%s", box, base, fbuf );
	if ((fd = open( buf, O_WRONLY|O_CREAT|O_EXCL, 0600 )) < 0) {
		if (errno != ENOENT || !to_trash) {
			sys_error( "Maildir error: cannot create %s", buf );
			free( data->data );
			cb( DRV_BOX_BAD, 0, aux );
			return;
		}
		if ((ret = maildir_validate( box, 1, ctx )) != DRV_OK) {
			free( data->data );
			cb( ret, 0, aux );
			return;
		}
		if ((fd = open( buf, O_WRONLY|O_CREAT|O_EXCL, 0600 )) < 0) {
			sys_error( "Maildir error: cannot create %s", buf );
			free( data->data );
			cb( DRV_BOX_BAD, 0, aux );
			return;
		}
	}
	ret = write( fd, data->data, data->len );
	free( data->data );
	if (ret != data->len || (UseFSync && (ret = fsync( fd )))) {
		if (ret < 0)
			sys_error( "Maildir error: cannot write %s", buf );
		else
			error( "Maildir error: cannot write %s. Disk full?\n", buf );
		close( fd );
		cb( DRV_BOX_BAD, 0, aux );
		return;
	}
	if (close( fd ) < 0) {
		/* Quota exceeded may cause this. */
		sys_error( "Maildir error: cannot write %s", buf );
		cb( DRV_BOX_BAD, 0, aux );
		return;
	}

	if (data->date) {
		/* Set atime and mtime according to INTERNALDATE or mtime of source message */
		struct utimbuf utimebuf;
		utimebuf.actime = utimebuf.modtime = data->date;
		if (utime( buf, &utimebuf ) < 0) {
			sys_error( "Maildir error: cannot set times for %s", buf );
			cb( DRV_BOX_BAD, 0, aux );
			return;
		}
	}

	/* Moving seen messages to cur/ is strictly speaking incorrect, but makes mutt happy. */
	nfsnprintf( nbuf, sizeof(nbuf), "%s/%s/%s%s", box, subdirs[!(data->flags & F_SEEN)], base, fbuf );
	if (rename( buf, nbuf )) {
		sys_error( "Maildir error: cannot rename %s to %s", buf, nbuf );
		cb( DRV_BOX_BAD, 0, aux );
		return;
	}
	cb( DRV_OK, uid, aux );
}

static void
maildir_find_new_msgs( store_t *gctx ATTR_UNUSED, int newuid ATTR_UNUSED,
                       void (*cb)( int sts, void *aux ) ATTR_UNUSED, void *aux ATTR_UNUSED )
{
	assert( !"maildir_find_new_msgs is not supposed to be called" );
}

static void
maildir_set_msg_flags( store_t *gctx, message_t *gmsg, int uid ATTR_UNUSED, int add, int del,
                       void (*cb)( int sts, void *aux ), void *aux )
{
	maildir_store_conf_t *conf = (maildir_store_conf_t *)gctx->conf;
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	maildir_message_t *msg = (maildir_message_t *)gmsg;
	char *s, *p;
	uint i;
	int j, ret, ol, fl, bbl, bl, tl;
	char buf[_POSIX_PATH_MAX], nbuf[_POSIX_PATH_MAX];

	bbl = nfsnprintf( buf, sizeof(buf), "%s/", gctx->path );
	memcpy( nbuf, gctx->path, bbl - 1 );
	memcpy( nbuf + bbl - 1, "/cur/", 5 );
	for (;;) {
		bl = bbl + nfsnprintf( buf + bbl, sizeof(buf) - bbl, "%s/", subdirs[gmsg->status & M_RECENT] );
		ol = strlen( msg->base );
		if ((int)sizeof(buf) - bl < ol + 3 + NUM_FLAGS)
			oob();
		memcpy( buf + bl, msg->base, ol + 1 );
		memcpy( nbuf + bl, msg->base, ol + 1 );
		if ((s = strstr( nbuf + bl, conf->info_prefix ))) {
			s += 3;
			fl = ol - (s - (nbuf + bl));
			for (i = 0; i < as(Flags); i++) {
				if ((p = strchr( s, Flags[i] ))) {
					if (del & (1 << i)) {
						memmove( p, p + 1, fl - (p - s) );
						fl--;
					}
				} else if (add & (1 << i)) {
					for (j = 0; j < fl && Flags[i] > s[j]; j++);
					fl++;
					memmove( s + j + 1, s + j, fl - j );
					s[j] = Flags[i];
				}
			}
			tl = ol + 3 + fl;
		} else {
			tl = ol + maildir_make_flags( conf->info_delimiter, msg->gen.flags, nbuf + bl + ol );
		}
		if (!rename( buf, nbuf ))
			break;
		if ((ret = maildir_again( ctx, msg, "Maildir error: cannot rename %s to %s", buf, nbuf )) != DRV_OK) {
			cb( ret, aux );
			return;
		}
	}
	free( msg->base );
	msg->base = nfmalloc( tl + 1 );
	memcpy( msg->base, nbuf + bl, tl + 1 );
	msg->gen.flags |= add;
	msg->gen.flags &= ~del;
	gmsg->status &= ~M_RECENT;

	cb( DRV_OK, aux );
}

#ifdef USE_DB
static int
maildir_purge_msg( maildir_store_t *ctx, const char *name )
{
	int ret;

	if ((ret = maildir_uidval_lock( ctx )) != DRV_OK)
		return ret;
	make_key( ((maildir_store_conf_t *)ctx->gen.conf)->info_stop, &key, (char *)name );
	if ((ret = ctx->db->del( ctx->db, 0, &key, 0 ))) {
		ctx->db->err( ctx->db, ret, "Maildir error: db->del()" );
		return DRV_BOX_BAD;
	}
	return DRV_OK;
}
#endif /* USE_DB */

static void
maildir_trash_msg( store_t *gctx, message_t *gmsg,
                   void (*cb)( int sts, void *aux ), void *aux )
{
	maildir_store_t *ctx = (maildir_store_t *)gctx;
	maildir_message_t *msg = (maildir_message_t *)gmsg;
	char *s;
	int ret;
	struct stat st;
	char buf[_POSIX_PATH_MAX], nbuf[_POSIX_PATH_MAX];

	for (;;) {
		nfsnprintf( buf, sizeof(buf), "%s/%s/%s", gctx->path, subdirs[gmsg->status & M_RECENT], msg->base );
		s = strstr( msg->base, ((maildir_store_conf_t *)gctx->conf)->info_prefix );
		nfsnprintf( nbuf, sizeof(nbuf), "%s/%s/%ld.%d_%d.%s%s", ctx->trash,
		            subdirs[gmsg->status & M_RECENT], (long)time( 0 ), Pid, ++MaildirCount, Hostname, s ? s : "" );
		if (!rename( buf, nbuf ))
			break;
		if (!stat( buf, &st )) {
			if ((ret = maildir_validate( ctx->trash, 1, ctx )) != DRV_OK) {
				cb( ret, aux );
				return;
			}
			if (!rename( buf, nbuf ))
				break;
			if (errno != ENOENT) {
				sys_error( "Maildir error: cannot move %s to %s", buf, nbuf );
				cb( DRV_BOX_BAD, aux );
				return;
			}
		}
		if ((ret = maildir_again( ctx, msg, "Maildir error: cannot move %s to %s", buf, nbuf )) != DRV_OK) {
			cb( ret, aux );
			return;
		}
	}
	gmsg->status |= M_DEAD;
	gctx->count--;

#ifdef USE_DB
	if (ctx->usedb) {
		cb( maildir_purge_msg( ctx, msg->base ), aux );
		return;
	}
#endif /* USE_DB */
	cb( DRV_OK, aux );
}

static void
maildir_close_box( store_t *gctx,
                   void (*cb)( int sts, void *aux ), void *aux )
{
#ifdef USE_DB
	maildir_store_t *ctx = (maildir_store_t *)gctx;
#endif /* USE_DB */
	message_t *msg;
	int basel, retry, ret;
	char buf[_POSIX_PATH_MAX];

	for (;;) {
		retry = 0;
		basel = nfsnprintf( buf, sizeof(buf), "%s/", gctx->path );
		for (msg = gctx->msgs; msg; msg = msg->next)
			if (!(msg->status & M_DEAD) && (msg->flags & F_DELETED)) {
				nfsnprintf( buf + basel, sizeof(buf) - basel, "%s/%s", subdirs[msg->status & M_RECENT], ((maildir_message_t *)msg)->base );
				if (unlink( buf )) {
					if (errno == ENOENT)
						retry = 1;
					else
						sys_error( "Maildir error: cannot remove %s", buf );
				} else {
					msg->status |= M_DEAD;
					gctx->count--;
#ifdef USE_DB
					if (ctx->db && (ret = maildir_purge_msg( ctx, ((maildir_message_t *)msg)->base )) != DRV_OK) {
						cb( ret, aux );
						return;
					}
#endif /* USE_DB */
				}
			}
		if (!retry) {
			cb( DRV_OK, aux );
			return;
		}
		if ((ret = maildir_rescan( (maildir_store_t *)gctx )) != DRV_OK) {
			cb( ret, aux );
			return;
		}
	}
}

static void
maildir_cancel_cmds( store_t *gctx ATTR_UNUSED,
                     void (*cb)( void *aux ), void *aux )
{
	cb( aux );
}

static void
maildir_commit_cmds( store_t *gctx )
{
	(void) gctx;
}

static int
maildir_memory_usage( store_t *gctx ATTR_UNUSED )
{
	return 0;
}

static int
maildir_fail_state( store_conf_t *gconf )
{
	return ((maildir_store_conf_t *)gconf)->failed;
}

static int
maildir_parse_store( conffile_t *cfg, store_conf_t **storep )
{
	maildir_store_conf_t *store;

	if (strcasecmp( "MaildirStore", cfg->cmd ))
		return 0;
	store = nfcalloc( sizeof(*store) );
	store->info_delimiter = FieldDelimiter;
	store->gen.driver = &maildir_driver;
	store->gen.name = nfstrdup( cfg->val );

	while (getcline( cfg ) && cfg->cmd)
		if (!strcasecmp( "Inbox", cfg->cmd ))
			store->inbox = expand_strdup( cfg->val );
		else if (!strcasecmp( "Path", cfg->cmd ))
			store->gen.path = expand_strdup( cfg->val );
#ifdef USE_DB
		else if (!strcasecmp( "AltMap", cfg->cmd ))
			store->alt_map = parse_bool( cfg );
#endif /* USE_DB */
		else if (!strcasecmp( "InfoDelimiter", cfg->cmd )) {
			if (strlen( cfg->val ) != 1) {
				error( "%s:%d: Info delimiter must be exactly one character long\n", cfg->file, cfg->line );
				cfg->err = 1;
				continue;
			}
			store->info_delimiter = cfg->val[0];
			if (!ispunct( store->info_delimiter )) {
				error( "%s:%d: Info delimiter must be a punctuation character\n", cfg->file, cfg->line );
				cfg->err = 1;
				continue;
			}
		} else if (!strcasecmp( "SubFolders", cfg->cmd )) {
			if (!strcasecmp( "Verbatim", cfg->val )) {
				store->sub_style = SUB_VERBATIM;
			} else if (!strcasecmp( "Maildir++", cfg->val )) {
				store->sub_style = SUB_MAILDIRPP;
			} else if (!strcasecmp( "Legacy", cfg->val )) {
				store->sub_style = SUB_LEGACY;
			} else {
				error( "%s:%d: Unrecognized SubFolders style\n", cfg->file, cfg->line );
				cfg->err = 1;
			}
		} else
			parse_generic_store( &store->gen, cfg );
	if (!store->inbox)
		store->inbox = expand_strdup( "~/Maildir" );
	nfasprintf( &store->info_prefix, "%c2,", store->info_delimiter );
	nfasprintf( &store->info_stop, "%c,", store->info_delimiter );
	*storep = &store->gen;
	return 1;
}

struct driver maildir_driver = {
	0, /* XXX DRV_CRLF? */
	maildir_parse_store,
	maildir_cleanup_drv,
	maildir_alloc_store,
	maildir_connect_store,
	maildir_free_store,
	maildir_free_store, /* _cancel_, but it's the same */
	maildir_list_store,
	maildir_select_box,
	maildir_create_box,
	maildir_open_box,
	maildir_confirm_box_empty,
	maildir_delete_box,
	maildir_finish_delete_box,
	maildir_prepare_load_box,
	maildir_load_box,
	maildir_fetch_msg,
	maildir_store_msg,
	maildir_find_new_msgs,
	maildir_set_msg_flags,
	maildir_trash_msg,
	maildir_close_box,
	maildir_cancel_cmds,
	maildir_commit_cmds,
	maildir_memory_usage,
	maildir_fail_state,
};

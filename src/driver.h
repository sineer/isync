/*
 * mbsync - mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2006,2010-2012 Oswald Buddenhagen <ossi@users.sf.net>
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

#ifndef DRIVER_H
#define DRIVER_H

#include "config.h"

typedef struct driver driver_t;

#define FAIL_TEMP   0  /* Retry immediately (also: no error) */
#define FAIL_WAIT   1  /* Retry after some time (if at all) */
#define FAIL_FINAL  2  /* Don't retry until store reconfiguration */

typedef struct store_conf {
	struct store_conf *next;
	char *name;
	driver_t *driver;
	const char *path; /* should this be here? its interpretation is driver-specific */
	const char *flat_delim;
	const char *map_inbox;
	const char *trash;
	uint max_size; /* off_t is overkill */
	char trash_remote_new, trash_only_new;
} store_conf_t;

/* For message->flags */
/* Keep the mailbox driver flag definitions in sync! */
/* The order is according to alphabetical maildir flag sort */
#define F_DRAFT	     (1<<0) /* Draft */
#define F_FLAGGED    (1<<1) /* Flagged */
#define F_ANSWERED   (1<<2) /* Replied */
#define F_SEEN       (1<<3) /* Seen */
#define F_DELETED    (1<<4) /* Trashed */
#define NUM_FLAGS 5

/* For message->status */
#define M_RECENT       (1<<0) /* unsyncable flag; maildir_* depend on this being 1<<0 */
#define M_DEAD         (1<<1) /* expunged */
#define M_FLAGS        (1<<2) /* flags fetched */

#define TUIDL 12

typedef struct message {
	struct message *next;
	struct sync_rec *srec;
	/* string_list_t *keywords; */
	size_t size; /* zero implies "not fetched" */
	int uid;
	uchar flags, status;
	char tuid[TUIDL];
} message_t;

/* For opts, both in store and driver_t->select() */
#define OPEN_OLD        (1<<0)
#define OPEN_NEW        (1<<1)
#define OPEN_FLAGS      (1<<2)
#define OPEN_SIZE       (1<<3)
#define OPEN_EXPUNGE    (1<<5)
#define OPEN_SETFLAGS   (1<<6)
#define OPEN_APPEND     (1<<7)
#define OPEN_FIND       (1<<8)

typedef struct store {
	struct store *next;
	store_conf_t *conf; /* foreign */
	string_list_t *boxes; /* _list results - own */
	char listed; /* was _list already run? */

	void (*bad_callback)( void *aux );
	void *bad_callback_aux;

	/* currently open mailbox */
	char *path; /* own */
	message_t *msgs; /* own */
	int uidvalidity;
	int uidnext; /* from SELECT responses */
	uint opts; /* maybe preset? */
	/* note that the following do _not_ reflect stats from msgs, but mailbox totals */
	int count; /* # of messages */
	int recent; /* # of recent messages - don't trust this beyond the initial read */
} store_t;

/* When the callback is invoked (at most once per store), the store is fubar;
 * call the driver's cancel_store() to dispose of it. */
static INLINE void
set_bad_callback( store_t *ctx, void (*cb)( void *aux ), void *aux )
{
	ctx->bad_callback = cb;
	ctx->bad_callback_aux = aux;
}

typedef struct {
	char *data;
	int len;
	time_t date;
	uchar flags;
} msg_data_t;

#define DRV_OK          0
/* Message went missing, or mailbox is full, etc. */
#define DRV_MSG_BAD     1
/* Something is wrong with the current mailbox - probably it is somehow inaccessible. */
#define DRV_BOX_BAD     2
/* Failed to connect store. */
#define DRV_STORE_BAD   3
/* The command has been cancel()ed or cancel_store()d. */
#define DRV_CANCELED    4

/* All memory belongs to the driver's user, unless stated otherwise. */

/*
   This flag says that the driver CAN store messages with CRLFs,
   not that it must. The lack of it OTOH implies that it CANNOT,
   and as CRLF is the canonical format, we convert.
*/
#define DRV_CRLF        1
/*
   This flag says that the driver will act upon (DFlags & VERBOSE).
*/
#define DRV_VERBOSE     2

#define LIST_INBOX      1
#define LIST_PATH       2
#define LIST_PATH_MAYBE 4

struct driver {
	int flags;

	/* Parse configuration. */
	int (*parse_store)( conffile_t *cfg, store_conf_t **storep );

	/* Close remaining server connections. All stores must be discarded first. */
	void (*cleanup)( void );

	/* Allocate a store with the given configuration. This is expected to
	 * return quickly, and must not fail. */
	store_t *(*alloc_store)( store_conf_t *conf, const char *label );

	/* Open/connect the store. This may recycle existing server connections. */
	void (*connect_store)( store_t *ctx,
	                       void (*cb)( int sts, void *aux ), void *aux );

	/* Discard the store. Underlying server connection may be kept alive. */
	void (*free_store)( store_t *ctx );

	/* Discard the store after a bad_callback. The server connections will be closed.
	 * Pending commands will have their callbacks synchronously invoked with DRV_CANCELED. */
	void (*cancel_store)( store_t *ctx );

	/* List the mailboxes in this store. Flags are ORed LIST_* values. */
	void (*list_store)( store_t *ctx, int flags,
	                    void (*cb)( int sts, void *aux ), void *aux );

	/* Invoked before open_box(), this informs the driver which box is to be opened.
	 * As a side effect, this should resolve ctx->path if applicable. */
	int (*select_box)( store_t *ctx, const char *name );

	/* Create the selected mailbox. */
	void (*create_box)( store_t *ctx,
	                    void (*cb)( int sts, void *aux ), void *aux );

	/* Open the selected mailbox.
	 * Note that this should not directly complain about failure to open. */
	void (*open_box)( store_t *ctx,
	                  void (*cb)( int sts, void *aux ), void *aux );

	/* Confirm that the open mailbox is empty. */
	int (*confirm_box_empty)( store_t *ctx );

	/* Delete the open mailbox. The mailbox is expected to be empty.
	 * Subfolders of the mailbox are *not* deleted.
	 * Some artifacts of the mailbox may remain, but they won't be
	 * recognized as a mailbox any more. */
	void (*delete_box)( store_t *ctx,
	                    void (*cb)( int sts, void *aux ), void *aux );

	/* Remove the last artifacts of the open mailbox, as far as possible. */
	int (*finish_delete_box)( store_t *ctx );

	/* Invoked before load_box(), this informs the driver which operations (OP_*)
	 * will be performed on the mailbox. The driver may extend the set by implicitly
	 * needed or available operations. */
	void (*prepare_load_box)( store_t *ctx, int opts );

	/* Load the message attributes needed to perform the requested operations.
	 * Consider only messages with UIDs between minuid and maxuid (inclusive)
	 * and those named in the excs array (smaller than minuid).
	 * The driver takes ownership of the excs array. Messages below newuid do not need
	 * to have the TUID populated even if OPEN_FIND is set. */
	void (*load_box)( store_t *ctx, int minuid, int maxuid, int newuid, int *excs, int nexcs,
	                  void (*cb)( int sts, void *aux ), void *aux );

	/* Fetch the contents and flags of the given message from the current mailbox. */
	void (*fetch_msg)( store_t *ctx, message_t *msg, msg_data_t *data,
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Store the given message to either the current mailbox or the trash folder.
	 * If the new copy's UID can be immediately determined, return it, otherwise -2. */
	void (*store_msg)( store_t *ctx, msg_data_t *data, int to_trash,
	                   void (*cb)( int sts, int uid, void *aux ), void *aux );

	/* Index the messages which have newly appeared in the mailbox, including their
	 * temporary UID headers. This is needed if store_msg() does not guarantee returning
	 * a UID; otherwise the driver needs to implement only the OPEN_FIND flag. */
	void (*find_new_msgs)( store_t *ctx, int newuid,
	                       void (*cb)( int sts, void *aux ), void *aux );

	/* Add/remove the named flags to/from the given message. The message may be either
	 * a pre-fetched one (in which case the in-memory representation is updated),
	 * or it may be identifed by UID only. The operation may be delayed until commit()
	 * is called. */
	void (*set_msg_flags)( store_t *ctx, message_t *msg, int uid, int add, int del, /* msg can be null, therefore uid as a fallback */
	                       void (*cb)( int sts, void *aux ), void *aux );

	/* Move the given message from the current mailbox to the trash folder.
	 * This may expunge the original message immediately, but it needn't to. */
	void (*trash_msg)( store_t *ctx, message_t *msg, /* This may expunge the original message immediately, but it needn't to */
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Expunge deleted messages from the current mailbox and close it.
	 * There is no need to explicitly close a mailbox if no expunge is needed. */
	void (*close_box)( store_t *ctx, /* IMAP-style: expunge inclusive */
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Cancel queued commands which are not in flight yet; they will have their
	 * callbacks invoked with DRV_CANCELED. Afterwards, wait for the completion of
	 * the in-flight commands. If the store is canceled before this command completes,
	 * the callback will *not* be invoked. */
	void (*cancel_cmds)( store_t *ctx,
	                     void (*cb)( void *aux ), void *aux );

	/* Commit any pending set_msg_flags() commands. */
	void (*commit_cmds)( store_t *ctx );

	/* Get approximate amount of memory occupied by the driver. */
	int (*memory_usage)( store_t *ctx );

	/* Get the FAIL_* state of the driver. */
	int (*fail_state)( store_conf_t *conf );
};

void free_generic_messages( message_t * );

void parse_generic_store( store_conf_t *store, conffile_t *cfg );

#define N_DRIVERS 2
extern driver_t *drivers[N_DRIVERS];
extern driver_t maildir_driver, imap_driver;

#endif

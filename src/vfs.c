#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "../include/dqlite.h"

#include "lib/byte.h"

#include "format.h"
#include "raft.h"
#include "tracing.h"
#include "vfs.h"

/* tinycc doesn't have this builtin, nor the warning that it's meant to silence.
 */
#ifdef __TINYC__
#define __builtin_assume_aligned(x, y) x
#endif

/* Byte order */
#if defined(DQLITE_LITTLE_ENDIAN)
#define VFS__BIGENDIAN 0
#elif defined(DQLITE_BIG_ENDIAN)
#define VFS__BIGENDIAN 1
#else
const int vfsOne = 1;
#define VFS__BIGENDIAN (*(char *)(&vfsOne) == 0)
#endif

/* Maximum pathname length supported by this VFS. */
#define VFS__MAX_PATHNAME 512

/* WAL magic value. Either this value, or the same value with the least
 * significant bit also set (FORMAT__WAL_MAGIC | 0x00000001) is stored in 32-bit
 * big-endian format in the first 4 bytes of a WAL file.
 *
 * If the LSB is set, then the checksums for each frame within the WAL file are
 * calculated by treating all data as an array of 32-bit big-endian
 * words. Otherwise, they are calculated by interpreting all data as 32-bit
 * little-endian words. */
#define VFS__WAL_MAGIC 0x377f0682

/* WAL format version (same for WAL index). */
#define VFS__WAL_VERSION 3007000

/* Index of the write lock in the WAL-index header locks area. */
#define VFS__WAL_WRITE_LOCK 0
#define VFS__WAL_CKPT_LOCK  1
#define VFS__WAL_RECOVER_LOCK  2

/* Write ahead log header size. */
#define VFS__WAL_HEADER_SIZE 32

/* Write ahead log frame header size. */
#define VFS__FRAME_HEADER_SIZE 24

/* Size of the first part of the WAL index header. */
#define VFS__WAL_INDEX_HEADER_SIZE 48

/* Size of a single memory-mapped WAL index region. */
#define VFS__WAL_INDEX_REGION_SIZE 32768

/* Offset of the "in header database size" field in the main database file. */
#define VFS__IN_HEADER_DATABASE_SIZE_OFFSET 28


/******************************************************************************/
/*                                   Helpers                                  */
/******************************************************************************/

#define vfsFrameSize(PAGE_SIZE) (VFS__FRAME_HEADER_SIZE + PAGE_SIZE)

/*
 * Generate or extend an 8 byte checksum based on the data in array data[] and
 * the initial values of in[0] and in[1] (or initial values of 0 and 0 if
 * in==NULL).
 *
 * The checksum is written back into out[] before returning.
 *
 * n must be a positive multiple of 8. */
static void vfsChecksum(
    uint8_t *data, /* Content to be checksummed */
    unsigned n,    /* Bytes of content in a[].  Must be a multiple of 8. */
    const uint32_t in[2], /* Initial checksum value input */
    uint32_t out[2]       /* OUT: Final checksum value output */
)
{
	assert((((uintptr_t)data) % sizeof(uint32_t)) == 0);

	uint32_t s1, s2;
	uint32_t *cur =
	    (uint32_t *)__builtin_assume_aligned(data, sizeof(uint32_t));
	uint32_t *end =
	    (uint32_t *)__builtin_assume_aligned(&data[n], sizeof(uint32_t));

	if (in) {
		s1 = in[0];
		s2 = in[1];
	} else {
		s1 = s2 = 0;
	}

	assert(n >= 8);
	assert((n & 0x00000007) == 0);
	assert(n <= 65536);

	do {
		s1 += *cur++ + s2;
		s2 += *cur++ + s1;
	} while (cur < end);

	out[0] = s1;
	out[1] = s2;
}

/* Return the page number field stored in the header of the given frame. */
#define vfsFrameGetPageNumber(f) ByteGetBe32(&((f)->header[0]))

/* Return the database size field stored in the header of the given frame. */
#define vfsFrameGetDatabaseSize(f) ByteGetBe32(&((f)->header[4]))

/* Return the checksum-1 field stored in the header of the given frame. */
#define vfsFrameGetChecksum1(f) ByteGetBe32(&((f)->header[16]))

/* Return the checksum-2 field stored in the header of the given frame. */
#define vfsFrameGetChecksum2(f) ByteGetBe32(&((f)->header[20]))

/* Return the salt-1 field stored in the WAL header.*/
#define vfsWalGetSalt1(w) *(uint32_t *)__builtin_assume_aligned(&(w)->hdr[16], sizeof(uint32_t))

/* Return the salt-2 field stored in the WAL header.*/
#define vfsWalGetSalt2(w) *(uint32_t *)__builtin_assume_aligned(&(w)->hdr[20], sizeof(uint32_t))

/* Return the checksum-1 field stored in the WAL header.*/
#define vfsWalGetChecksum1(w) ByteGetBe32(&(w)->hdr[24])

/* Return the checksum-2 field stored in the WAL header.*/
#define vfsWalGetChecksum2(w) ByteGetBe32(&(w)->hdr[28]);

/* Parse the page size ("Must be a power of two between 512 and 32768
 * inclusive, or the value 1 representing a page size of 65536").
 *
 * Return 0 if the page size is out of bound. */
static uint32_t vfsParsePageSize(uint32_t page_size)
{
	if (page_size == 1) {
		page_size = FORMAT__PAGE_SIZE_MAX;
	} else if (page_size < FORMAT__PAGE_SIZE_MIN) {
		page_size = 0;
	} else if (page_size > (FORMAT__PAGE_SIZE_MAX / 2)) {
		page_size = 0;
	} else if (((page_size - 1) & page_size) != 0) {
		page_size = 0;
	}

	return page_size;
}

static bool vfsFilenameEndsWith(const char *filename, const char *suffix)
{
	size_t n_filename = strlen(filename);
	size_t n_suffix = strlen(suffix);
	if (n_suffix > n_filename) {
		return false;
	}
	return strncmp(filename + n_filename - n_suffix, suffix, n_suffix) == 0;
}

/******************************************************************************/
/*                            Main data structures                            */
/******************************************************************************/

/* Hold the content of a single WAL frame. */
struct vfsFrame
{
	uint8_t header[VFS__FRAME_HEADER_SIZE];
	uint8_t *page; /* Content of the page. */
};

/* Create a new frame of a WAL file. */
static struct vfsFrame *vfsFrameCreate(unsigned size)
{
	struct vfsFrame *f;

	assert(size > 0);

	f = sqlite3_malloc(sizeof *f);
	if (f == NULL) {
		goto oom;
	}

	f->page = sqlite3_malloc64(size);
	if (f->page == NULL) {
		goto oom_after_page_alloc;
	}

	memset(f->header, 0, FORMAT__WAL_FRAME_HDR_SIZE);
	memset(f->page, 0, (size_t)size);

	return f;

oom_after_page_alloc:
	sqlite3_free(f);
oom:
	return NULL;
}

/* Fill the header and the content of a WAL frame. The given checksum is the
 * rolling one of all preceeding frames and is updated by this function. */
static void vfsFrameFill(struct vfsFrame *f,
			 uint32_t page_number,
			 uint32_t database_size,
			 uint32_t salt[2],
			 uint32_t checksum[2],
			 uint8_t *page,
			 uint32_t page_size)
{
	BytePutBe32(page_number, &f->header[0]);
	BytePutBe32(database_size, &f->header[4]);

	vfsChecksum(f->header, 8, checksum, checksum);
	vfsChecksum(page, page_size, checksum, checksum);

	memcpy(&f->header[8], &salt[0], sizeof salt[0]);
	memcpy(&f->header[12], &salt[1], sizeof salt[1]);

	BytePutBe32(checksum[0], &f->header[16]);
	BytePutBe32(checksum[1], &f->header[20]);

	memcpy(f->page, page, page_size);
}

/* Destroy a WAL frame */
static void vfsFrameDestroy(struct vfsFrame *f)
{
	assert(f != NULL);
	assert(f->page != NULL);

	sqlite3_free(f->page);
	sqlite3_free(f);
}

/* Hold content for a shared memory mapping. */
struct vfsShm
{
	int fd;
	int size;
	/* Lock array. Each of these lock has the following semantics:
	 *  -  0 means unlocked;
	 *  - -1 means exclusive locked;
	 *  - >0 means shared locked and the value is the count of
	 *       shared locks taken.
	 */
	int lock[SQLITE_SHM_NLOCK];
};

/* Initialize the shared memory mapping of a database file. */
static int vfsShmInit(struct vfsShm *s)
{
	int fd = memfd_create("dqlite-shm", 0);
	if (fd < 0) {
		return SQLITE_NOMEM;
	}
	*s = (struct vfsShm){
		.fd = fd,
	};
	return SQLITE_OK;
}

static int vfsShmLock(struct vfsShm *s, int ofst, int n, bool exclusive)
{
	PRE(ofst >= 0 && n > 0 && ofst + n <= SQLITE_SHM_NLOCK);

	for (int i = ofst; i < ofst + n; i++) {
		if (exclusive) {
			if (s->lock[i] != 0) {
				return SQLITE_BUSY;
			}
		} else {
			if (s->lock[i] < 0) {
				return SQLITE_BUSY;
			}
		}
	}

	for (int i = ofst; i < ofst + n; i++) {
		if (exclusive) {
			s->lock[i] = -1;
		} else {
			s->lock[i]++;
		}
	}

	return SQLITE_OK;
}

static int vfsShmUnlock(struct vfsShm *s, int ofst, int n, bool exclusive)
{
	PRE(ofst >= 0 && n > 0 && ofst + n <= SQLITE_SHM_NLOCK);

	for (int i = ofst; i < ofst + n; i++) {
		if (exclusive) {
			PRE(s->lock[i] < 0);
			s->lock[i] = 0;
		} else {
			PRE(s->lock[i] > 0);
			s->lock[i]--;
		}
	}
	return SQLITE_OK;
}

/* Release all resources used by a shared memory mapping. */
static void vfsShmClose(struct vfsShm *s)
{
	int rv = close(s->fd);
	if (rv != 0 && errno != EINTR) {
		tracef("closing shared memory failed: %d", errno);
	}
}

/* WAL-specific content.
 * Watch out when changing the members of this struct, see
 * comment in `formatWalChecksumBytes`. */
struct vfsWal
{
	uint8_t hdr[VFS__WAL_HEADER_SIZE]; /* Header. */
	struct vfsFrame **frames;          /* All frames committed. */
	unsigned n_frames;                 /* Number of committed frames. */
	struct vfsFrame **tx;              /* Frames added by a transaction. */
	unsigned n_tx;                     /* Number of added frames. */
};

/* Initialize a new WAL object. */
static void vfsWalInit(struct vfsWal *w)
{
	*w = (struct vfsWal){};
}

/* Lookup a frame from the WAL, returning NULL if it doesn't exist. */
static struct vfsFrame *vfsWalFrameLookup(struct vfsWal *w, unsigned n)
{
	struct vfsFrame *frame;

	assert(w != NULL);
	assert(n > 0);

	if (n > w->n_frames + w->n_tx) {
		/* This page hasn't been written yet. */
		return NULL;
	}
	if (n <= w->n_frames) {
		frame = w->frames[n - 1];
	} else {
		frame = w->tx[n - w->n_frames - 1];
	}

	assert(frame != NULL);

	return frame;
}

/* Get a frame from the current transaction, possibly creating a new one. */
static int vfsWalFrameGet(struct vfsWal *w,
			  unsigned index,
			  uint32_t page_size,
			  struct vfsFrame **frame)
{
	int rv;

	assert(w != NULL);
	assert(index > 0);

	/* SQLite should access pages progressively, without jumping more than
	 * one page after the end. */
	if (index > w->n_frames + w->n_tx + 1) {
		rv = SQLITE_IOERR_WRITE;
		goto err;
	}

	if (index == w->n_frames + w->n_tx + 1) {
		/* Create a new frame, grow the transaction array, and append
		 * the new frame to it. */
		struct vfsFrame **tx;

		/* We assume that the page size has been set, either by
		 * intervepting the first main database file write, or by
		 * handling a 'PRAGMA page_size=N' command in
		 * vfs__file_control(). This assumption is enforved in
		 * vfsFileWrite(). */
		assert(page_size > 0);

		*frame = vfsFrameCreate(page_size);
		if (*frame == NULL) {
			rv = SQLITE_NOMEM;
			goto err;
		}

		tx = sqlite3_realloc64(w->tx, sizeof *tx * w->n_tx + 1);
		if (tx == NULL) {
			rv = SQLITE_NOMEM;
			goto err_after_vfs_frame_create;
		}

		/* Append the new page to the new page array. */
		tx[index - w->n_frames - 1] = *frame;

		/* Update the page array. */
		w->tx = tx;
		w->n_tx++;
	} else {
		/* Return the existing page. */
		assert(w->tx != NULL);
		*frame = w->tx[index - w->n_frames - 1];
	}

	return SQLITE_OK;

err_after_vfs_frame_create:
	vfsFrameDestroy(*frame);
err:
	*frame = NULL;
	return rv;
}

/* Get the page size stored in the WAL header. */
static uint32_t vfsWalGetPageSize(struct vfsWal *w)
{
	/* The page size is stored in the 4 bytes starting at 8
	 * (big-endian) */
	return vfsParsePageSize(ByteGetBe32(&w->hdr[8]));
}

/* Release all memory used by a WAL object. */
static void vfsWalClose(struct vfsWal *w)
{
	unsigned i;
	for (i = 0; i < w->n_frames; i++) {
		vfsFrameDestroy(w->frames[i]);
	}
	if (w->frames != NULL) {
		sqlite3_free(w->frames);
	}
	for (i = 0; i < w->n_tx; i++) {
		vfsFrameDestroy(w->tx[i]);
	}
	if (w->tx != NULL) {
		sqlite3_free(w->tx);
	}
}

/* Database-specific content */
struct vfsDatabase
{
	char *name;         /* Database name. Read only. */
	void **pages;       /* All database. */
	unsigned page_size; /* Only used for on-disk db */
	unsigned n_pages;   /* Number of pages. */
	struct vfsShm shm;  /* Shared memory. */
	struct vfsWal wal;  /* Associated WAL. */
};

/*
 * Comment copied entirely for sqlite source code, it is safe to assume
 * the value 0x40000000 will never change. dq_sqlite_pending_byte is global
 * to be able to adapt it in the unittest, the value must never be changed.
 *
 * ==BEGIN COPY==
 * The value of the "pending" byte must be 0x40000000 (1 byte past the
 * 1-gibabyte boundary) in a compatible database.  SQLite never uses
 * the database page that contains the pending byte.  It never attempts
 * to read or write that page.  The pending byte page is set aside
 * for use by the VFS layers as space for managing file locks.
 *
 * During testing, it is often desirable to move the pending byte to
 * a different position in the file.  This allows code that has to
 * deal with the pending byte to run on files that are much smaller
 * than 1 GiB.  The sqlite3_test_control() interface can be used to
 * move the pending byte.
 *
 * IMPORTANT:  Changing the pending byte to any value other than
 * 0x40000000 results in an incompatible database file format!
 * Changing the pending byte during operation will result in undefined
 * and incorrect behavior.
 * ==END COPY==
 */
DQLITE_VISIBLE_TO_TESTS unsigned dq_sqlite_pending_byte = 0x40000000;

/* Initialize a new database object. */
static int vfsDatabaseInit(struct vfsDatabase *d, const char *name)
{
	char *dbname = sqlite3_malloc((int)strlen(name) + 1);
	if (dbname == NULL) {
		return SQLITE_NOMEM;
	}
	strcpy(dbname, name);

	*d = (struct vfsDatabase){
		.name = dbname,
	};
	int rv = vfsShmInit(&d->shm);
	if (rv != SQLITE_OK) {
		sqlite3_free(dbname);
		return rv;
	}
	vfsWalInit(&d->wal);
	return SQLITE_OK;
}

/* Get a page from the given database, possibly creating a new one. */
static int vfsDatabaseGetPage(struct vfsDatabase *d,
			      uint32_t page_size,
			      unsigned pgno,
			      void **page)
{
	int rc;

	assert(d != NULL);
	assert(pgno > 0);

	/* SQLite should access pages progressively, without jumping more than
	 * one page after the end unless one would attempt to access a page at
	 * `sqlite_pending_byte` offset, skipping a page is permitted then. */
	bool pending_byte_page_reached =
	    (page_size * d->n_pages == dq_sqlite_pending_byte);
	if ((pgno > d->n_pages + 1) && !pending_byte_page_reached) {
		tracef("page number greater than length (requested %d, last %d)",
		       pgno, d->n_pages);
		*page = NULL;
		return SQLITE_IOERR_WRITE;
	}

	if (pgno <= d->n_pages) {
		/* Return the existing page. */
		assert(d->pages != NULL);
		*page = d->pages[pgno - 1];
		return SQLITE_OK;
	}

	/* Create a new page, grow the page array, and append the
	 * new page to it. */
	*page = sqlite3_malloc64(page_size);
	if (*page == NULL) {
		rc = SQLITE_NOMEM;
		goto err;
	}

	void **pages = sqlite3_realloc64(d->pages, sizeof *pages * pgno);
	if (pages == NULL) {
		rc = SQLITE_NOMEM;
		goto err_after_vfs_page_create;
	}

	pages[pgno - 1] = *page;

	/* Allocate a page to store the pending_byte */
	if (pending_byte_page_reached) {
		void *pending_byte_page = sqlite3_malloc64(page_size);
		if (pending_byte_page == NULL) {
			rc = SQLITE_NOMEM;
			goto err_after_pending_byte_page;
		}
		pages[d->n_pages] = pending_byte_page;
	}

	/* Update the page array. */
	d->pages = pages;
	d->n_pages = pgno;

	return SQLITE_OK;

err_after_pending_byte_page:
	d->pages = pages;

err_after_vfs_page_create:
	sqlite3_free(*page);
err:
	*page = NULL;
	return rc;
}

/* Lookup a page from the given database, returning NULL if it doesn't exist. */
static void *vfsDatabasePageLookup(struct vfsDatabase *d, unsigned pgno)
{
	void *page;

	assert(d != NULL);
	assert(pgno > 0);

	if (pgno > d->n_pages) {
		/* This page hasn't been written yet. */
		return NULL;
	}

	page = d->pages[pgno - 1];

	assert(page != NULL);

	return page;
}

static uint32_t vfsDatabaseGetPageSize(struct vfsDatabase *d)
{
	uint8_t *page;

	/* Only set in disk-mode */
	if (d->page_size != 0) {
		return d->page_size;
	}

	assert(d->n_pages > 0);
	page = d->pages[0];

	/* The page size is stored in the 16th and 17th bytes of the first
	 * database page (big-endian) */
	return vfsParsePageSize(ByteGetBe16(&page[16]));
}

/* Return the size of the database file in bytes. */
static int64_t vfsDatabaseFileSize(struct vfsDatabase *d)
{
	int64_t size = 0;
	if (d->n_pages > 0) {
		size = (int64_t)d->n_pages * (int64_t)vfsDatabaseGetPageSize(d);
	}
	/* TODO dqlite is limited to a max database size of SIZE_MAX */
	assert((uint64_t)size <= SIZE_MAX);
	return size;
}

/* This function modifies part of the WAL index header to reflect the current
 * content of the WAL.
 *
 * It is called in two cases. First, after a write transaction gets completed
 * and the SQLITE_FCNTL_COMMIT_PHASETWO file control op code is triggered, in
 * order to "rewind" the mxFrame and szPage fields of the WAL index header back
 * to when the write transaction started, effectively "shadowing" the
 * transaction, which will be replicated asynchronously. Second, when the
 * replication actually succeeds and dqlite_vfs_apply() is called on the VFS
 * that originated the transaction, in order to make the transaction visible.
 *
 * Note that the hash table contained in the WAL index does not get modified,
 * and even after a rewind following a write transaction it will still contain
 * entries for the frames committed by the transaction. That's safe because
 * mxFrame will make clients ignore those hash table entries. However it means
 * that in case the replication is not actually successful and
 * dqlite_vfs_abort() is called the WAL index must be invalidated.
 **/
static void vfsAmendWalIndexHeader(struct vfsDatabase *d)
{
	struct vfsShm *shm = &d->shm;
	struct vfsWal *wal = &d->wal;
	uint8_t index[VFS__WAL_INDEX_HEADER_SIZE * 2];
	uint32_t frame_checksum[2] = { 0, 0 };
	uint32_t n_pages = (uint32_t)d->n_pages;
	uint32_t checksum[2] = { 0, 0 };

	assert(shm->size > 0);
	ssize_t rv = pread(shm->fd, index, sizeof(index), 0);
	assert(rv == sizeof(index));

	if (wal->n_frames > 0) {
		struct vfsFrame *last = wal->frames[wal->n_frames - 1];
		frame_checksum[0] = vfsFrameGetChecksum1(last);
		frame_checksum[1] = vfsFrameGetChecksum2(last);
		n_pages = vfsFrameGetDatabaseSize(last);
	}

	/* index is an alias for shm->regions[0] which is a void* that points to
	 * memory allocated by `sqlite3_malloc64` and has the required alignment
	 */
	uint32_t index0;
	memcpy(&index0, &index[0], sizeof(uint32_t));
	assert(index0 == VFS__WAL_VERSION);  /* iVersion */
	assert(index[12] == 1);              /* isInit */
	assert(index[13] == VFS__BIGENDIAN); /* bigEndCksum */

	memcpy(&index[16], &wal->n_frames, sizeof(uint32_t));
	memcpy(&index[20], &n_pages, sizeof(uint32_t));
	memcpy(&index[24], &frame_checksum[0], sizeof(uint32_t));
	memcpy(&index[28], &frame_checksum[1], sizeof(uint32_t));

	vfsChecksum(index, 40, checksum, checksum);

	memcpy(&index[40], &checksum[0], sizeof(uint32_t));
	memcpy(&index[44], &checksum[1], sizeof(uint32_t));

	/* Update the second copy of the first part of the WAL index header. */
	memcpy(index + VFS__WAL_INDEX_HEADER_SIZE, index,
	       VFS__WAL_INDEX_HEADER_SIZE);
	rv = pwrite(shm->fd, index, sizeof(index), 0);
	assert(rv == sizeof(index));
}

/* Truncate a database file to be exactly the given number of pages. */
static int vfsDatabaseTruncate(struct vfsDatabase *d, sqlite_int64 size)
{
	void **cursor;
	uint32_t page_size;
	unsigned n_pages;
	unsigned i;

	if (d->n_pages == 0) {
		if (size > 0) {
			return SQLITE_IOERR_TRUNCATE;
		}
		return SQLITE_OK;
	}

	/* Since the file size is not zero, some content must
	 * have been written and the page size must be known. */
	page_size = vfsDatabaseGetPageSize(d);
	assert(page_size > 0);

	if ((size % page_size) != 0) {
		return SQLITE_IOERR_TRUNCATE;
	}

	n_pages = (unsigned)(size / page_size);

	/* We expect callers to only invoke us if some actual content has been
	 * written already. */
	assert(d->n_pages > 0);

	/* Truncate should always shrink a file. */
	assert(n_pages <= d->n_pages);
	assert(d->pages != NULL);

	/* Destroy pages beyond pages_len. */
	cursor = d->pages + n_pages;
	for (i = 0; i < (d->n_pages - n_pages); i++) {
		sqlite3_free(*cursor);
		cursor++;
	}

	/* Shrink the page array, possibly to 0.
	 *
	 * TODO: in principle realloc could fail also when shrinking. */
	d->pages = sqlite3_realloc64(d->pages, sizeof *d->pages * n_pages);

	/* Update the page count. */
	d->n_pages = n_pages;

	return SQLITE_OK;
}

/* Release all memory used by a database object. */
static void vfsDatabaseClose(struct vfsDatabase *d)
{
	for (unsigned i = 0; d->pages != NULL && i < d->n_pages; i++) {
		sqlite3_free(d->pages[i]);
	}
	sqlite3_free(d->pages);
	sqlite3_free(d->name);
	vfsWalClose(&d->wal);
	vfsShmClose(&d->shm);
}

/* Custom dqlite VFS. Contains pointers to all databases that were created. */
struct vfs
{
	struct vfsDatabase **databases; /* Database objects */
	unsigned n_databases;           /* Number of databases */
	int error;                      /* Last error occurred. */
	bool disk; /* True if the database is kept on disk. */
	struct sqlite3_vfs *base_vfs; /* Base VFS. */
};

/* Create a new vfs object. */
static struct vfs *vfsCreate(void)
{
	struct vfs *v;

	v = sqlite3_malloc(sizeof *v);
	if (v == NULL) {
		return NULL;
	}

	*v = (struct vfs) {
		.base_vfs = sqlite3_vfs_find("unix"),
	};
	assert(v->base_vfs != NULL);
	return v;
}

/* Create a database object and add it to the databases array. */
static struct vfsDatabase *vfsCreateDatabase(struct vfs *v, const char *name)
{
	unsigned n = v->n_databases + 1;
	struct vfsDatabase **databases;
	struct vfsDatabase *d;

	assert(name != NULL);

	/* Create a new entry. */
	databases = sqlite3_realloc64(v->databases, sizeof *databases * n);
	if (databases == NULL) {
		return NULL;
	}
	v->databases = databases;

	d = sqlite3_malloc(sizeof *d);
	if (d == NULL) {
		return NULL;
	}

	int rv = vfsDatabaseInit(d, name);
	if (rv != SQLITE_OK) {
		sqlite3_free(d);
		return NULL;
	}

	v->databases[n - 1] = d;
	v->n_databases = n;

	return d;
}

/* Find the database object associated with the given filename. */
static struct vfsDatabase *vfsDatabaseLookup(struct vfs *v,
					     const char *filename)
{
	size_t n = strlen(filename);
	unsigned i;

	assert(v != NULL);
	assert(filename != NULL);

	if (vfsFilenameEndsWith(filename, "-wal")) {
		n -= strlen("-wal");
	}
	if (vfsFilenameEndsWith(filename, "-journal")) {
		n -= strlen("-journal");
	}

	for (i = 0; i < v->n_databases; i++) {
		struct vfsDatabase *database = v->databases[i];
		if (strlen(database->name) == n &&
		    strncmp(database->name, filename, n) == 0) {
			// Found matching file.
			return database;
		}
	}

	return NULL;
}

static int vfsDeleteDatabase(struct vfs *r, const char *name)
{
	for (unsigned i = 0; i < r->n_databases; i++) {
		struct vfsDatabase *database = r->databases[i];
		unsigned j;

		if (strcmp(database->name, name) != 0) {
			continue;
		}

		vfsDatabaseClose(database);
		sqlite3_free(database);

		/* Shift all other contents objects. */
		for (j = i + 1; j < r->n_databases; j++) {
			r->databases[j - 1] = r->databases[j];
		}
		r->n_databases--;

		return SQLITE_OK;
	}

	r->error = ENOENT;
	return SQLITE_IOERR_DELETE_NOENT;
}

/* Release the memory used internally by the VFS object.
 *
 * All file content will be de-allocated, so dangling open FDs against
 * those files will be broken.
 */
static void vfsDestroy(struct vfs *r)
{
	for (unsigned i = 0; i < r->n_databases; i++) {
		struct vfsDatabase *database = r->databases[i];
		vfsDatabaseClose(database);
		sqlite3_free(database);
	}
	sqlite3_free(r->databases);
}

/******************************************************************************/
/*                           SQLite3 implementation                           */
/******************************************************************************/

typedef sqlite3_file vfsNoopFile;

static int vfsNoopClose(sqlite3_file *file)
{
	(void)file;
	return SQLITE_OK;
}

static int vfsNoopTruncate(sqlite3_file *file, sqlite3_int64 size)
{
	(void)file;
	(void)size;
	return SQLITE_IOERR_TRUNCATE;
}

static int vfsNoopSync(sqlite3_file *file, int flags)
{
	(void)file;
	(void)flags;
	return SQLITE_IOERR;
}

static int vfsNoopFileSize(sqlite3_file *file, sqlite3_int64 *pSize)
{
	(void)file;
	(void)pSize;
	*pSize = 0;
	return SQLITE_OK;
}

static int vfsNoopLock(sqlite3_file *file, int lockType)
{
	(void)file;
	(void)lockType;
	return SQLITE_OK;
}

static int vfsNoopUnlock(sqlite3_file *file, int lockType)
{
	(void)file;
	(void)lockType;
	return SQLITE_OK;
}

static int vfsNoopFileControl(sqlite3_file *file, int op, void *pArg)
{
	(void)file;
	(void)op;
	(void)pArg;
	return SQLITE_NOTFOUND;
}

static int vfsNoopSectorSize(sqlite3_file *file)
{
	(void)file;
	return 0;
}

static int vfsNoopRead(sqlite3_file *file,
		       void *data,
		       int iAmt,
		       sqlite3_int64 iOfst)
{
	(void)file;
	(void)iOfst;
	memset(data, 0, (size_t)iAmt);  // Always empty
	return SQLITE_OK;
}

static int vfsNoopWrite(sqlite3_file *file,
			const void *data,
			int iAmt,
			sqlite3_int64 iOfst)
{
	(void)file;
	(void)data;
	(void)iAmt;
	(void)iOfst;
	return SQLITE_OK;
}

static int vfsNoopCheckReservedLock(sqlite3_file *file, int *pResOut)
{
	(void)file;
	*pResOut = 0;
	return SQLITE_OK;
}

static int vfsNoopDeviceCharacteristics(sqlite3_file *file)
{
	(void)file;
	return SQLITE_IOCAP_ATOMIC | SQLITE_IOCAP_SAFE_APPEND |
	       SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE;
}

static const sqlite3_io_methods vfsNoopMethods = {
	.iVersion = 1,
	.xClose = vfsNoopClose,
	.xRead = vfsNoopRead,
	.xWrite = vfsNoopWrite,
	.xTruncate = vfsNoopTruncate,
	.xSync = vfsNoopSync,
	.xFileSize = vfsNoopFileSize,
	.xLock = vfsNoopLock,
	.xUnlock = vfsNoopUnlock,
	.xCheckReservedLock = vfsNoopCheckReservedLock,
	.xFileControl = vfsNoopFileControl,
	.xSectorSize = vfsNoopSectorSize,
	.xDeviceCharacteristics = vfsNoopDeviceCharacteristics,
};

struct vfsWalFile
{
	sqlite3_file base;  /* Base class. Must be first. */
	struct vfsWal *wal; /* Underlying in-memory wal. */
};

/* Return the size of the WAL file in bytes. */
static int64_t vfsWalSize(struct vfsWal *w)
{
	int64_t size = 0;
	if (w->n_frames > 0) {
		uint32_t page_size;
		page_size = vfsWalGetPageSize(w);
		size += VFS__WAL_HEADER_SIZE;
		size += (int64_t)w->n_frames *
			(int64_t)(FORMAT__WAL_FRAME_HDR_SIZE + page_size);
	}
	/* TODO dqlite is limited to a max database size of SIZE_MAX */
	assert((size >= 0) && ((uint64_t)size <= SIZE_MAX));
	return (int64_t)size;
}

static int vfsWalFileSize(sqlite3_file* file, sqlite3_int64 *pSize)
{
	struct vfsWalFile *f = (struct vfsWalFile *)file;
	*pSize = (sqlite3_int64)vfsWalSize(f->wal);
	return SQLITE_OK;
}

static int vfsWalFileRead(sqlite3_file* file, void* buf, int amount, sqlite3_int64 offset)
{
	struct vfsWalFile *f = (struct vfsWalFile *)file;

	uint32_t page_size;
	unsigned index;
	struct vfsFrame *frame;

	if (offset == 0) {
		/* Read the header. */
		assert(amount == VFS__WAL_HEADER_SIZE);
		memcpy(buf, f->wal->hdr, VFS__WAL_HEADER_SIZE);
		return SQLITE_OK;
	}

	page_size = vfsWalGetPageSize(f->wal);
	assert(page_size > 0);

	/* For any other frame, we expect either a header read,
	 * a checksum read, a page read or a full frame read. */
	if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
		assert(((offset - VFS__WAL_HEADER_SIZE) %
			((int)page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
		index =
		    (unsigned)formatWalCalcFrameIndex((int)page_size, offset);
	} else if (amount == sizeof(uint32_t) * 2) {
		if (offset == FORMAT__WAL_FRAME_HDR_SIZE) {
			/* Read the checksum from the WAL
			 * header. */
			memcpy(buf, f->wal->hdr + offset, (size_t)amount);
			return SQLITE_OK;
		}
		assert(((offset - 16 - VFS__WAL_HEADER_SIZE) %
			((int)page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
		index =
		    (unsigned)((offset - 16 - VFS__WAL_HEADER_SIZE) /
			       ((int)page_size + FORMAT__WAL_FRAME_HDR_SIZE)) +
		    1;
	} else if (amount == (int)page_size) {
		assert(((offset - VFS__WAL_HEADER_SIZE -
			 FORMAT__WAL_FRAME_HDR_SIZE) %
			((int)page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
		index =
		    (unsigned)formatWalCalcFrameIndex((int)page_size, offset);
	} else {
		assert(amount == (FORMAT__WAL_FRAME_HDR_SIZE + (int)page_size));
		index =
		    (unsigned)formatWalCalcFrameIndex((int)page_size, offset);
	}

	if (index == 0) {
		/* From SQLite docs:
		*
		*   If xRead() returns SQLITE_IOERR_SHORT_READ it must also fill
		*   in the unread portions of the buffer with zeros.  A VFS that
		*   fails to zero-fill short reads might seem to work.  However,
		*   failure to zero-fill short reads will eventually lead to
		*   database corruption.
		*/
		memset(buf, 0, (size_t)amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	frame = vfsWalFrameLookup(f->wal, index);
	if (frame == NULL) {
		/* From SQLite docs:
		*
		*   If xRead() returns SQLITE_IOERR_SHORT_READ it must also fill
		*   in the unread portions of the buffer with zeros.  A VFS that
		*   fails to zero-fill short reads might seem to work.  However,
		*   failure to zero-fill short reads will eventually lead to
		*   database corruption.
		*/
		memset(buf, 0, (size_t)amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
		memcpy(buf, frame->header, (size_t)amount);
	} else if (amount == sizeof(uint32_t) * 2) {
		memcpy(buf, frame->header + 16, (size_t)amount);
	} else if (amount == (int)page_size) {
		memcpy(buf, frame->page, (size_t)amount);
	} else {
		memcpy(buf, frame->header, FORMAT__WAL_FRAME_HDR_SIZE);
		memcpy(buf + FORMAT__WAL_FRAME_HDR_SIZE, frame->page,
		       page_size);
	}

	return SQLITE_OK;
}

static int vfsWalFileWrite(sqlite3_file* file, const void* buf, int amount, sqlite3_int64 offset)
{
	struct vfsWalFile *f = (struct vfsWalFile *)file;
	uint32_t page_size;
	unsigned index;
	struct vfsFrame *frame;

	/* WAL header. */
	if (offset == 0) {
		/* We expect the data to contain exactly 32
		 * bytes. */
		assert(amount == VFS__WAL_HEADER_SIZE);

		memcpy(f->wal->hdr, buf, (size_t)amount);
		return SQLITE_OK;
	}

	page_size = vfsWalGetPageSize(f->wal);
	assert(page_size > 0);

	/* This is a WAL frame write. We expect either a frame
	 * header or page write. */
	if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
		/* Frame header write. */
		assert(((offset - VFS__WAL_HEADER_SIZE) %
			((int)page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);

		index =
		    (unsigned)formatWalCalcFrameIndex((int)page_size, offset);

		vfsWalFrameGet(f->wal, index, page_size, &frame);
		if (frame == NULL) {
			return SQLITE_NOMEM;
		}
		memcpy(frame->header, buf, (size_t)amount);
	} else {
		/* Frame page write. */
		assert(amount == (int)page_size);
		assert(((offset - VFS__WAL_HEADER_SIZE -
			 FORMAT__WAL_FRAME_HDR_SIZE) %
			((int)page_size + FORMAT__WAL_FRAME_HDR_SIZE)) == 0);

		index =
		    (unsigned)formatWalCalcFrameIndex((int)page_size, offset);

		/* The header for the this frame must already
		 * have been written, so the page is there. */
		frame = vfsWalFrameLookup(f->wal, index);

		assert(frame != NULL);

		memcpy(frame->page, buf, (size_t)amount);
	}

	return SQLITE_OK;
}

static int vfsWalFileTruncate(sqlite3_file* file, sqlite3_int64 size)
{
	struct vfsWalFile *f = (struct vfsWalFile *)file;

	/* We expect SQLite to only truncate to zero, after a
	 * full checkpoint. */
	if (size != 0) {
		return SQLITE_PROTOCOL;
	}

	formatWalRestartHeader(f->wal->hdr);
	vfsWalClose(f->wal);
	f->wal->frames = NULL;
	f->wal->n_frames = 0;
	f->wal->tx = NULL;
	f->wal->n_tx = 0;

	return SQLITE_OK;
}

static const sqlite3_io_methods vfsWalFileMethods = {
	.iVersion = 1,
	.xClose = vfsNoopClose,
	.xRead = vfsWalFileRead,
	.xWrite = vfsWalFileWrite,
	.xTruncate = vfsWalFileTruncate,
	.xSync = vfsNoopSync,
	.xFileSize = vfsWalFileSize,
	.xLock = vfsNoopLock,
	.xUnlock = vfsNoopUnlock,
	.xCheckReservedLock = vfsNoopCheckReservedLock,
	.xFileControl = vfsNoopFileControl,
	.xSectorSize = vfsNoopSectorSize,
	.xDeviceCharacteristics = vfsNoopDeviceCharacteristics,
};

/* Implementation of the abstract sqlite3_file base class.
 * for the main database file */
struct vfsMainFile {
	sqlite3_file base;            /* Base class. Must be first. */
	struct vfs *vfs;              /* Pointer to volatile VFS data. */
	struct vfsDatabase *database; /* Underlying database content. */
	uint16_t sharedMask;          /* Mask of shared locks held */
	uint16_t exclMask;            /* Mask of exclusive locks held */
	struct {
		void **ptr;
		int len, cap;
	} mappedShmRegions;
};

static int vfsMainFileRead(sqlite3_file *file,
			   void *buf,
			   int amount,
			   sqlite_int64 offset)
{
	struct vfsMainFile *f = (struct vfsMainFile *)file;

	int page_size;
	unsigned pgno;
	const char *page;

	if (f->database->n_pages == 0) {
		/* From SQLite docs:
		*
		*   If xRead() returns SQLITE_IOERR_SHORT_READ it must also fill
		*   in the unread portions of the buffer with zeros.  A VFS that
		*   fails to zero-fill short reads might seem to work.  However,
		*   failure to zero-fill short reads will eventually lead to
		*   database corruption.
		*/
		memset(buf, 0, (size_t)amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	/* If the main database file is not empty, we expect the
	 * page size to have been set by an initial write. */
	uint32_t page_size_u32 = vfsDatabaseGetPageSize(f->database);
	assert(page_size_u32 > 0 && page_size_u32 <= INT_MAX);
	page_size = (int)page_size_u32;

	if (offset < page_size) {
		/* Reading from page 1. We expect the read to be
		 * at most page_size bytes. */
		assert(amount <= page_size);
		pgno = 1;
	} else {
		/* For pages greater than 1, we expect an offset
		 * that starts exactly at a page boundary. The read
		 * size can be less than a full page when SQLite
		 * is compiled with SQLITE_DIRECT_OVERFLOW_READ
		 * (enabled by default since 3.45.1). */
		assert(amount <= page_size);

		assert((offset % page_size) == 0);
		pgno = (unsigned)(offset / page_size) + 1;
	}

	assert(pgno > 0);

	page = vfsDatabasePageLookup(f->database, pgno);

	if (page == NULL) {
		/* From SQLite docs:
		 *
		 *   If xRead() returns SQLITE_IOERR_SHORT_READ it must also
		 *   fill in the unread portions of the buffer with zeros.  A VFS
		 *   that fails to zero-fill short reads might seem to work.
		 *   However, failure to zero-fill short reads will eventually
		 *   lead to database corruption.
		 */
		memset(buf, 0, (size_t)amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	memcpy(buf, pgno == 1 ? page + offset : page, (size_t)amount);
	return SQLITE_OK;
}

static int vfsMainFileWrite(sqlite3_file *file,
			    const void *buf,
			    int amount,
			    sqlite_int64 offset)
{
	struct vfsMainFile *f = (struct vfsMainFile *)file;

	assert(buf != NULL);
	assert(amount > 0);
	assert(f != NULL);

	unsigned pgno;
	uint32_t page_size;
	void *page;

	if (offset == 0) {
		const uint8_t *header = buf;

		/* This is the first database page. We expect
		 * the data to contain at least the header. */
		assert(amount >= FORMAT__DB_HDR_SIZE);

		/* Extract the page size from the header. */
		page_size = vfsParsePageSize(ByteGetBe16(&header[16]));
		if (page_size == 0) {
			return SQLITE_CORRUPT;
		}

		pgno = 1;
	} else {
		page_size = vfsDatabaseGetPageSize(f->database);

		/* The header must have been written and the page size set. */
		assert(page_size > 0);

		/* For pages beyond the first we expect offset to be a multiple
		 * of the page size. */
		assert((offset % (int)page_size) == 0);

		/* We expect that SQLite writes a page at time. */
		assert(amount == (int)page_size);

		pgno = ((unsigned)(offset / (int)page_size)) + 1;
	}

	int rv = vfsDatabaseGetPage(f->database, page_size, pgno, &page);
	if (rv != SQLITE_OK) {
		return rv;
	}

	assert(page != NULL);
	memcpy(page, buf, (size_t)amount);
	return SQLITE_OK;
}

static int vfsMainFileTruncate(sqlite3_file *file, sqlite_int64 size)
{
	struct vfsMainFile *f = (struct vfsMainFile *)file;
	return vfsDatabaseTruncate(f->database, size);
}

static int vfsMainFileSize(sqlite3_file *file, sqlite_int64 *size)
{
	struct vfsMainFile *f = (struct vfsMainFile *)file;
	*size = vfsDatabaseFileSize(f->database);
	return SQLITE_OK;
}

/* Handle pragma a pragma file control. See the xFileControl
 * docstring in sqlite.h.in for more details. */
static int vfsFileControlPragma(struct vfsMainFile *f, char **fcntl)
{
	const char *left;
	const char *right;

	assert(f != NULL);
	assert(fcntl != NULL);

	left = fcntl[1];
	right = fcntl[2];

	assert(left != NULL);

	if (sqlite3_stricmp(left, "page_size") == 0 && right) {
		/* When the user executes 'PRAGMA page_size=N' we save the
		 * size internally.
		 *
		 * The page size must be between 512 and 65536, and be a
		 * power of two. The check below was copied from
		 * sqlite3BtreeSetPageSize in btree.c.
		 *
		 * Invalid sizes are simply ignored, SQLite will do the same.
		 *
		 * It's not possible to change the size after it's set.
		 */
		int page_size = atoi(right);

		if (page_size >= FORMAT__PAGE_SIZE_MIN &&
		    page_size <= FORMAT__PAGE_SIZE_MAX &&
		    ((page_size - 1) & page_size) == 0) {
			if (f->database->n_pages > 0 &&
			    page_size !=
				(int)vfsDatabaseGetPageSize(f->database)) {
				fcntl[0] = sqlite3_mprintf(
				    "changing page size is not supported");
				return SQLITE_IOERR;
			}
		}
	}

	/* We're returning NOTFOUND here to tell SQLite that we wish it to go on
	 * with its own handling as well. If we returned SQLITE_OK the page size
	 * of the journal mode wouldn't be effectively set, as the processing of
	 * the PRAGMA would stop here. */
	return SQLITE_NOTFOUND;
}


static int vfsMainFileControl(sqlite3_file *file, int op, void *arg)
{
	struct vfsMainFile *f = (struct vfsMainFile *)file;

	switch (op) {
		case SQLITE_FCNTL_PRAGMA:
			return vfsFileControlPragma(f, arg);
		case SQLITE_FCNTL_COMMIT_PHASETWO:
			if (f->database->wal.n_tx > 0) {
				vfsAmendWalIndexHeader(f->database);
			}
			return SQLITE_OK;
		case SQLITE_FCNTL_PERSIST_WAL:
			/* This prevents SQLite from deleting the WAL after the
			 * last connection is closed. */
			*(int *)(arg) = 1;
			return SQLITE_OK;
		default:
			return SQLITE_OK;
	}
}

/* Simulate shared memory by allocating on the C heap. */
static int vfsMainFileShmMap(sqlite3_file *file, /* Handle open on database file */
			 int region_index,   /* Region to retrieve */
			 int region_size,    /* Size of regions */
			 int extend, /* True to extend file if necessary */
			 void volatile **out /* OUT: Mapped memory */
)
{
	const int offset = region_index * region_size;
	struct vfsMainFile *f = (struct vfsMainFile *)file;
	struct vfsShm *s = &f->database->shm;

	PRE(region_size == VFS__WAL_INDEX_REGION_SIZE);

	int rv;
	if (s->size < offset + region_size) {
		if (extend) {
			/* The file is not big enough */
			rv = ftruncate(s->fd, offset + region_size);
			if (rv < 0) {
				return SQLITE_NOMEM;
			}
			s->size = offset + region_size;
		} else {
			*out = NULL;
			return SQLITE_OK;
		}
	}

	while (f->mappedShmRegions.cap <= region_index) {
		int newCap = 2 * f->mappedShmRegions.cap + 1;
		void **newPtr = sqlite3_realloc64(
		    f->mappedShmRegions.ptr,
		    (sqlite3_uint64)sizeof(*f->mappedShmRegions.ptr) *
			(sqlite3_uint64)newCap);
		if (newPtr == NULL) {
			return SQLITE_NOMEM;
		}
		memset(newPtr + f->mappedShmRegions.cap, 0,
		       sizeof(void *) *
			   (size_t)(newCap - f->mappedShmRegions.cap));
		f->mappedShmRegions.cap = newCap;
		f->mappedShmRegions.ptr = newPtr;
	}

	if (region_index < f->mappedShmRegions.len) {
		*out = f->mappedShmRegions.ptr[region_index];
		return SQLITE_OK;
	}

	PRE(region_index == f->mappedShmRegions.len);

	void *region =
	    mmap(NULL, VFS__WAL_INDEX_REGION_SIZE, PROT_READ | PROT_WRITE,
		 MAP_SHARED, s->fd, region_index * region_size);
	if (region == MAP_FAILED) {
		return SQLITE_IOERR_SHMMAP;
	}

	f->mappedShmRegions.len = region_index + 1;
	f->mappedShmRegions.ptr[region_index] = region;
	*out = region;
	return SQLITE_OK;
}

/* If there's a uncommitted transaction, roll it back. */
static void vfsWalRollbackIfUncommitted(struct vfsWal *w)
{
	struct vfsFrame *last;
	uint32_t commit;
	unsigned i;

	if (w->n_tx == 0) {
		return;
	}

	tracef("rollback n_tx:%d", w->n_tx);
	last = w->tx[w->n_tx - 1];
	commit = vfsFrameGetDatabaseSize(last);

	if (commit > 0) {
		tracef("rollback commit:%u", commit);
		return;
	}

	for (i = 0; i < w->n_tx; i++) {
		vfsFrameDestroy(w->tx[i]);
	}

	w->n_tx = 0;
}

static int vfsMainFileShmLock(sqlite3_file *file, int ofst, int n, int flags)
{
	struct vfsMainFile *f = (struct vfsMainFile *)file;
	const uint16_t mask =
	    (uint16_t)((1 << (ofst + n)) -
		       (1 << ofst)); /* Mask of locks to take or release */

	/* Legal values for the offset and the range */
	PRE(n > 0);
	PRE(ofst >= 0 && ofst + n <= SQLITE_SHM_NLOCK);
	PRE(n == 1 || (flags & SQLITE_SHM_EXCLUSIVE) != 0);

	/* Legal values for the flags.
	 *
	 * See https://sqlite.org/c3ref/c_shm_exclusive.html. */
	PRE(flags == (SQLITE_SHM_LOCK | SQLITE_SHM_SHARED) ||
	    flags == (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE) ||
	    flags == (SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED) ||
	    flags == (SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE));

	PRE((f->exclMask & f->sharedMask) == 0);

	int rv = SQLITE_OK;
	if (
		((flags & SQLITE_SHM_UNLOCK) && ((f->exclMask | f->sharedMask) & mask)) ||
	    (flags == (SQLITE_SHM_SHARED | SQLITE_SHM_LOCK) && 0 == (f->sharedMask & mask)) ||
	    (flags == (SQLITE_SHM_EXCLUSIVE | SQLITE_SHM_LOCK))) {

		if (flags & SQLITE_SHM_UNLOCK) {
			PRE(!(flags & SQLITE_SHM_EXCLUSIVE) ||
			    (f->exclMask & mask) == mask);
			PRE(!(flags & SQLITE_SHM_SHARED) ||
			    (f->sharedMask & mask) == mask);

			rv = vfsShmUnlock(&f->database->shm, ofst, n,
					  flags & SQLITE_SHM_EXCLUSIVE);
			if (rv == SQLITE_OK) {
				f->exclMask &= ~mask;
				f->sharedMask &= ~mask;

				if (ofst == VFS__WAL_WRITE_LOCK &&
				    flags & SQLITE_SHM_EXCLUSIVE) {
					/* When releasing the write lock, if we
					 * find a pending uncommitted
					 * transaction then a rollback must have
					 * occurred. In that case we delete the
					 * pending transaction. */
					tracef("ROLLBACK");
					vfsWalRollbackIfUncommitted(
					    &f->database->wal);
				}
			}
		} else if (flags & SQLITE_SHM_SHARED) {
			rv = vfsShmLock(&f->database->shm, ofst, n, false);
			if (rv == SQLITE_OK) {
				f->sharedMask |= mask;
			}
		} else {
			PRE(flags & SQLITE_SHM_EXCLUSIVE);
			PRE((f->sharedMask & mask) == 0);
			PRE((f->exclMask & mask) == 0);

			/* When acquiring a write lock, make sure there's no
			 * transaction that hasn't been rolled back or polled.
			 * If there is, just return SQLITE_BUSY. */
			// FIXME: this check is strange because we need to
			// support a weird way to check for checkpointing.
			if (ofst == VFS__WAL_WRITE_LOCK &&
			    f->database->wal.n_tx > 0) {
				rv = SQLITE_BUSY;
			} else {
				rv = vfsShmLock(&f->database->shm, ofst, n,
						true);
				if (rv == SQLITE_OK) {
					f->exclMask |= mask;
				}
			}
		}

		/* This code makes sure that when taking a lock no transaction
		 * is pending. */
		if (rv == SQLITE_OK && ofst == VFS__WAL_WRITE_LOCK) {
			assert(n == 1);
			if (flags == (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE)) {
				assert(f->database->wal.n_tx == 0);
			}
		}
	}
	POST((f->exclMask & f->sharedMask) == 0);
	return rv;
}

static void vfsMainFileShmBarrier(sqlite3_file *file)
{
	(void)file;
	/* This is a no-op since we expect SQLite to be compiled with mutex
	 * support (i.e. SQLITE_MUTEX_OMIT or SQLITE_MUTEX_NOOP are *not*
	 * defined, see sqliteInt.h). */
}

static int vfsMainFileShmUnmap(sqlite3_file *file, int delete_flag)
{
	(void)delete_flag;
	struct vfsMainFile *f = (struct vfsMainFile *)file;
	for (int i = 0; i < f->mappedShmRegions.len; i++) {
		if (f->mappedShmRegions.ptr[i] != NULL) {
			int rv = munmap(f->mappedShmRegions.ptr[i], VFS__WAL_INDEX_REGION_SIZE);
			assert(rv == 0);
		}
	}
	sqlite3_free(f->mappedShmRegions.ptr);
	f->mappedShmRegions.ptr = NULL;
	f->mappedShmRegions.len = 0;
	f->mappedShmRegions.cap = 0;
	return SQLITE_OK;
}

static const sqlite3_io_methods vfsFileMethods = {
	.iVersion = 2,
	.xClose = vfsNoopClose,
	.xRead = vfsMainFileRead,
	.xWrite = vfsMainFileWrite,
	.xTruncate = vfsMainFileTruncate,
	.xSync = vfsNoopSync,
	.xFileSize = vfsMainFileSize,
	.xLock = vfsNoopLock,
	.xUnlock = vfsNoopUnlock,
	.xCheckReservedLock = vfsNoopCheckReservedLock,
	.xFileControl = vfsMainFileControl,
	.xSectorSize = vfsNoopSectorSize,
	.xDeviceCharacteristics = vfsNoopDeviceCharacteristics,
	.xShmMap = vfsMainFileShmMap,
	.xShmLock = vfsMainFileShmLock,
	.xShmBarrier = vfsMainFileShmBarrier,
	.xShmUnmap = vfsMainFileShmUnmap,
};

/* Implementation of the abstract sqlite3_file base class.
 * for the main database file */
struct vfsDiskMainFile
{
	struct vfsMainFile base;
	sqlite3_file *underlying;             /* On disk database file. */
};

static int vfsDiskFileClose(sqlite3_file *file)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;

	if (f->underlying != NULL) {
		int rc = f->underlying->pMethods->xClose(f->underlying);
		sqlite3_free(f->underlying);
		f->underlying = NULL;
		if (rc != SQLITE_OK) {
			return rc;
		}
	}

	return SQLITE_OK;
}

static int vfsDiskFileRead(sqlite3_file *file,
			   void *buf,
			   int amount,
			   sqlite_int64 offset)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xRead(f->underlying, buf, amount, offset);
}

/* Need to keep track of the number of database pages to allow creating correct
 * WAL headers when in on-disk mode. */
static int vfsDiskDatabaseTrackNumPages(struct vfsDatabase *d,
					sqlite_int64 offset)
{
	unsigned pgno;

	if (offset == 0) {
		pgno = 1;
	} else {
		assert(d->page_size != 0);
		if (d->page_size == 0) {
			return SQLITE_ERROR;
		}
		pgno = ((unsigned)offset / d->page_size) + 1;
	}

	if (pgno > d->n_pages) {
		d->n_pages = pgno;
	}

	return SQLITE_OK;
}

static int vfsDiskFileWrite(sqlite3_file *file,
			    const void *buf,
			    int amount,
			    sqlite_int64 offset)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;

	/* Write to the actual database file. */
	vfsDiskDatabaseTrackNumPages(f->base.database, offset);
	int rv = f->underlying->pMethods->xWrite(f->underlying, buf, amount, offset);
	tracef("vfsDiskFileWrite %s amount:%d rv:%d", "db", amount, rv);
	return rv;
}

static int vfsDiskFileTruncate(sqlite3_file *file, sqlite_int64 size)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xTruncate(f->underlying, size);
}

static int vfsDiskFileSync(sqlite3_file *file, int flags)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xSync(f->underlying, flags);
}

static int vfsDiskFileSize(sqlite3_file *file, sqlite_int64 *size)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xFileSize(f->underlying, size);
}

static int vfsDiskFileLock(sqlite3_file *file, int lock)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xLock(f->underlying, lock);
}

static int vfsDiskFileUnlock(sqlite3_file *file, int lock)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xUnlock(f->underlying, lock);
}

/* Handle pragma a pragma file control. See the xFileControl
 * docstring in sqlite.h.in for more details. */
static int vfsDiskFileControlPragma(struct vfsDiskMainFile *f, char **fcntl)
{
	int rv;
	const char *left;
	const char *right;

	assert(f != NULL);
	assert(fcntl != NULL);

	left = fcntl[1];
	right = fcntl[2];

	assert(left != NULL);

	if (strcmp(left, "page_size") == 0 && right) {
		int page_size = atoi(right);
		/* The first page_size pragma sets page_size member of the db
		 * and is called by dqlite based on the page_size configuration. */
		if (page_size > UINT16_MAX) {
			fcntl[0] = sqlite3_mprintf("max page_size exceeded");
			return SQLITE_IOERR;
		}
		if (f->base.database->page_size == 0) {
			rv = f->underlying->pMethods->xFileControl(
			    f->underlying, SQLITE_FCNTL_PRAGMA, fcntl);
			if (rv == SQLITE_NOTFOUND || rv == SQLITE_OK) {
				f->base.database->page_size = (uint16_t)page_size;
			}
			return rv;
		} else if ((uint16_t)page_size != f->base.database->page_size) {
			fcntl[0] = sqlite3_mprintf(
			    "changing page size is not supported");
			return SQLITE_IOERR;
		}
	}

	/* We're returning NOTFOUND here to tell SQLite that we wish it to go on
	 * with its own handling as well. If we returned SQLITE_OK the page size
	 * of the journal mode wouldn't be effectively set, as the processing of
	 * the PRAGMA would stop here. */
	return SQLITE_NOTFOUND;
}

static int vfsDiskFileControl(sqlite3_file *file, int op, void *arg)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	int rv;

	switch (op) {
		case SQLITE_FCNTL_PRAGMA:
			rv = vfsDiskFileControlPragma(f, arg);
			break;
		case SQLITE_FCNTL_COMMIT_PHASETWO:
			if (f->base.database->wal.n_tx > 0) {
				vfsAmendWalIndexHeader(f->base.database);
			}
			return SQLITE_OK;
		case SQLITE_FCNTL_PERSIST_WAL:
			/* This prevents SQLite from deleting the WAL after the
			 * last connection is closed. */
			*(int *)(arg) = 1;
			rv = SQLITE_OK;
			break;
		default:
			rv = SQLITE_OK;
			break;
	}

	return rv;
}

static int vfsDiskFileSectorSize(sqlite3_file *file)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xSectorSize(f->underlying);
}

static int vfsDiskFileDeviceCharacteristics(sqlite3_file *file)
{
	struct vfsDiskMainFile *f = (struct vfsDiskMainFile *)file;
	return f->underlying->pMethods->xDeviceCharacteristics(f->underlying);
}

static const sqlite3_io_methods vfsDiskFileMethods = {
	.iVersion = 2,
	.xClose = vfsDiskFileClose,
	.xRead = vfsDiskFileRead,
	.xWrite = vfsDiskFileWrite,
	.xTruncate = vfsDiskFileTruncate,
	.xSync = vfsDiskFileSync,
	.xFileSize = vfsDiskFileSize,
	.xLock = vfsDiskFileLock,
	.xUnlock = vfsDiskFileUnlock,
	.xCheckReservedLock = vfsNoopCheckReservedLock,
	.xFileControl = vfsDiskFileControl,
	.xSectorSize = vfsDiskFileSectorSize,
	.xDeviceCharacteristics = vfsDiskFileDeviceCharacteristics,
	.xShmMap = vfsMainFileShmMap,
	.xShmLock = vfsMainFileShmLock,
	.xShmBarrier = vfsMainFileShmBarrier,
	.xShmUnmap = vfsMainFileShmUnmap,
};

static int vfsOpen(sqlite3_vfs *vfs,
		   const char *filename,
		   sqlite3_file *file,
		   int flags,
		   int *out_flags)
{
	struct vfs *v = vfs->pAppData;
	struct vfsDatabase *database;
	int exclusive = flags & SQLITE_OPEN_EXCLUSIVE;
	int create = flags & SQLITE_OPEN_CREATE;

	(void)out_flags;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);
	assert(file != NULL);

	/* From sqlite3.h.in:
	 *
	 *   The SQLITE_OPEN_EXCLUSIVE flag is always used in conjunction with
	 *   the SQLITE_OPEN_CREATE flag, which are both directly analogous to
	 *   the O_EXCL and O_CREAT flags of the POSIX open() API.  The
	 *   SQLITE_OPEN_EXCLUSIVE flag, when paired with the
	 *   SQLITE_OPEN_CREATE, is used to indicate that file should always be
	 *   created, and that it is an error if it already exists.  It is not
	 *   used to indicate the file should be opened for exclusive access.
	 */
	assert(!exclusive || create);

	/* From SQLite documentation:
	 *
	 * If the zFilename parameter to xOpen is a NULL pointer then xOpen
	 * must invent its own temporary name for the file. Whenever the
	 * xFilename parameter is NULL it will also be the case that the
	 * flags parameter will include SQLITE_OPEN_DELETEONCLOSE.
	 */
	if (filename == NULL) {
		assert(flags & SQLITE_OPEN_DELETEONCLOSE);

		/* Open an actual temporary file. */
		vfs = sqlite3_vfs_find("unix");
		assert(vfs != NULL);
		return vfs->xOpen(vfs, NULL, file, flags, out_flags);
	} else if (flags & SQLITE_OPEN_MAIN_JOURNAL) {
		/* Journal file is just a noop file as only WAL mode is supported */
		file->pMethods = &vfsNoopMethods;
		return SQLITE_OK;
	}

	assert((flags & SQLITE_OPEN_DELETEONCLOSE) == 0);

	/* This tells SQLite to not call Close() in case we return an error. */
	file->pMethods = 0;

	/* Search if the database object exists already. */
	database = vfsDatabaseLookup(v, filename);

	if ((flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_WAL)) == 0) {
		v->error = ENOENT;
		return SQLITE_CANTOPEN;
	}

	/* If file exists, and the exclusive flag is on, return an error. */
	if (database != NULL && exclusive && create && (flags & SQLITE_OPEN_MAIN_DB)) {
		v->error = EEXIST;
		return SQLITE_CANTOPEN;
	}

	if (flags & SQLITE_OPEN_WAL) {
		/* When opening the WAL file we expect the main
		 * database file to have already been created. */
		if (database == NULL) {
			v->error = ENOENT;
			return SQLITE_CANTOPEN;
		}

		struct vfsWalFile *walFile = (struct vfsWalFile *)file;
		*walFile = (struct vfsWalFile){
			.base = {
				.pMethods = &vfsWalFileMethods,
			},
			.wal = &database->wal,
		};

		return SQLITE_OK;
	}

	assert(flags & SQLITE_OPEN_MAIN_DB);

	if (database == NULL) {
		if (!create) {
			v->error = ENOENT;
			return SQLITE_CANTOPEN;
		}

		database = vfsCreateDatabase(v, filename);
		if (database == NULL) {
			v->error = ENOMEM;
			return SQLITE_CANTOPEN;
		}
	}

	if (v->disk) {
		sqlite3_file *underlying = sqlite3_malloc(vfs->szOsFile);
		if (underlying == NULL) {
			return SQLITE_NOMEM;
		}

		int rc = v->base_vfs->xOpen(v->base_vfs, filename, underlying, flags, out_flags);
		if (rc != SQLITE_OK) {
			sqlite3_free(underlying);
			return rc;
		}
		*(struct vfsDiskMainFile *)file = (struct vfsDiskMainFile){
			.base = {
				.base = {
					.pMethods = &vfsDiskFileMethods,
				},
				.vfs = v,
				.database = database,
			},
			.underlying = underlying,
		};
	} else {
		*(struct vfsMainFile *)file = (struct vfsMainFile){
			.base = {
				.pMethods = &vfsFileMethods,
			},
			.vfs = v,
			.database = database,
		};
	}
	return SQLITE_OK;
}

static int vfsDelete(sqlite3_vfs *vfs, const char *filename, int dir_sync)
{
	(void)dir_sync;
	struct vfs *v = vfs->pAppData;
	assert(v != NULL);

	if (vfsFilenameEndsWith(filename, "-journal")) {
		return SQLITE_OK;
	}

	if (vfsFilenameEndsWith(filename, "-wal")) {
		return SQLITE_OK;
	}

	int rv = vfsDeleteDatabase(v, filename);
	if (rv != SQLITE_OK) {
		return rv;
	}

	if (v->disk) {
		rv = v->base_vfs->xDelete(v->base_vfs, filename, dir_sync);
		if (rv != SQLITE_OK) {
			return rv;
		}
	}

	return SQLITE_OK;
}

static int vfsAccess(sqlite3_vfs *vfs,
		     const char *filename,
		     int flags,
		     int *result)
{
	struct vfs *v;
	struct vfsDatabase *database;

	(void)flags;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);

	v = (struct vfs *)(vfs->pAppData);

	/* If the database object exists, we consider all associated files as
	 * existing and accessible. */
	database = vfsDatabaseLookup(v, filename);
	if (database == NULL) {
		*result = 0;
	} else if (v->disk) {
		if (vfsFilenameEndsWith(filename, "-journal")) {
			*result = 1;
		} else if (vfsFilenameEndsWith(filename, "-wal")) {
			*result = 1;
		} else {
			/* dqlite database object exists, now check if the regular
			* SQLite file exists. */
			return v->base_vfs->xAccess(vfs, filename, flags, result);
		}
	} else {
		*result = 1;
	}

	return SQLITE_OK;
}

static int vfsFullPathname(sqlite3_vfs *vfs,
			   const char *filename,
			   int pathname_len,
			   char *pathname)
{
	(void)vfs;

	/* Just return the path unchanged. */
	sqlite3_snprintf(pathname_len, pathname, "%s", filename);
	return SQLITE_OK;
}

static void *vfsDlOpen(sqlite3_vfs *vfs, const char *filename)
{
	(void)vfs;
	(void)filename;

	return 0;
}

static void vfsDlError(sqlite3_vfs *vfs, int nByte, char *zErrMsg)
{
	(void)vfs;

	sqlite3_snprintf(nByte, zErrMsg,
			 "Loadable extensions are not supported");
	zErrMsg[nByte - 1] = '\0';
}

static void (*vfsDlSym(sqlite3_vfs *vfs, void *pH, const char *z))(void)
{
	(void)vfs;
	(void)pH;
	(void)z;

	return 0;
}

static void vfsDlClose(sqlite3_vfs *vfs, void *pHandle)
{
	(void)vfs;
	(void)pHandle;

	return;
}

static int vfsRandomness(sqlite3_vfs *vfs, int nByte, char *zByte)
{
	(void)vfs;

	ssize_t rv = getrandom(zByte, (size_t)nByte, 0);
	if (rv == -1) {
		/* Ignore failed attempts */
		return 0;
	}
	return (int)rv;
}

static int vfsSleep(sqlite3_vfs *vfs, int microseconds)
{
	(void)vfs;

	/* TODO (is this needed?) */
	return microseconds;
}

static int vfsCurrentTimeInt64(sqlite3_vfs *vfs, sqlite3_int64 *piNow)
{
	static const sqlite3_int64 unixEpoch =
	    24405875 * (sqlite3_int64)8640000;
	struct timeval now;

	(void)vfs;

	gettimeofday(&now, 0);
	*piNow =
	    unixEpoch + 1000 * (sqlite3_int64)now.tv_sec + now.tv_usec / 1000;
	return SQLITE_OK;
}

static int vfsCurrentTime(sqlite3_vfs *vfs, double *piNow)
{
	sqlite3_int64 iNow;
	int rc = vfsCurrentTimeInt64(vfs, &iNow);
	if (rc == SQLITE_OK) {
		*piNow = ((double)iNow) / 86400000.0;
	}
	return rc;
}

static int vfsGetLastError(sqlite3_vfs *vfs, int x, char *y)
{
	struct vfs *v = (struct vfs *)(vfs->pAppData);
	int rc;

	(void)vfs;
	(void)x;
	(void)y;

	rc = v->error;

	return rc;
}

int VfsInit(struct sqlite3_vfs *vfs, const char *name)
{
	tracef("vfs init");

	vfs->iVersion = 2;
	vfs->mxPathname = VFS__MAX_PATHNAME;
	vfs->pNext = NULL;

	struct vfs *v = vfsCreate();
	if (v == NULL) {
		return DQLITE_NOMEM;
	}
	vfs->pAppData = v;
	vfs->szOsFile = MAX(sizeof(struct vfsMainFile), sizeof(struct vfsWalFile));
	if (vfs->szOsFile < v->base_vfs->szOsFile) {
		vfs->szOsFile = v->base_vfs->szOsFile;
	}
	vfs->xOpen = vfsOpen;
	vfs->xDelete = vfsDelete;
	vfs->xAccess = vfsAccess;
	vfs->xFullPathname = vfsFullPathname;
	vfs->xDlOpen = vfsDlOpen;
	vfs->xDlError = vfsDlError;
	vfs->xDlSym = vfsDlSym;
	vfs->xDlClose = vfsDlClose;
	vfs->xRandomness = vfsRandomness;
	vfs->xSleep = vfsSleep;
	vfs->xCurrentTime = vfsCurrentTime;
	vfs->xGetLastError = vfsGetLastError;
	vfs->xCurrentTimeInt64 = vfsCurrentTimeInt64;
	vfs->zName = name;

	return 0;
}

void VfsClose(struct sqlite3_vfs *vfs)
{
	tracef("vfs close");
	struct vfs *v = vfs->pAppData;
	vfsDestroy(v);
	sqlite3_free(v);
}

int VfsPoll(sqlite3 *conn, struct vfsTransaction *transaction)
{
	sqlite3_file *file;
	struct vfsMainFile *f;
	struct vfsFrame *last;
	uint32_t commit;
	unsigned i;
	int rv;

	rv = sqlite3_file_control(conn, NULL, SQLITE_FCNTL_FILE_POINTER, &file);
	assert(rv == SQLITE_OK);
	f = (struct vfsMainFile*)file;
	tracef("vfs poll filename:%s", f->database->name);

	if (f->database->wal.n_tx == 0) {
		*transaction = (struct vfsTransaction){};
		return SQLITE_OK;
	}

	/* Check if the last frame in the transaction has the commit marker. */
	last = f->database->wal.tx[f->database->wal.n_tx - 1];
	commit = vfsFrameGetDatabaseSize(last);

	if (commit == 0) {
		*transaction = (struct vfsTransaction){};
		return SQLITE_OK;
	}

	uint64_t *numbers = sqlite3_malloc64(sizeof(*numbers) * f->database->wal.n_tx);
	if (numbers == NULL) {
		return SQLITE_NOMEM;
	}

	void **pages = sqlite3_malloc64(sizeof(*pages) * f->database->wal.n_tx);
	if (pages == NULL) {
		sqlite3_free(numbers);
		return SQLITE_NOMEM;
	}


	for (i = 0; i < f->database->wal.n_tx; i++) {
		numbers[i] = vfsFrameGetPageNumber(f->database->wal.tx[i]);
		pages[i] = f->database->wal.tx[i]->page;
		/* Release the vfsFrame object, but not its buf attribute, since
		 * responsibility for that memory has been transferred to the
		 * caller. */
		sqlite3_free(f->database->wal.tx[i]);
	}
	sqlite3_free(f->database->wal.tx);
	*transaction = (struct vfsTransaction) {
		.n_pages     = f->database->wal.n_tx,
		.page_numbers = numbers,
		.pages   = pages,
	};
	f->database->wal.n_tx = 0;
	f->database->wal.tx = NULL;

	/* If some frames have been written take the write lock. */
	if (transaction->n_pages > 0) {
		rv = vfsShmLock(&f->database->shm, 0, 1, SQLITE_SHM_EXCLUSIVE);
		if (rv != 0) {
			tracef("shm lock failed %d", rv);
			return rv;
		}
		vfsAmendWalIndexHeader(f->database);
	}

	return SQLITE_OK;
}

/* Append the given pages as new frames. */
static int vfsWalAppend(struct vfsDatabase *d,
	const struct vfsTransaction *transaction)
{
	struct vfsWal *w = &d->wal;
	struct vfsFrame **frames; /* New frames array. */
	uint32_t page_size;
	uint32_t database_size;
	unsigned i;
	unsigned j;
	uint32_t salt[2];
	uint32_t checksum[2];

	/* No pending transactions. */
	assert(w->n_tx == 0);

	page_size = vfsWalGetPageSize(w);
	assert(page_size > 0);

	/* Get the salt from the WAL header. */
	salt[0] = vfsWalGetSalt1(w);
	salt[1] = vfsWalGetSalt2(w);

	/* If there are currently no frames in the WAL, the starting database
	 * size will be equal to the current number of pages in the main
	 * database, and the starting checksum should be set to the one stored
	 * in the WAL header. Otherwise, the starting database size and checksum
	 * will be the ones stored in the last frame of the WAL. */
	if (w->n_frames == 0) {
		database_size = d->n_pages;
		checksum[0] = vfsWalGetChecksum1(w);
		checksum[1] = vfsWalGetChecksum2(w);
	} else {
		struct vfsFrame *frame = w->frames[w->n_frames - 1];
		checksum[0] = vfsFrameGetChecksum1(frame);
		checksum[1] = vfsFrameGetChecksum2(frame);
		database_size = vfsFrameGetDatabaseSize(frame);
	}

	frames = sqlite3_realloc64(
	    w->frames, sizeof(*frames) * (w->n_frames + transaction->n_pages));
	if (frames == NULL) {
		goto oom;
	}
	w->frames = frames;

	for (i = 0; i < transaction->n_pages; i++) {
		struct vfsFrame *frame = vfsFrameCreate(page_size);
		uint32_t page_number = (uint32_t)transaction->page_numbers[i];
		uint32_t commit = 0;
		uint8_t *page = transaction->pages[i];

		if (frame == NULL) {
			goto oom_after_frames_alloc;
		}

		/* When writing the SQLite database header, make sure to sync
		 * the file size to the logical database size. */
		if (page_number == 1) {
			database_size = ByteGetBe32(&page[VFS__IN_HEADER_DATABASE_SIZE_OFFSET]);
		}

		/* For commit records, the size of the database file in pages
		 * after the commit. For all other records, zero. */
		if (i == transaction->n_pages - 1) {
			commit = database_size;
		}

		vfsFrameFill(frame, page_number, commit, salt, checksum, page,
			     page_size);

		frames[w->n_frames + i] = frame;
	}

	w->n_frames += transaction->n_pages;

	return 0;

oom_after_frames_alloc:
	for (j = 0; j < i; j++) {
		vfsFrameDestroy(frames[w->n_frames + j]);
	}
oom:
	return DQLITE_NOMEM;
}

/* Write the header of a brand new WAL file image. */
static void vfsWalStartHeader(struct vfsWal *w, uint32_t page_size)
{
	assert(page_size > 0);
	uint32_t checksum[2] = {0, 0};
	/* SQLite calculates checksums for the WAL header and frames either
	 * using little endian or big endian byte order when adding up 32-bit
	 * words. The byte order that should be used is recorded in the WAL file
	 * header by setting the least significant bit of the magic value stored
	 * in the first 32 bits. This allows portability of the WAL file across
	 * hosts with different native byte order.
	 *
	 * When creating a brand new WAL file, SQLite will set the byte order
	 * bit to match the host's native byte order, so checksums are a bit
	 * more efficient.
	 *
	 * In Dqlite the WAL file image is always generated at run time on the
	 * host, so we can always use the native byte order. */
	BytePutBe32(VFS__WAL_MAGIC | VFS__BIGENDIAN, &w->hdr[0]);
	BytePutBe32(VFS__WAL_VERSION, &w->hdr[4]);
	BytePutBe32(page_size, &w->hdr[8]);
	BytePutBe32(0, &w->hdr[12]);
	sqlite3_randomness(8, &w->hdr[16]);
	vfsChecksum(w->hdr, 24, checksum, checksum);
	BytePutBe32(checksum[0], w->hdr + 24);
	BytePutBe32(checksum[1], w->hdr + 28);
}

/* Invalidate the WAL index header, forcing the next connection that tries to
 * start a read transaction to rebuild the WAL index by reading the WAL.
 *
 * No read or write lock must be currently held. */
static void vfsInvalidateWalIndexHeader(struct vfsDatabase *d)
{
	struct vfsShm *shm = &d->shm;

	PRE(shm->lock[VFS__WAL_WRITE_LOCK] >= 0);
	PRE(shm->lock[VFS__WAL_CKPT_LOCK] >= 0);
	PRE(shm->lock[VFS__WAL_RECOVER_LOCK] >= 0);
	PRE(shm->size >= VFS__WAL_INDEX_HEADER_SIZE * 2);

	/* The walIndexTryHdr function in sqlite/wal.c (which is indirectly
	 * called by sqlite3WalBeginReadTransaction), compares the first and
	 * second copy of the WAL index header to see if it is valid. Changing
	 * the first byte of each of the two copies is enough to make the check
	 * fail. */
	uint8_t buffer[1] = { 1 };
	ssize_t rv = pwrite(shm->fd, buffer, 1, 0);
	assert(rv == 1);

	buffer[0] = 0;
	rv = pwrite(shm->fd, buffer, 1, VFS__WAL_INDEX_HEADER_SIZE);
	assert(rv == 1);
}

int VfsApply(sqlite3 *conn, const struct vfsTransaction *transaction)
{
	sqlite3_file *file;
	struct vfsMainFile *f;
	int rv;

	rv = sqlite3_file_control(conn, NULL, SQLITE_FCNTL_FILE_POINTER, &file);
	assert(rv == SQLITE_OK);
	f = (struct vfsMainFile*)file;
	tracef("vfs apply on %s %u pages", f->database->name, transaction->n_pages);


	/* If there's no page size set in the WAL header, it must mean that WAL
	 * file was never written. In that case we need to initialize the WAL
	 * header. */
	if (vfsWalGetPageSize(&f->database->wal) == 0) {
		vfsWalStartHeader(&f->database->wal, vfsDatabaseGetPageSize(f->database));
	}

	rv = vfsWalAppend(f->database, transaction);
	if (rv != 0) {
		tracef("wal append failed rv:%d n_pages:%u n:%u", rv,
		       f->database->n_pages, transaction->n_pages);
		return rv;
	}

	/* If a write lock is held it means that this is the VFS that orginated
	 * this commit and on which dqlite_vfs_poll() was called. In that case
	 * we release the lock and update the WAL index.
	 *
	 * Otherwise, if the WAL index header is mapped it means that this VFS
	 * has one or more open connections even if it's not the one that
	 * originated the transaction (this can happen for example when applying
	 * a Raft barrier and replaying the Raft log in order to serve a request
	 * of a newly connected client). */
	if (f->database->shm.lock[0] < 0) {
		f->database->shm.lock[0] = 0;
		vfsAmendWalIndexHeader(f->database);
	} else {
		if (f->database->shm.size > 0) {
			vfsInvalidateWalIndexHeader(f->database);
		}
	}

	return 0;
}

int VfsAbort(sqlite3 *conn)
{
	sqlite3_file *file;
	int rv = sqlite3_file_control(conn, NULL, SQLITE_FCNTL_FILE_POINTER, &file);
	assert(rv == SQLITE_OK);
	struct vfsMainFile *f = (struct vfsMainFile*)file;

	rv = vfsShmUnlock(&f->database->shm, 0, 1, SQLITE_SHM_EXCLUSIVE);
	if (rv != SQLITE_OK) {
		tracef("shm unlock failed %d", rv);
		return rv;
	}

	return SQLITE_OK;
}

/* Extract the number of pages field from the database header. */
static uint32_t vfsDatabaseGetNumberOfPages(struct vfsDatabase *d)
{
	if (d->n_pages == 0) {
		return 0;
	}

	/* The page size is stored in the 16th and 17th bytes of the first
	 * database page (big-endian) */
	uint8_t *page = d->pages[0];
	return ByteGetBe32(&page[VFS__IN_HEADER_DATABASE_SIZE_OFFSET]);
}

static uint32_t vfsDatabaseNumPages(struct vfsDatabase *database, bool use_wal) {
	uint32_t n;
	if (use_wal && database->wal.n_frames > 0) {
		n = vfsFrameGetDatabaseSize(database->wal.frames[database->wal.n_frames-1]);
		/* If the result is zero, it means that the WAL contains
		 * uncommitted transactions. */
		POST(n > 0);
	} else {
		n = vfsDatabaseGetNumberOfPages(database);
	}
	return n;
}

int VfsDatabaseNumPages(sqlite3_vfs *vfs,
			const char *filename,
			bool use_wal,
			uint32_t *n)
{
	struct vfs *v;
	struct vfsDatabase *d;

	v = (struct vfs *)(vfs->pAppData);
	d = vfsDatabaseLookup(v, filename);
	if (d == NULL) {
		return -1;
	}

	*n = vfsDatabaseNumPages(d, use_wal);
	return 0;
}


static void vfsDatabaseSnapshot(struct vfsDatabase *d, uint8_t **cursor)
{
	uint32_t page_size;
	unsigned i;

	page_size = vfsDatabaseGetPageSize(d);
	assert(page_size > 0);
	assert(d->n_pages == vfsDatabaseGetNumberOfPages(d));

	for (i = 0; i < d->n_pages; i++) {
		memcpy(*cursor, d->pages[i], page_size);
		*cursor += page_size;
	}
}

static void vfsWalSnapshot(struct vfsWal *w, uint8_t **cursor)
{
	uint32_t page_size;
	unsigned i;

	if (w->n_frames == 0) {
		return;
	}

	memcpy(*cursor, w->hdr, VFS__WAL_HEADER_SIZE);
	*cursor += VFS__WAL_HEADER_SIZE;

	page_size = vfsWalGetPageSize(w);
	assert(page_size > 0);

	for (i = 0; i < w->n_frames; i++) {
		struct vfsFrame *frame = w->frames[i];
		memcpy(*cursor, frame->header, FORMAT__WAL_FRAME_HDR_SIZE);
		*cursor += FORMAT__WAL_FRAME_HDR_SIZE;
		memcpy(*cursor, frame->page, page_size);
		*cursor += page_size;
	}
}

int VfsSnapshot(sqlite3_vfs *vfs, const char *filename, void **data, size_t *n)
{
	tracef("vfs snapshot filename %s", filename);
	struct vfs *v;
	struct vfsDatabase *database;
	struct vfsWal *wal;
	uint8_t *cursor;

	v = (struct vfs *)(vfs->pAppData);
	database = vfsDatabaseLookup(v, filename);

	if (database == NULL) {
		tracef("not found");
		*data = NULL;
		*n = 0;
		return 0;
	}

	if (database->n_pages != vfsDatabaseGetNumberOfPages(database)) {
		tracef("corrupt");
		return SQLITE_CORRUPT;
	}

	wal = &database->wal;

	*n = (size_t)(vfsDatabaseFileSize(database) + vfsWalSize(wal));
	/* TODO: we should fix the tests and use sqlite3_malloc instead. */
	*data = raft_malloc(*n);
	if (*data == NULL) {
		tracef("malloc");
		return DQLITE_NOMEM;
	}

	cursor = *data;

	vfsDatabaseSnapshot(database, &cursor);
	vfsWalSnapshot(wal, &cursor);

	return 0;
}

static void vfsDatabaseShallowSnapshot(struct vfsDatabase *d,
				       struct dqlite_buffer *bufs, uint32_t n)
{
	uint32_t page_size;

	page_size = vfsDatabaseGetPageSize(d);
	assert(page_size > 0);

	if (d->n_pages < n) {
		n = d->n_pages;
	}

	/* Fill the buffers with pointers to all of the database pages */
	for (unsigned i = 0; i < n; ++i) {
		bufs[i].base = d->pages[i];
		bufs[i].len = page_size;
	}
}

static void vfsWalShallowSnapshot(struct vfsWal *w,
				  struct dqlite_buffer *bufs,
				  uint32_t n) {
	uint32_t page_size;
	unsigned i;

	if (w->n_frames == 0) {
		return;
	}

	page_size = vfsWalGetPageSize(w);
	assert(page_size > 0);

	for (i = 0; i < w->n_frames; i++) {
		struct vfsFrame *frame = w->frames[i];
		uint32_t page_number = vfsFrameGetPageNumber(frame);
		assert(page_number <= n);
		bufs[page_number - 1].base = frame->page;
		bufs[page_number - 1].len = page_size;
	}
}

int VfsShallowSnapshot(sqlite3_vfs *vfs,
		       const char *filename,
		       struct dqlite_buffer bufs[],
		       uint32_t n)
{
	tracef("vfs snapshot filename %s", filename);
	struct vfs *v;
	struct vfsDatabase *database;

	v = (struct vfs *)(vfs->pAppData);
	database = vfsDatabaseLookup(v, filename);

	if (database == NULL) {
		tracef("not found");
		return -1;
	}

	if (database->n_pages != vfsDatabaseGetNumberOfPages(database)) {
		tracef("corrupt");
		return SQLITE_CORRUPT;
	}

	if (vfsDatabaseNumPages(database, true) != n) {
		tracef("not enough buffers provided");
		return SQLITE_MISUSE;
	}

	vfsDatabaseShallowSnapshot(database, bufs, n);
	/* Update the bufs array with newer versions of pages from the WAL. */
	vfsWalShallowSnapshot(&database->wal, bufs, n);

	return 0;
}

static int vfsDatabaseRestore(struct vfsDatabase *d,
			      const uint8_t *data,
			      size_t n)
{
	uint32_t page_size = vfsParsePageSize(ByteGetBe16(&data[16]));
	unsigned n_pages;
	void **pages;
	unsigned i;
	size_t offset;
	int rv;

	assert(page_size > 0);

	/* Check that the page size of the snapshot is consistent with what we
	 * have here. */
	assert(vfsDatabaseGetPageSize(d) == page_size);

	n_pages = (unsigned)ByteGetBe32(&data[28]);

	if (n < (uint64_t)n_pages * (uint64_t)page_size) {
		return DQLITE_ERROR;
	}

	pages = sqlite3_malloc64(sizeof *pages * n_pages);
	if (pages == NULL) {
		goto oom;
	}

	for (i = 0; i < n_pages; i++) {
		void *page = sqlite3_malloc64(page_size);
		if (page == NULL) {
			unsigned j;
			for (j = 0; j < i; j++) {
				sqlite3_free(pages[j]);
			}
			goto oom_after_pages_alloc;
		}
		pages[i] = page;
		offset = (size_t)i * (size_t)page_size;
		memcpy(page, &data[offset], page_size);
	}

	/* Truncate any existing content. */
	rv = vfsDatabaseTruncate(d, 0);
	assert(rv == 0);

	d->pages = pages;
	d->n_pages = n_pages;

	return 0;

oom_after_pages_alloc:
	sqlite3_free(pages);
oom:
	return DQLITE_NOMEM;
}

static int vfsWalRestore(struct vfsWal *w,
			 const uint8_t *data,
			 size_t n,
			 uint32_t page_size)
{
	struct vfsFrame **frames;
	unsigned n_frames;
	unsigned i;
	size_t offset;

	if (n == 0) {
		return SQLITE_OK;
	}

	assert(w->n_frames == 0);
	assert(w->n_tx == 0);

	assert(n > VFS__WAL_HEADER_SIZE);
	assert(((n - (size_t)VFS__WAL_HEADER_SIZE) %
		((size_t)vfsFrameSize(page_size))) == 0);

	n_frames = (unsigned)((n - (size_t)VFS__WAL_HEADER_SIZE) /
			      ((size_t)vfsFrameSize(page_size)));

	frames = sqlite3_malloc64(sizeof(*frames) * n_frames);
	if (frames == NULL) {
		goto oom;
	}

	memcpy(w->hdr, data, VFS__WAL_HEADER_SIZE);
	for (i = 0; i < n_frames; i++) {
		struct vfsFrame *frame = vfsFrameCreate(page_size);
		const uint8_t *p;

		if (frame == NULL) {
			unsigned j;
			for (j = 0; j < i; j++) {
				vfsFrameDestroy(frames[j]);
			}
			goto oom_after_frames_alloc;
		}
		frames[i] = frame;

		offset = (size_t)VFS__WAL_HEADER_SIZE +
			 ((size_t)i * (size_t)vfsFrameSize(page_size));
		p = &data[offset];
		memcpy(frame->header, p, VFS__FRAME_HEADER_SIZE);
		memcpy(frame->page, p + VFS__FRAME_HEADER_SIZE, page_size);
	}

	w->frames = frames;
	w->n_frames = n_frames;

	return SQLITE_OK;

oom_after_frames_alloc:
	sqlite3_free(frames);
oom:
	return DQLITE_NOMEM;
}

int VfsRestore(sqlite3_vfs *vfs,
	       const char *filename,
	       const void *data,
	       size_t n)
{
	tracef("vfs restore filename %s size %zd", filename, n);
	struct vfs *v = vfs->pAppData;
	struct vfsDatabase *database;
	uint32_t page_size;
	size_t offset;
	int rv = SQLITE_OK;

	database = vfsDatabaseLookup(v, filename);
	assert(database != NULL);

	/* Lock the database. The locking scheme here is similar to the one used
	 * when transitioning from WAL to DELETE mode. The WAL-Index recovery is
	 * not enough as it lets connections read the content of the main file.
	 * This means that:
	 *  - all WAL-Index locks must be held exclusively, including READ(0).
	 *  - an exclusive lock must be held on the main file.
	 * Given that only WAL mode is supported, a lock on the database object
	 * should be enough to emulate the file lock. */
	rv = vfsShmLock(&database->shm, 0, SQLITE_SHM_NLOCK, true);
	if (rv != SQLITE_OK) {
		goto err_locked;
	}

	/* Restore the content of the main database and of the WAL. */
	rv = vfsDatabaseRestore(database, data, n);
	if (rv != SQLITE_OK) {
		tracef("database restore failed %d", rv);
		goto err_locked;
	}

	rv = ftruncate(database->shm.fd, 0);
	assert(rv == 0);
	database->shm.size = 0;

	vfsWalClose(&database->wal);
	vfsWalInit(&database->wal);

	page_size = vfsDatabaseGetPageSize(database);
	offset = (size_t)database->n_pages * (size_t)page_size;
	rv = vfsWalRestore(&database->wal, data + offset, n - offset, page_size);
	if (rv != 0) {
		/* FIXME: the issue here is that the logic was able to restore
		 * the database, but not the WAL. This results in corrupted
		 * reads as the pages in the WAL might refer to a different
		 * version of the database. I wonder if it would make sense to
		 * at least reset the WAL so that it is still possible to read
		 * an old version of the database. Regardless, with the new
		 * "shallow" version of the snapshot mechanism, this is not a
		 * problem as the WAL segment is always empty. */
		tracef("wal restore failed %d", rv);
		goto err_locked;
	}
err_locked:
	vfsShmUnlock(&database->shm, 0, SQLITE_SHM_NLOCK, true);
	return rv;
}

int VfsEnableDisk(struct sqlite3_vfs *vfs)
{
	if (vfs->pAppData == NULL) {
		return -1;
	}

	struct vfs *v = vfs->pAppData;
	v->disk = true;

	return 0;
}

int VfsDiskSnapshotWal(sqlite3_vfs *vfs,
		       const char *path,
		       struct dqlite_buffer *buf)
{
	struct vfs *v;
	struct vfsDatabase *database;
	struct vfsWal *wal;
	uint8_t *cursor;
	int rv;

	v = (struct vfs *)(vfs->pAppData);
	database = vfsDatabaseLookup(v, path);

	if (database == NULL) {
		tracef("not found");
		rv = SQLITE_NOTFOUND;
		goto err;
	}

	/* Copy WAL to last buffer. */
	wal = &database->wal;
	buf->len = (size_t)vfsWalSize(wal);
	buf->base = sqlite3_malloc64(buf->len);
	/* WAL can have 0 length! */
	if (buf->base == NULL && buf->len != 0) {
		rv = SQLITE_NOMEM;
		goto err;
	}
	cursor = buf->base;
	vfsWalSnapshot(wal, &cursor);

	return 0;

err:
	return rv;
}

int VfsDiskSnapshotDb(sqlite3_vfs *vfs,
		      const char *path,
		      struct dqlite_buffer *buf)
{
	struct vfs *v;
	struct vfsDatabase *database;
	int fd;
	int rv;
	char *addr;
	struct stat sb;

	v = (struct vfs *)(vfs->pAppData);
	database = vfsDatabaseLookup(v, path);

	if (database == NULL) {
		tracef("not found");
		rv = SQLITE_NOTFOUND;
		goto err;
	}

	/* mmap the database file */
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		tracef("failed to open %s", path);
		rv = SQLITE_IOERR;
		goto err;
	}

	rv = fstat(fd, &sb);
	if (rv == -1) {
		tracef("fstat failed path:%s fd:%d", path, fd);
		close(fd);
		rv = SQLITE_IOERR;
		goto err;
	}

	/* TODO database size limited to whatever fits in a size_t. Multiple
	 * mmap's needed. This limitation also exists in various other places
	 * throughout the codebase. */
	addr = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED) {
		rv = SQLITE_IOERR;
		goto err;
	}

	buf->base = addr;
	buf->len = (size_t)sb.st_size;

	return 0;

err:
	return rv;
}

int VfsSnapshotDisk(sqlite3_vfs *vfs,
		    const char *filename,
		    struct dqlite_buffer bufs[],
		    unsigned n)
{
	int rv;
	if (n != 2) {
		return -1;
	}

	rv = VfsDiskSnapshotDb(vfs, filename, &bufs[0]);
	if (rv != 0) {
		return rv;
	}

	rv = VfsDiskSnapshotWal(vfs, filename, &bufs[1]);
	return rv;
}

static int vfsDiskDatabaseRestore(struct vfsDatabase *d,
				  const char *filename,
				  const uint8_t *data,
				  size_t n)
{
	int rv = 0;
	int fd;
	ssize_t sz; /* rv of write */
	uint32_t page_size;
	unsigned n_pages;
	const uint8_t *cursor;
	size_t n_left; /* amount of data left to write */

	fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd == -1) {
		tracef("fopen failed filename:%s", filename);
		return -1;
	}

	n_left = n;
	cursor = data;
	while (n_left > 0) {
		sz = write(fd, cursor, n_left);
		/* sz == 0 should not be possible when writing a positive amount
		 * of bytes. */
		if (sz <= 0) {
			tracef("fwrite failed n:%zd sz:%zd errno:%d", n_left,
			       sz, errno);
			rv = DQLITE_ERROR;
			goto out;
		}
		n_left -= (size_t)sz;
		cursor += sz;
	}

	page_size = vfsParsePageSize(ByteGetBe16(&data[16]));
	assert(page_size > 0);
	/* Check that the page size of the snapshot is consistent with what we
	 * have here. */
	assert(vfsDatabaseGetPageSize(d) == page_size);

	n_pages = (unsigned)ByteGetBe32(&data[28]);
	d->n_pages = n_pages;
	d->page_size = page_size;

out:
	close(fd);
	return rv;
}

int VfsDiskRestore(sqlite3_vfs *vfs,
		   const char *path,
		   const void *data,
		   size_t main_size,
		   size_t wal_size)
{
	tracef("vfs restore path %s main_size %zd wal_size %zd", path,
	       main_size, wal_size);
	struct vfs *v = vfs->pAppData;
	struct vfsDatabase *database;
	uint32_t page_size;
	int rv;

	database = vfsDatabaseLookup(v, path);
	assert(database != NULL);

	/* Lock the database. The locking scheme here is similar to the one used
	 * when transitioning from WAL to DELETE mode. The WAL-Index recovery is
	 * not enough as it lets connections read the content of the main file.
	 * This means that:
	 *  - all WAL-Index locks must be held exclusively, including READ(0).
	 *  - a exclusive lock must be held on the main file.
	 * Given that only WAL mode is supported, a lock on the database object
	 * should be enough to emulate the file lock.
	 */
	rv = vfsShmLock(&database->shm, 0, SQLITE_SHM_NLOCK, true);
	if (rv != SQLITE_OK) {
		goto err_locked;
	}

	rv = vfsDiskDatabaseRestore(database, path, data, main_size);
	if (rv != 0) {
		tracef("database restore failed %d", rv);
		goto err_locked;
	}

	rv = ftruncate(database->shm.fd, 0);
	assert(rv == 0);
	database->shm.size = 0;

	vfsWalClose(&database->wal);
	vfsWalInit(&database->wal);

	page_size = vfsDatabaseGetPageSize(database);
	rv = vfsWalRestore(&database->wal, data + main_size, wal_size, page_size);
	if (rv != 0) {
		tracef("wal restore failed %d", rv);
		goto err_locked;
	}

err_locked:
	vfsShmUnlock(&database->shm, 0, SQLITE_SHM_NLOCK, true);
	return rv;
}

uint64_t VfsDatabaseSize(sqlite3_vfs *vfs,
			 const char *path,
			 unsigned n,
			 unsigned page_size)
{
	struct vfs *v;
	struct vfsDatabase *database;
	struct vfsWal *wal;
	uint64_t new_wal_size;

	v = (struct vfs *)(vfs->pAppData);
	database = vfsDatabaseLookup(v, path);
	assert(database != NULL);

	wal = &database->wal;
	new_wal_size = (uint64_t)vfsWalSize(wal);
	if (new_wal_size == 0) {
		new_wal_size += (uint64_t)VFS__WAL_HEADER_SIZE;
	}
	new_wal_size += (uint64_t)n * (uint64_t)vfsFrameSize(page_size);
	return (uint64_t)vfsDatabaseFileSize(database) + new_wal_size;
}

uint64_t VfsDatabaseSizeLimit(sqlite3_vfs *vfs)
{
	(void)vfs;
	return (uint64_t)SIZE_MAX;
}

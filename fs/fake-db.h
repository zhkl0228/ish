#ifndef FS_FAKEFS_API_H
#define FS_FAKEFS_API_H

#include <sqlite3.h>
#include "fs/fix_path.h"
#include "misc.h"

struct fakefs_db {
    sqlite3 *db;
    struct {
        sqlite3_stmt *begin_deferred;
        sqlite3_stmt *begin_immediate;
        sqlite3_stmt *commit;
        sqlite3_stmt *rollback;
        sqlite3_stmt *path_get_inode;
        sqlite3_stmt *path_read_stat;
        sqlite3_stmt *path_create_stat;
        sqlite3_stmt *path_create_path;
        sqlite3_stmt *inode_read_stat;
        sqlite3_stmt *inode_write_stat;
        sqlite3_stmt *path_link;
        sqlite3_stmt *path_unlink;
        sqlite3_stmt *path_rename;
        sqlite3_stmt *path_from_inode;
        sqlite3_stmt *try_cleanup_inode;
        sqlite3_stmt *orphan_note;
        sqlite3_stmt *orphan_clear;
    } stmt;
    sqlite3_mutex *lock;
    // path_read_stat_cached(): sqlite's mapping of the -shm file's header
    // (NULL: no cache), and what was read
    const volatile uint32_t *wal_index;
    struct fakefs_cache *cache;
};

// The tables of a fakefs made new (fakefsify, a fresh mount), at
// user_version 3: fake_db_init's migrations bring them up to date. Here, not
// in fake-db.c, which fakefsify does not link (it needs the kernel's printk).
static const char *const fakefs_schema =
    "create table meta (id integer unique default 0, db_inode integer);"
    "insert into meta (db_inode) values (0);"
    "create table stats (inode integer primary key, stat blob);"
    "create table paths (path blob primary key, inode integer references stats(inode));"
    "create index inode_to_path on paths (inode, path);"
    // no index is needed on stats, because the rows are ordered by the primary key
    "pragma user_version=3;";

int fake_db_init(struct fakefs_db *fs, const char *db_path, int root_fd);
int fake_db_deinit(struct fakefs_db *fs);

void db_begin_read(struct fakefs_db *fs);
void db_begin_write(struct fakefs_db *fs);
void db_commit(struct fakefs_db *fs);
void db_rollback(struct fakefs_db *fs);

bool db_exec(struct fakefs_db *fs, sqlite3_stmt *stmt);
void db_reset(struct fakefs_db *fs, sqlite3_stmt *stmt);
void db_exec_reset(struct fakefs_db *fs, sqlite3_stmt *stmt);

struct ish_stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
};

typedef uint64_t inode_t;

inode_t path_get_inode(struct fakefs_db *fs, const char *path);
bool path_read_stat(struct fakefs_db *fs, const char *path, struct ish_stat *stat, uint64_t *inode);
// path_read_stat() in a read transaction of its own, or with none when no
// connection (this process's or another's: the file provider) has committed
// since the same path was read: from the cache then. The database must be in
// WAL mode for it (fake_db_init).
bool path_read_stat_cached(struct fakefs_db *fs, const char *path, struct ish_stat *stat, uint64_t *inode);
inode_t path_create(struct fakefs_db *fs, const char *path, struct ish_stat *stat);

bool inode_read_stat_if_exist(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat);
void inode_read_stat_or_die(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat);
void inode_write_stat(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat);

void path_link(struct fakefs_db *fs, const char *src, const char *dst);
inode_t path_unlink(struct fakefs_db *fs, const char *path);
// Inside a write transaction: remember that `inode` may just have lost its
// last path, so a crash before its cleanup is caught at the next mount.
void inode_note_orphan(struct fakefs_db *fs, inode_t inode);
// After the cleanup of `inode` ran (whether or not it deleted the stats row).
void inode_clear_orphan(struct fakefs_db *fs, inode_t inode);
void path_rename(struct fakefs_db *fs, const char *src, const char *dst);

#endif

# SQLite

SQLite 3.54.1, the amalgamation: `sqlite3.c` and `sqlite3.h`, from
<https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>
(SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, as
<https://www.sqlite.org/download.html> gives it). Public domain.

fakefs's metadata database (`meta.db`: modes, owners, symlinks, device nodes and
inode numbers of the root's files) runs on this copy, not the system's. Apple's
libsqlite3 shrinks its purgeable page cache with a system call at every
statement reset, and fakefs resets one or more statements per guest file
operation. On a Mac, on a fakefs, `ls -lR`, `find` with `stat`, `rm -rf`,
`git status` and `cp -r` took 7-20% less time with this copy than with Apple's.
Its syncs are ordered as Apple's are (fs/fake-db.c's `barrier_fcntl()`).

meson.build builds it as `libsqlite3` and links it whole into libfakefs. So the
CLI, fakefsify, the app and its file provider all run this copy (the file
provider prepares and steps fakefs's statements too). The app's targets find
`sqlite3.h` here first (`HEADER_SEARCH_PATHS`). If they found the SDK's header,
its module would also link the system library in.

To update, replace both files from a newer amalgamation zip. Then note its version
and hash here.

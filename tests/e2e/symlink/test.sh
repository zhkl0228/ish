#!/bin/sh
# Opening a file through a symlink. 32-bit musl opens everything with
# O_LARGEFILE, which the x86 guest once read as O_NOFOLLOW: the open failed
# with ELOOP, and the dynamic loader could not load a library through its
# soname symlink (libz.so.1), so apk did not run.
echo through a symlink > target.txt
ln -sf target.txt link.txt
cat link.txt
ln -sf link.txt link2.txt
cat link2.txt

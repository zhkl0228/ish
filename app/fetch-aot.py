#!/usr/bin/env python3
"""Fetch the AOT images the ARM64 app links, and the root they were recorded on.

Run by the app's Build Meson (ARM64) phase, before the app links: the set
benchmark/aojit/app-images.lock.json names (publish_app_images.py), its objects
into build/aot/<set>/ (app/AOTImages.xcconfig links them from there), and the
root into deps/rootfs/arm64-root.tar.gz if there is none. Every file is checked
against the lock's sha256: a file that does not match fails the build, whatever
the server gave.

A root that is there already and is not the lock's (one made with
tools/build-rootfs.sh) is kept and the build warned: the images of its modules
that differ (musl, by build-id) then serve only what did not change.

Python 3.9 (Xcode's), the standard library only.
"""

import hashlib
import json
import lzma
import os
import shutil
import subprocess
import sys

SRCROOT = os.environ.get('SRCROOT') or os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
LOCK = os.path.join(SRCROOT, 'benchmark', 'aojit', 'app-images.lock.json')
XCCONFIG = os.path.join(SRCROOT, 'app', 'AOTImages.xcconfig')
CACHE = os.path.join(SRCROOT, 'build', 'aot')
ROOTFS = os.path.join(SRCROOT, 'deps', 'rootfs', 'arm64-root.tar.gz')


def fail(message):
    print('error: fetch-aot: ' + message, file=sys.stderr)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def download(url, path, sha, xz):
    """url into path (decompressed if xz), checked against sha before it takes path's place.

    By curl, which goes through a proxy as the shell's other tools do (an https:// one
    too, which urllib cannot)."""
    print('fetch-aot: %s' % url)
    tmp = path + '.part'
    fetched = tmp + '.xz' if xz else tmp
    got = subprocess.run(['curl', '-fsSL', '--retry', '3', '-o', fetched, url], capture_output=True, text=True)
    if got.returncode != 0:
        fail('%s: curl exited %d: %s' % (url, got.returncode, got.stderr.strip()))
    if xz:
        try:
            with lzma.open(fetched) as src, open(tmp, 'wb') as out:
                shutil.copyfileobj(src, out, 1 << 20)
        except lzma.LZMAError as e:
            fail('%s: %s' % (url, e))
        os.remove(fetched)
    got = sha256(tmp)
    if got != sha:
        os.remove(tmp)
        fail('%s has sha256 %s, the lock says %s' % (url, got, sha))
    os.replace(tmp, path)


def main():
    lock = json.load(open(LOCK))
    set_name = lock['set']
    xcconfig = open(XCCONFIG).read()
    if 'ISH_AOT_SET = %s\n' % set_name not in xcconfig:
        fail('app/AOTImages.xcconfig is not the set of benchmark/aojit/app-images.lock.json (%s): '
             'publish_app_images.py writes both' % set_name)
    base = '%s/%s/' % (lock['base_url'], set_name)

    # The objects: checked once when fetched, then known by size and mtime (a stamp), so that
    # a build does not hash a few hundred MB every time
    directory = os.path.join(CACHE, set_name)
    os.makedirs(directory, exist_ok=True)
    stamp_path = os.path.join(directory, '.verified')
    stamp = json.load(open(stamp_path)) if os.path.exists(stamp_path) else {}
    for name, image in sorted(lock['images'].items()):
        path = os.path.join(directory, image['object'])
        if os.path.exists(path):
            st = os.stat(path)
            if stamp.get(image['object']) == [image['sha256'], st.st_size, st.st_mtime_ns]:
                continue
            if sha256(path) != image['sha256']:
                os.remove(path)
        if not os.path.exists(path):
            download(base + image['object'] + '.xz', path, image['sha256'], xz=True)
        st = os.stat(path)
        stamp[image['object']] = [image['sha256'], st.st_size, st.st_mtime_ns]
        with open(stamp_path, 'w') as f:
            json.dump(stamp, f)
    # sets of other locks, no longer linked
    for other in os.listdir(CACHE):
        if other != set_name:
            shutil.rmtree(os.path.join(CACHE, other), ignore_errors=True)

    rootfs = lock['rootfs']
    if not os.path.exists(ROOTFS):
        os.makedirs(os.path.dirname(ROOTFS), exist_ok=True)
        download(base + rootfs['file'], ROOTFS, rootfs['sha256'], xz=False)
    elif os.path.getsize(ROOTFS) != rootfs['size'] or sha256(ROOTFS) != rootfs['sha256']:
        print('warning: fetch-aot: deps/rootfs/arm64-root.tar.gz is not the root the AOT images were '
              'recorded on (set %s): the images of its modules that differ serve only the blocks that '
              'did not change. Remove it to have that root fetched.' % set_name, file=sys.stderr)
    print('fetch-aot: set %s, %d images' % (set_name, len(lock['images'])))


if __name__ == '__main__':
    main()

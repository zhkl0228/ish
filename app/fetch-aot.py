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

import concurrent.futures
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


class FetchError(Exception):
    """A download that failed, or did not match the lock: raised in download's thread."""


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def download(url, path, sha, xz):
    """url into path (decompressed if xz), checked against sha before it takes path's place.

    By curl, which goes through a proxy as the shell's other tools do (an https:// one
    too, which urllib cannot). A transfer cut off is resumed where it stopped, whatever
    cut it: through a proxy, two of eleven at once ended with HTTP/2's PROTOCOL_ERROR
    (curl's 92), which --retry alone does not retry."""
    print('fetch-aot: %s' % url)
    tmp = path + '.part'
    fetched = tmp + '.xz' if xz else tmp
    got = subprocess.run(['curl', '-fsSL', '--retry', '5', '--retry-all-errors', '-C', '-', '-o', fetched, url],
                         capture_output=True, text=True)
    if got.returncode != 0:
        raise FetchError('%s: curl exited %d: %s' % (url, got.returncode, got.stderr.strip()))
    if xz:
        try:
            with lzma.open(fetched) as src, open(tmp, 'wb') as out:
                shutil.copyfileobj(src, out, 1 << 20)
        except lzma.LZMAError as e:
            raise FetchError('%s: %s' % (url, e))
        os.remove(fetched)
    got = sha256(tmp)
    if got != sha:
        os.remove(tmp)
        raise FetchError('%s has sha256 %s, the lock says %s' % (url, got, sha))
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

    def verified(image):
        st = os.stat(os.path.join(directory, image['object']))
        stamp[image['object']] = [image['sha256'], st.st_size, st.st_mtime_ns]
        with open(stamp_path, 'w') as f:
            json.dump(stamp, f)

    missing = []
    for name, image in sorted(lock['images'].items()):
        path = os.path.join(directory, image['object'])
        if os.path.exists(path):
            st = os.stat(path)
            if stamp.get(image['object']) == [image['sha256'], st.st_size, st.st_mtime_ns]:
                continue
            if sha256(path) != image['sha256']:
                os.remove(path)
        if os.path.exists(path):
            verified(image)
        else:
            missing.append(image)
    # The missing ones all at once, each over a connection of its own: one connection to the
    # server ran at about 200 KB/s, and three at once at as much each (2026-11-27), which
    # left a set's 91 MB, one after another, taking eight minutes
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(len(missing), 1)) as pool:
        downloads = {pool.submit(download, base + image['object'] + '.xz', os.path.join(directory, image['object']),
                                 image['sha256'], True): image for image in missing}
        for done in concurrent.futures.as_completed(downloads):
            try:
                done.result()
            except FetchError as e:
                errors.append(str(e))
                continue
            verified(downloads[done])
    if errors:
        fail('; '.join(errors))
    # sets of other locks, no longer linked
    for other in os.listdir(CACHE):
        if other != set_name:
            shutil.rmtree(os.path.join(CACHE, other), ignore_errors=True)

    rootfs = lock['rootfs']
    if not os.path.exists(ROOTFS):
        os.makedirs(os.path.dirname(ROOTFS), exist_ok=True)
        try:
            download(base + rootfs['file'], ROOTFS, rootfs['sha256'], xz=False)
        except FetchError as e:
            fail(str(e))
    elif os.path.getsize(ROOTFS) != rootfs['size'] or sha256(ROOTFS) != rootfs['sha256']:
        print('warning: fetch-aot: deps/rootfs/arm64-root.tar.gz is not the root the AOT images were '
              'recorded on (set %s): the images of its modules that differ serve only the blocks that '
              'did not change. Remove it to have that root fetched.' % set_name, file=sys.stderr)
    print('fetch-aot: set %s, %d images' % (set_name, len(lock['images'])))


if __name__ == '__main__':
    main()

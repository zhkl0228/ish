#!/usr/bin/env python3
"""Publish the app's AOT images (record_app_images.sh) as a set the app's build fetches.

usage: publish_app_images.py <images dir> <recording ish> <recording root> [--upload]

  images dir      benchmark/aojit/images/<abi>/app: aot_<name>_ios.o of every image of
                  images-app.json
  recording ish   the ish that recorded them (-Djit=true; its abi is the images')
  recording root  the root they were recorded on (record_app_images.sh's, a fakefs): the
                  versions of the packages and npm tools come from it
  --upload        upload the set to the server (as user ish-aot, the key AOT_SSH_KEY, by
                  default ~/.ssh/ish_aot_ci), keeping there only the set master's lock names
                  and this one, then write the files below; without it the set is only
                  staged (build/aot-publish/<set>) and the repository left as it is

A set is the root the app ships (deps/rootfs/arm64-root.tar.gz, which the images were
recorded on: musl and the rest are matched by build-id) and the images' iOS objects,
xz-compressed. Its name is the abi, the day and a hash of its files, so a set never
changes once published. Then, in the repository:

  benchmark/aojit/app-images.lock.json   the set: where it is, each file's sha256, the abi,
                                         the versions it was recorded on
  app/AOTImages.xcconfig                 the objects the app links (ISH_AOT_OBJECTS), in
                                         build/aot/<set>/, where app/fetch-aot.py puts them
  app/RootfsPatch.bundle/files/usr/local/share/ish-install/versions
                                         the versions of what the images serve: the npm
                                         tools', which ish-install installs, and the apk
                                         packages', of which every login shell names an
                                         older one installed (ish-install --outdated); the
                                         patch's version goes up when they change
"""

import datetime
import hashlib
import json
import lzma
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recording_inputs import recording_inputs  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LIST = os.path.join(REPO, 'benchmark', 'aojit', 'images-app.json')
LOCK = os.path.join(REPO, 'benchmark', 'aojit', 'app-images.lock.json')
XCCONFIG = os.path.join(REPO, 'app', 'AOTImages.xcconfig')
PATCH = os.path.join(REPO, 'app', 'RootfsPatch.bundle')
VERSIONS = 'files/usr/local/share/ish-install/versions'
ROOTFS = os.path.join(REPO, 'deps', 'rootfs', 'arm64-root.tar.gz')
STAGE = os.path.join(REPO, 'build', 'aot-publish')

BASE_URL = 'https://hk.gzmtx.cn/ish-aot'
HOST = 'ish-aot@hk.gzmtx.cn'
# The apk packages whose builds the images serve (musl under all of them; bash, busybox and
# libcrypto3 with the root, python3 from ish-install, openssh-client-default's ssh, git and
# github-cli's gh from apk)
APK = ['musl', 'nodejs', 'openjdk25-jdk', 'maven', 'bash', 'python3', 'busybox', 'openssh-client-default',
       'libcrypto3', 'git', 'github-cli']
# Of them, those of whose code an image is: maven is Java's, which the JVM runs (openjdk25-jdk's
# libjvm, whose image it is)
APK_SERVED = [p for p in APK if p != 'maven']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def guest(ish, root, script):
    out = subprocess.run([ish, '-f', root, '/bin/sh', '-c', script], check=True, capture_output=True, text=True)
    return out.stdout


def recorded_versions(ish, root, packages):
    """The apk packages' versions and the npm tools' in the recording root."""
    apk = {}
    package = None
    for line in guest(ish, root, 'cat /lib/apk/db/installed').splitlines():
        if line.startswith('P:'):
            package = line[2:]
        elif line.startswith('V:') and package in APK:
            apk[package] = line[2:]
    missing = [p for p in APK if p not in apk]
    if missing:
        raise SystemExit('the recording root has no %s installed' % ' '.join(missing))
    npm = {}
    prefixes = {p: None for p in packages['npm']}
    for prefix, pkgs in packages['npm_prefix'].items():
        for p in pkgs:
            prefixes[p] = prefix
    for package, prefix in prefixes.items():
        # npm -g installs into /usr/local (Alpine's npm), npm -g --prefix into the prefix
        modules = (prefix or '/usr/local') + '/lib/node_modules'
        npm[package] = json.loads(guest(ish, root, 'cat %s/%s/package.json' % (modules, package)))['version']
    return apk, npm


def abi_of(ish, root):
    out = subprocess.run([ish, '-f', root, '/bin/cat', '/proc/ish/jit'], check=True, capture_output=True, text=True,
                         env=dict(os.environ, ISH_JIT_PIC='1')).stdout
    return re.search(r'abi ([0-9a-f]{8})', out).group(1)


def master_set():
    """The set master's lock names, which the server keeps: builds of master fetch it. Master as it is now,
    fetched: a clone's origin/master is as it was when the clone was made, and another run may have committed
    since. Read from that, a run's upload deleted the set master had just been given (2026-11-26: a scheduled
    run cloned 31 seconds before a run by hand committed its set, and kept the set before)."""
    subprocess.run(['git', '-C', REPO, 'fetch', '-q', 'origin', 'master'], check=True)
    out = subprocess.run(['git', '-C', REPO, 'show', 'origin/master:benchmark/aojit/app-images.lock.json'],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return None  # no lock on master yet
    return json.loads(out.stdout)['set']


def write_versions(npm, apk):
    path = os.path.join(PATCH, VERSIONS)
    text = ('# The versions the app\'s AOT images were recorded on, whose builds they serve\n'
            '# (benchmark/aojit/app-images.lock.json): of the npm tools, which ish-install installs at\n'
            '# them, and of the apk packages of whose code an image is, an older one of which every\n'
            '# login shell names (ish-install --outdated). Written by publish_app_images.py.\n')
    text += ''.join('npm %s %s\n' % (p, v) for p, v in sorted(npm.items()))
    text += ''.join('apk %s %s\n' % (p, apk[p]) for p in sorted(APK_SERVED))
    old = open(path).read() if os.path.exists(path) else None
    if old == text:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)
    return True


def bump_patch_version():
    """The patch's version, so that the app applies it again (its files: the versions)."""
    path = os.path.join(PATCH, 'manifest.plist')
    s = open(path).read()
    m = re.search(r'(<key>version</key>\s*<integer>)(\d+)(</integer>)', s)
    s = s[:m.start(2)] + str(int(m.group(2)) + 1) + s[m.end(2):]
    with open(path, 'w') as f:
        f.write(s)
    return int(m.group(2)) + 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    upload = '--upload' in sys.argv[1:]
    if len(args) != 3:
        raise SystemExit(__doc__)
    images_dir, ish, root = args
    listing = json.load(open(LIST))
    names = sorted(listing['images'])

    objects = {}
    for name in names:
        path = os.path.join(images_dir, 'aot_%s_ios.o' % name)
        if not os.path.exists(path):
            raise SystemExit('no %s: record_app_images.sh made no image %s' % (path, name))
        objects[name] = path
    abi = abi_of(ish, root)
    apk, npm = recorded_versions(ish, root, listing['packages'])

    files = {'rootfs': {'file': 'arm64-root.tar.gz', 'sha256': sha256(ROOTFS), 'size': os.path.getsize(ROOTFS)}}
    images = {}
    for name, path in objects.items():
        images[name] = {'module': listing['images'][name]['module'], 'object': os.path.basename(path),
                        'sha256': sha256(path), 'size': os.path.getsize(path)}
    content = hashlib.sha256(json.dumps([files, images], sort_keys=True).encode()).hexdigest()[:8]
    set_name = '%s-%s-%s' % (abi, datetime.date.today().strftime('%Y%m%d'), content)

    stage = os.path.join(STAGE, set_name)
    shutil.rmtree(STAGE, ignore_errors=True)
    os.makedirs(stage)
    shutil.copyfile(ROOTFS, os.path.join(stage, files['rootfs']['file']))
    for name, path in objects.items():
        with open(path, 'rb') as src, lzma.open(os.path.join(stage, images[name]['object'] + '.xz'), 'wb') as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        images[name]['xz_size'] = os.path.getsize(os.path.join(stage, images[name]['object'] + '.xz'))

    lock = {
        '_comment': ['The AOT images the app links and the root they were recorded on, as',
                     'benchmark/aojit/publish_app_images.py published them: app/fetch-aot.py fetches',
                     'them in the app\'s build (base_url/set/<file>, objects xz-compressed) and checks',
                     'each sha256. Written by publish_app_images.py; do not edit.'],
        'base_url': BASE_URL,
        'set': set_name,
        'abi': abi,
        'recorded': {'apk': apk, 'npm': npm, 'inputs': recording_inputs()},
        'rootfs': files['rootfs'],
        'images': images,
    }
    with open(os.path.join(stage, 'lock.json'), 'w') as f:
        json.dump(lock, f, indent=2, sort_keys=True)

    if upload:
        key = os.environ.get('AOT_SSH_KEY', os.path.expanduser('~/.ssh/ish_aot_ci'))
        keep = master_set()
        cmd = ['rsync', '-a', '--delete', '-e', 'ssh -i %s -o IdentitiesOnly=yes -o BatchMode=yes' % key]
        if keep and keep != set_name:
            cmd += ['--filter', 'P %s/' % keep]
        subprocess.run(cmd + [STAGE + '/', HOST + ':'], check=True)
        for name in [files['rootfs']['file']] + [i['object'] + '.xz' for i in images.values()]:
            url = '%s/%s/%s' % (BASE_URL, set_name, name)
            head = subprocess.run(['curl', '-sfI', url], capture_output=True, text=True)
            if head.returncode != 0:
                raise SystemExit('uploaded, but %s is not served (curl exited %d)' % (url, head.returncode))
        print('uploaded %s; kept on the server: %s' % (set_name, ', '.join(filter(None, {keep, set_name}))))
    else:
        print('staged %s in %s (not uploaded: the repository is left as it is)' % (set_name, stage))
        return

    with open(LOCK, 'w') as f:
        json.dump(lock, f, indent=2, sort_keys=True)
        f.write('\n')
    with open(XCCONFIG, 'w') as f:
        f.write('// The AOT images the ARM64 app links: generated by benchmark/aojit/publish_app_images.py\n'
                '// from benchmark/aojit/app-images.lock.json, do not edit. app/fetch-aot.py fetches them\n'
                '// into build/aot/<set>/ in the build (Build Meson (ARM64)), before the app links.\n'
                'ISH_AOT_SET = %s\n' % set_name)
        f.write('ISH_AOT_OBJECTS =' + ''.join(' $(SRCROOT)/build/aot/$(ISH_AOT_SET)/%s' % images[n]['object']
                                              for n in names) + '\n')
    if write_versions(npm, apk):
        print('ish-install versions: %s; RootfsPatch version %d' % (
            ', '.join('%s %s' % i for i in sorted(npm.items()) + [(p, apk[p]) for p in sorted(APK_SERVED)]),
            bump_patch_version()))
    print('set %s (abi %s): %s' % (set_name, abi, ', '.join('%s %.1f MB (%.1f MB xz)' % (
        n, images[n]['size'] / 1e6, images[n]['xz_size'] / 1e6) for n in names)))


if __name__ == '__main__':
    main()

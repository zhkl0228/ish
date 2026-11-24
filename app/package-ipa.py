#!/usr/bin/env python3
"""Package an unsigned Xcode archive of the app as an ipa, and its dSYMs.

usage: app/package-ipa.py <archive.xcarchive> <out dir>

Writes <out dir>/iSH-ARM64-unsigned.ipa (Payload/<the app>) and
<out dir>/iSH-ARM64-dSYMs.zip (the archive's dSYMs), and prints the app's
version and build as version=... and build=..., which it also appends to
$GITHUB_OUTPUT when that is set (.github/workflows/ci.yml publishes them as a
release). The archive is xcodebuild archive's with CODE_SIGNING_ALLOWED=NO: to
install it, sign it with one's own developer account.
"""
import os
import plistlib
import stat
import sys
import zipfile


def add_tree(z, root, arc_root):
    """Add root to z as arc_root, a directory's files and its symlinks as
    such, with their modes (an executable stays one)."""
    paths = [root]
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            paths.append(os.path.join(dirpath, name))
    for path in paths:
        arc = os.path.join(arc_root, os.path.relpath(path, root)) if path != root else arc_root
        st = os.lstat(path)
        if stat.S_ISLNK(st.st_mode):
            info = zipfile.ZipInfo(arc)
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o755) << 16
            z.writestr(info, os.readlink(path))
        elif stat.S_ISDIR(st.st_mode):
            z.write(path, arc + '/')
        elif stat.S_ISREG(st.st_mode):
            z.write(path, arc, zipfile.ZIP_DEFLATED)
        else:
            raise SystemExit('%s: neither a file, a directory nor a symlink' % path)


def main():
    archive, out = sys.argv[1], sys.argv[2]
    apps_dir = os.path.join(archive, 'Products', 'Applications')
    apps = [a for a in os.listdir(apps_dir) if a.endswith('.app')]
    if len(apps) != 1:
        raise SystemExit('%s: %d apps, not one: %s' % (apps_dir, len(apps), apps))
    app = os.path.join(apps_dir, apps[0])
    with open(os.path.join(app, 'Info.plist'), 'rb') as f:
        info = plistlib.load(f)
    version, build = info['CFBundleShortVersionString'], info['CFBundleVersion']

    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(os.path.join(out, 'iSH-ARM64-unsigned.ipa'), 'w') as z:
        z.write(out, 'Payload/')
        add_tree(z, app, 'Payload/' + apps[0])
    dsyms = os.path.join(archive, 'dSYMs')
    with zipfile.ZipFile(os.path.join(out, 'iSH-ARM64-dSYMs.zip'), 'w') as z:
        for name in sorted(os.listdir(dsyms)):
            add_tree(z, os.path.join(dsyms, name), name)

    lines = 'version=%s\nbuild=%s\n' % (version, build)
    print(lines, end='')
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
            f.write(lines)


if __name__ == '__main__':
    main()

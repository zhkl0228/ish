#!/usr/bin/env python3
"""What the app's images are recorded from, as one hash: the workloads (guest/), the list of
images and packages (images-app.json), the recording scripts, and gen.py with what it uses.

publish_app_images.py writes it in the lock (recorded.inputs), and ci_app_images.sh records the
images again when it is another, as when ish's abi or a package's version is: Claude Code's
workload recorded with JavaScriptCore's JIT off, as the app came to run it, changed neither, and
the images were not recorded again until a run was forced.

The files are the repository's tracked ones under PATHS, hashed as they are in the work tree.

usage: benchmark/aojit/recording_inputs.py   (prints the hash)
"""
import hashlib
import os
import subprocess

PATHS = ['benchmark/aojit/guest', 'benchmark/aojit/images-app.json', 'benchmark/aojit/record_app_images.sh',
         'benchmark/aojit/record_images.sh', 'tools/jit_aot/record.sh', 'tools/jit_aot/gen.py',
         'tools/jit_aot/compact.py']
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))


def recording_inputs():
    listed = subprocess.run(['git', '-C', REPO, 'ls-files', '-z', '--'] + PATHS,
                            capture_output=True, check=True).stdout
    h = hashlib.sha256()
    for name in sorted(f for f in listed.split(b'\0') if f):
        h.update(name + b'\0')
        with open(os.path.join(REPO, name.decode()), 'rb') as f:
            h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()[:16]


if __name__ == '__main__':
    print(recording_inputs())

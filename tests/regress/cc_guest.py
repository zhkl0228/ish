#!/usr/bin/env python3
"""The guest's programs of tests/regress, compiled on one host and run on another.

run.sh compiles each of its programs with $CC_GUEST, as "$CC_GUEST <flags> -o <program>
<sources>". A host with no compiler for the guest (macOS: no musl, no Docker) runs
programs another compiled (CI's linux job, in Alpine as the app's root is):

  cc_guest.py record <store> <compiler> <args...>   compiles with <compiler>, and keeps a
                                                    copy of the program in <store>
  cc_guest.py prebuilt <store> <args...>            the program kept for these args

$CC_GUEST is a script that runs one of them with run.sh's arguments (ci.sh writes them).
A program is kept under its key: the args but the program's path, a source's (a file
given) by its contents. run.sh compiles programs of the same name from other sources
(each fakefs test's init), and some sources it writes as it runs.
"""
import hashlib
import os
import shutil
import subprocess
import sys


def key_and_program(args):
    digest = hashlib.sha256()
    program = None
    i = 0
    while i < len(args):
        if args[i] == '-o':
            program = args[i + 1]
            i += 2
            continue
        if os.path.isfile(args[i]):
            with open(args[i], 'rb') as f:
                digest.update(b'file\0' + hashlib.sha256(f.read()).digest())
        else:
            digest.update(b'arg\0' + args[i].encode() + b'\0')
        i += 1
    if program is None:
        raise SystemExit('cc_guest.py: no -o in %r' % (args,))
    return digest.hexdigest(), program


def main():
    mode, store = sys.argv[1], sys.argv[2]
    if mode == 'record':
        compiler, args = sys.argv[3], sys.argv[4:]
        result = subprocess.run([compiler] + args)
        if result.returncode != 0:
            return result.returncode
        key, program = key_and_program(args)
        os.makedirs(store, exist_ok=True)
        shutil.copyfile(program, os.path.join(store, key))
        return 0
    if mode == 'prebuilt':
        args = sys.argv[3:]
        key, program = key_and_program(args)
        kept = os.path.join(store, key)
        if not os.path.exists(kept):
            print('cc_guest.py: no program kept in %s for %r (key %s)' % (store, args, key), file=sys.stderr)
            return 1
        shutil.copyfile(kept, program)
        os.chmod(program, 0o755)
        return 0
    raise SystemExit('cc_guest.py: unknown mode %r' % mode)


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Write the AltStore source (the JSON Feather, AltStore and SideStore read)
that lists an ipa release.

usage: app/altsource.py <ipa> <repository> <tag> <date> <out>

<repository> is the GitHub repository (owner/name) whose release <tag> holds
the ipa, published at <date> (ISO 8601, as 2026-11-26T15:52:44Z). The app's
name, identifier, version, build and minimum iOS come from the ipa's
Info.plist. .github/workflows/ci.yml uploads the source with each ipa it
releases, so the newest one is always at
https://github.com/<repository>/releases/latest/download/source.json: a source
to add once, which lists each new build when refreshed.

The version is the app's version and its build, as 2.3.0_821: Feather offers
an update only when the version differs, and the builds of a version come one
after another. The icon is master's, not the tag's: Feather keeps a source's
icon URL from when it was added, and the workflow deletes the tags of all but
the newest five releases.
"""
import json
import os
import plistlib
import sys
import urllib.parse
import zipfile

ICON = 'app/Assets.xcassets/AppIcon.appiconset/App Store.png'


def main():
    ipa, repository, tag, date, out = sys.argv[1:]
    with zipfile.ZipFile(ipa) as z:
        apps = [n for n in z.namelist() if n.count('/') == 2 and n.endswith('.app/Info.plist')]
        if len(apps) != 1:
            raise SystemExit('%s: %d apps, not one: %s' % (ipa, len(apps), apps))
        info = plistlib.loads(z.read(apps[0]))
    name = info['CFBundleName']
    version, build = info['CFBundleShortVersionString'], info['CFBundleVersion']
    owner = repository.split('/')[0]
    release = 'https://github.com/%s/releases/download/%s/' % (repository, tag)
    icon = 'https://raw.githubusercontent.com/%s/master/%s' % (repository, urllib.parse.quote(ICON))

    source = {
        'name': name,
        'identifier': 'com.github.%s' % repository.replace('/', '.'),
        'iconURL': icon,
        'apps': [{
            'name': name,
            'bundleIdentifier': info['CFBundleIdentifier'],
            'developerName': owner,
            'iconURL': icon,
            'localizedDescription': '%s, built by %s from its release %s.' % (name, repository, tag),
            'versions': [{
                'version': '%s_%s' % (version, build),
                'buildVersion': build,
                'date': date,
                'downloadURL': release + urllib.parse.quote(os.path.basename(ipa)),
                'size': os.path.getsize(ipa),
                'minOSVersion': info['MinimumOSVersion'],
            }],
        }],
    }
    with open(out, 'w') as f:
        json.dump(source, f, indent=2)
        f.write('\n')


if __name__ == '__main__':
    main()

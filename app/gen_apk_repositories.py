import os

# The root is built from this Alpine release (tools/build-rootfs.sh); its
# aarch64 packages come from Alpine's CDN.
APK_REPOSITORIES = [
    ('v3.24', 'main'),
    ('v3.24', 'community'),
]

repos_file = []
for version, repo in APK_REPOSITORIES:
    repos_file.append(f'https://dl-cdn.alpinelinux.org/alpine/{version}/{repo}')

with open(os.path.join(os.environ['BUILT_PRODUCTS_DIR'], os.environ['CONTENTS_FOLDER_PATH'], 'repositories.txt'), 'w') as f:
    for line in repos_file:
        print(line, file=f)

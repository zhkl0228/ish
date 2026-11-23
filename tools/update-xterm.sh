#!/bin/bash
# Make app/terminal/xterm_all.js, the terminal's xterm.js: xterm.js and the
# addons term.js uses, from npm at the versions below, with xterm.js's
# stylesheet, which the file puts in the page when it loads. Run it to move to
# other versions, then try the terminal: term.js uses their APIs.
#
# usage: tools/update-xterm.sh
#
# Needs the network, curl, tar and python3.
set -euo pipefail

PACKAGES=(
    @xterm/xterm@6.0.0
    @xterm/addon-unicode11@0.9.0
    @xterm/addon-fit@0.11.0
    @xterm/addon-webgl@0.19.0
    @xterm/addon-search@0.16.0
)

cd "$(dirname "$0")/.."
out=app/terminal/xterm_all.js
work=$(mktemp -d -t ish-xterm.XXXXXX)
trap 'rm -rf "$work"' EXIT

for spec in "${PACKAGES[@]}"; do
    name=${spec%@*}
    version=${spec##*@}
    dir=$work/${name#@xterm/}
    mkdir -p "$dir"
    # the tarball and its integrity, as npm's registry gives them
    curl -fsSL "https://registry.npmjs.org/$name/$version" > "$dir/meta.json"
    python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1]))["dist"]; print(d["tarball"]); print(d["integrity"])' \
        "$dir/meta.json" > "$dir/dist.txt"
    { read -r tarball; read -r integrity; } < "$dir/dist.txt"
    curl -fsSL -o "$dir/package.tgz" "$tarball"
    [ "${integrity%%-*}" = sha512 ] || { echo "$spec: integrity $integrity is not sha512" >&2; exit 1; }
    actual=sha512-$(openssl dgst -sha512 -binary "$dir/package.tgz" | base64 -w0)
    [ "$actual" = "$integrity" ] || { echo "$spec: integrity $actual, the registry says $integrity" >&2; exit 1; }
    tar xzf "$dir/package.tgz" -C "$dir"
done

python3 -I - "$work" "$out" "${PACKAGES[@]}" <<'EOF'
import json, os, sys

work, out, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
parts = []
license = open(os.path.join(work, 'xterm', 'package', 'LICENSE')).read().strip()
parts.append('/*\n * xterm.js and its addons, for the terminal (term.js): made by\n'
             ' * tools/update-xterm.sh from ' + ', '.join(specs) + '.\n *\n'
             + '\n'.join((' * ' + line).rstrip() for line in license.split('\n')) + '\n */\n')
for spec in specs:
    name = spec.rsplit('@', 1)[0]
    short = name[len('@xterm/'):]
    main = json.load(open(os.path.join(work, short, 'package', 'package.json')))['main']
    code = open(os.path.join(work, short, 'package', main)).read()
    # their source maps are not here
    code = '\n'.join(line for line in code.split('\n') if not line.startswith('//# sourceMappingURL='))
    parts.append('// %s\n%s\n' % (spec, code.rstrip()))
css = open(os.path.join(work, 'xterm', 'package', 'css', 'xterm.css')).read()
parts.append('// xterm.js\'s stylesheet, in the page\n'
             '(() => {\n'
             '    const style = document.createElement("style");\n'
             '    style.textContent = %s;\n'
             '    document.head.appendChild(style);\n'
             '})();\n' % json.dumps(css))
with open(out, 'w') as f:
    f.write('\n'.join(parts))
EOF
echo "wrote $out ($(du -h "$out" | cut -f1))"

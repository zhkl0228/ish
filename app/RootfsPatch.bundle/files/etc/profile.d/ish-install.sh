# Installed by the iSH app, which rewrites this file when it updates the root.
# Until one of the development tools ish-install installs is there, every
# login shell says how to install them.
ish_install_hint=1
for ish_install_cmd in python3 node kimi go java gcc clang redis-server mariadb gh shellcheck 7z; do
    if command -v "$ish_install_cmd" > /dev/null; then
        ish_install_hint=
        break
    fi
done
if [ -x /opt/claude-code/bin/claude ]; then
    ish_install_hint=
fi
if [ -n "$ish_install_hint" ]; then
    echo "Python, Node.js, Claude Code, Kimi Code, Go, Java, C/C++ and more are not installed yet:"
    echo "  ish-install              installs them all"
    echo "  ish-install go python    installs only those (ish-install -l lists them)"
    echo "  ish-install --cn ...     the same, from Alibaba Cloud's mirrors (in China)"
    echo
fi
unset ish_install_hint ish_install_cmd

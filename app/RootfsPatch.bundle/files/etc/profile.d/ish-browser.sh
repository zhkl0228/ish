# Installed by the iSH app, which rewrites this file when it updates the root.
# The browser programs open: xdg-open, which asks the app to open the URL.
# Claude Code's /login tries one only if $BROWSER names it, there being no
# display; gh and Python's webbrowser go by it too. A BROWSER already set is
# kept; set your own in ~/.profile.
export BROWSER="${BROWSER:-xdg-open}"

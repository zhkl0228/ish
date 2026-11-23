# Installed by the iSH app, which rewrites this file when it updates the root.

# A tab the app restored when it started again: its shell goes back to the
# directory the tab was in, which the app left in /ish/session/<the pts>.
ish_session_dir=/ish/session/$(tty | sed 's|^/dev/||; s|/|-|')
if [ -f "$ish_session_dir" ]; then
    cd "$(cat "$ish_session_dir")"
    rm -f "$ish_session_dir"
fi
unset ish_session_dir

# bash writes its history (~/.bash_history) when it exits, and the app can end
# without its shells exiting (closed from the app switcher, or reclaimed by
# iOS): each command goes there as it is run instead.
if [ -n "$BASH_VERSION" ]; then
    shopt -s histappend
    PROMPT_COMMAND="history -a${PROMPT_COMMAND:+; $PROMPT_COMMAND}"
fi

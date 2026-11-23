# Installed by the iSH app, which rewrites this file when it updates the root.

ish_sync_claude_memory() {
    local directory target imported_state checksum_state setting imported previous configured temporary
    directory="$HOME/.claude"
    target="$directory/CLAUDE.md"
    imported_state="$directory/.ish-imported-memory"
    checksum_state="$directory/.ish-imported-memory.cksum"

    # Settings' import, {"name": ..., "content": <base64>}, there once there is one
    setting=/proc/ish/defaults/claude_memory
    configured=0
    imported=
    if [ -e "$setting" ]; then
        configured=1
        imported=$(jq -r .content "$setting") || return 1
    fi
    previous=$(cat "$imported_state" 2>/dev/null)

    if [ "$configured" -eq 0 ]; then
        [ -f "$imported_state" ] || return 0
        if [ -f "$target" ] && { [ ! -f "$checksum_state" ] || [ "$(cksum < "$target")" != "$(cat "$checksum_state")" ]; }; then
            echo 'iSH: Claude memory was removed in Settings, but the modified guest copy was preserved' >&2
        else
            rm -f "$target"
        fi
        rm -f "$imported_state" "$checksum_state"
        return 0
    fi

    if [ -f "$imported_state" ] && [ "$imported" = "$previous" ] && [ -e "$target" ]; then
        return 0
    fi

    mkdir -p "$directory" || return 1
    if [ -e "$target" ]; then
        if [ ! -f "$checksum_state" ] || [ "$(cksum < "$target")" != "$(cat "$checksum_state")" ]; then
            echo 'iSH: imported Claude memory was not applied because the guest copy was modified' >&2
            printf '%s\n' "$imported" > "$imported_state"
            return 0
        fi
    fi

    temporary="$directory/.CLAUDE.md.ish.$$"
    if printf '%s' "$imported" | base64 -d > "$temporary"; then
        mv "$temporary" "$target" || return 1
        cksum < "$target" > "$checksum_state"
    else
        rm -f "$temporary"
        echo 'iSH: imported Claude memory could not be decoded' >&2
        return 1
    fi
    printf '%s\n' "$imported" > "$imported_state"
}

ish_sync_claude_memory
unset -f ish_sync_claude_memory

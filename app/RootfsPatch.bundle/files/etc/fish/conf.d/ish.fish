# Installed by the iSH app, which rewrites this file when it updates the root.
# fish reads no /etc/profile, where iSH sets a login shell up: PATH with
# /usr/local/bin (claude, ish-install, xdg-open), and /etc/profile.d's ish-*.sh
# (BROWSER, the directory a restored tab was in, Claude's memory, the mirrors
# of ish-install --cn). A login fish runs it in sh, and takes the environment
# it leaves and the directory it is in. What it prints goes to the terminal:
# only env's output is read.
status is-login; or return
for variable in (sh -c '. /etc/profile >&2; env -0' | string split0)
    set -l name_value (string split -m 1 = -- $variable)
    switch $name_value[1]
        case PWD
            cd $name_value[2]
        case SHLVL _
            # fish's own, which it does not let be set
        case '*'
            set -gx $name_value[1] $name_value[2]
    end
end

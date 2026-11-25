# Installed by the iSH app, which rewrites this file when it updates the root.
# zsh as ish-install zsh sets it up, for typing on a phone: a command you ran
# before that starts with what is typed, suggested in gray (→ takes it);
# commands colored as they are typed (red: no such command); tab's
# completions in a menu the arrows go through; ↑ and ↓ through the commands
# that start with what is typed. /etc/profile (iSH's /etc/profile.d with it)
# comes before this, from /etc/zsh/zprofile; your own settings go in ~/.zshrc,
# which comes after.

# emacs keys, as bash's: zsh would take vi's where $EDITOR or $VISUAL is vi
bindkey -e

# Each command written to the history as it is run, as ish-session.sh has
# bash do: the app can end without its shells exiting
HISTFILE=~/.zsh_history
HISTSIZE=10000
SAVEHIST=10000
setopt INC_APPEND_HISTORY HIST_IGNORE_DUPS

# compinit's dump of the completions it found, which a start reads instead of
# looking through them all again (40,000 lines of zsh, and 16 processes),
# where it can be written: compinit's own place, $ZDOTDIR, is ~/.config/zsh
# where ~ has no .zshrc (Alpine's zshenv), a directory nothing makes
zcompdump=${XDG_CACHE_HOME:-$HOME/.cache}/zsh/zcompdump
[[ -d ${zcompdump:h} ]] || mkdir -p ${zcompdump:h}
autoload -Uz compinit && compinit -d $zcompdump
unset zcompdump
zstyle ':completion:*' menu select
# A lowercase letter typed completes an uppercase one too
zstyle ':completion:*' matcher-list 'm:{a-z}={A-Z}'

# ↑ and ↓ as the terminal sends them, and in application cursor mode
autoload -Uz up-line-or-beginning-search down-line-or-beginning-search
zle -N up-line-or-beginning-search
zle -N down-line-or-beginning-search
bindkey '^[[A' up-line-or-beginning-search
bindkey '^[OA' up-line-or-beginning-search
bindkey '^[[B' down-line-or-beginning-search
bindkey '^[OB' down-line-or-beginning-search

source /usr/share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh
# Suggestions found in this shell: its async mode starts a process for each
# key typed (a process substitution), which in iSH costs more than the search
unset ZSH_AUTOSUGGEST_USE_ASYNC
# Last, as it asks: it colors what the widgets before it leave
source /usr/share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh

# gh (GitHub CLI, a Go binary): its offline commands (../gh/gh_train.sh: help, completion, config,
# alias), then what talks to GitHub's API, as a session's gh run list, pr list, release list and
# api do. With a token GitHub refuses, the requests go out and their answers (401) come back over
# TLS and through gh's JSON: no account needed. A request that does not reach GitHub fails as the
# others do, and the recording goes on. Its image is for the JIT's code region (gh's code native
# from the image, not translated into the region, which gh took the most of in a session that
# watched CI) and for the app without the JIT.
sh /tmp/aojit/gh/gh_train.sh
export GH_CONFIG_DIR=/tmp/aojit/gh/cfg_app GH_TOKEN=ish-aot-recording-not-a-token GH_PROMPT_DISABLED=1
rm -rf "$GH_CONFIG_DIR"
for i in 1 2; do
    gh api repos/cli/cli > /dev/null 2>&1 || true
    gh run list -R cli/cli -L 5 > /dev/null 2>&1 || true
    gh pr list -R cli/cli -L 5 > /dev/null 2>&1 || true
    gh release list -R cli/cli -L 3 > /dev/null 2>&1 || true
    gh repo view cli/cli > /dev/null 2>&1 || true
done
rm -rf "$GH_CONFIG_DIR"

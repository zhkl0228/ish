# Claude Code as a long session runs it: on its interactive screen (screen.py
# drives it in a pseudo-terminal), a session resumed, its tools (Read, Edit
# with the screen asking to allow it, Write, Bash with long output, a
# subagent) with thinking and a long markdown answer, the conversation
# scrolled back by the mouse wheel and down again, its context compacted
# (/compact), and a fresh screen's prompt; and the session in print mode,
# made and resumed. The wheel is the app's drag on claude's full screen,
# which draws its conversation again for each line it moves: no recording
# had turned it, and on the phone a drag through a long session took a core
# of its own. A notch moves it a line (Claude Code 2.1.332); scrolled back,
# it says "Jump to bottom". The model is ../llm/mock_llm.py, a "[long]
# <project>:" prompt. What claude_train.sh's print mode never runs: on the
# phone, against
# its images, a long session resumed on the screen ran 85.6% of claude.exe's
# blocks from the image, a prompt on a fresh screen 81.7%, print mode 95.3%
# (2026-11-25, Claude Code 2.1.332).
cd /tmp/aojit/claude || exit 1
python3 ../llm/mock_llm.py 18083 &
mock=$!
claude=/opt/claude-code/bin/claude
project=/tmp/aojit/claude/long
# A HOME of its own, where Claude Code's start asks nothing: its onboarding and
# release notes seen, the API key allowed, the project trusted
export HOME=/tmp/aojit/claude/home-long
export ANTHROPIC_BASE_URL=http://127.0.0.1:18083 ANTHROPIC_API_KEY=sk-ant-mock \
    DISABLE_AUTOUPDATER=1 CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1
# JavaScriptCore's JIT off, as the app's claude-wrapper runs it
export BUN_JSC_useJIT=0
rm -rf "$HOME" "$project" && mkdir -p "$HOME" "$project" || exit 1
python3 - "$HOME/.claude.json" "$project" <<'EOF'
import json, sys
version = json.load(open('/opt/claude-code/lib/node_modules/@anthropic-ai/claude-code/package.json'))['version']
json.dump({'hasCompletedOnboarding': True, 'lastOnboardingVersion': version, 'lastReleaseNotesSeen': version,
           'theme': 'dark', 'customApiKeyResponses': {'approved': ['sk-ant-mock'], 'rejected': []},
           'projects': {sys.argv[2]: {'hasTrustDialogAccepted': True, 'allowedTools': []}}}, open(sys.argv[1], 'w'))
EOF
until python3 -c "import socket; socket.create_connection(('127.0.0.1', 18083))" 2> /dev/null; do sleep 1; done

cd "$project" || exit 1
git init -q && git config user.email long@aojit && git config user.name long
cat > app.py <<'EOF'
limit = 10


def take(items):
    # TODO: stop at the first item that is done
    return [item for item in items[:limit] if item]
EOF
cat > test_app.py <<'EOF'
import unittest

import app


class TestApp(unittest.TestCase):
    def test_take(self):
        self.assertEqual(len(app.take(list(range(1, 50)))), app.limit)


if __name__ == '__main__':
    unittest.main()
EOF
for n in 1 2 3 4 5; do
    printf 'def helper%d(value):\n    # TODO: check the value\n    return value * %d\n' "$n" "$n" > "helper$n.py"
done
seq 1 3000 | sed 's/^/line /' > big.txt
git add -A && git commit -q -m "The project"
base=$(git rev-parse HEAD)
session=$(python3 -c 'import uuid; print(uuid.uuid4())')
# Not auto mode, this version's default: its check of each action is the
# server's, which the model API here is not
tools="--permission-mode default --allowedTools Bash Read Edit Write Agent"

# The session made in print mode, a prompt of tools, and resumed for another
# (its edits found done) and for a plain one
# shellcheck disable=SC2086
"$claude" -p "[long] $project: Raise the limit to 20 and tidy the helpers" --session-id "$session" $tools \
    < /dev/null | tail -c 100
echo
# shellcheck disable=SC2086
"$claude" -p --resume "$session" "[long] $project: Look at it all again" $tools < /dev/null | tail -c 100
echo
"$claude" -p --resume "$session" "What changed" < /dev/null | tail -c 100
echo
git reset -q --hard "$base"

# Resumed on the screen: a prompt of tools, the screen asking to allow each
# edit (Edit not allowed beforehand), its answer; the conversation scrolled
# back 120 lines by the wheel, which a drag turns in the app, and down to the
# answer's last line again; compacted; a plain prompt
python3 /tmp/aojit/claude/screen.py "$claude" --resume "$session" --permission-mode default \
    --allowedTools Bash Read Write Agent -- \
    ready "say:[long] $project: Raise the limit to 20 and tidy the helpers" \
    "until:make this edit to app.py" press:enter "until:make this edit to helper1.py" press:enter \
    "until:Summary of the session" "until:says more of the limit" \
    wheel:up:120 "until:Jump to bottom" wheel:down:160 "until:says more of the limit" \
    "say:/compact" "until:Compacted" "say:What changed" "until:plan" \
    say:/exit end || status=1
git reset -q --hard "$base"

# A fresh screen: its start, a prompt, its answer
python3 /tmp/aojit/claude/screen.py "$claude" --permission-mode default -- \
    ready "say:Plan the change to the loop" "until:plan" say:/exit end || status=1

kill $mock
exit ${status:-0}

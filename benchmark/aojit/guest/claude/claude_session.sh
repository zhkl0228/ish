# A Claude Code session that runs tools, as a coding session does: the model
# (../llm/mock_llm.py, a [session] prompt) has it run git, grep, find, sed and
# python's unittest through Bash, and its own Glob and Grep, in a small project
# of its own, twice. What it starts besides claude.exe (bash, busybox, git,
# python3, ripgrep) is what this measures for images of their own.
# AOJIT_EXTERNAL_MOCK=1: the model API runs outside ish, on the host's port
# 18082 (the guest's 127.0.0.1), so that its python3 is not timed with the session.
cd /tmp/aojit/claude || exit 1
if [ "$AOJIT_EXTERNAL_MOCK" != 1 ]; then
    python3 ../llm/mock_llm.py 18082 &
    mock=$!
fi
export HOME=/root
export ANTHROPIC_BASE_URL=http://127.0.0.1:18082 ANTHROPIC_API_KEY=sk-ant-mock \
    DISABLE_AUTOUPDATER=1 CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1
# JavaScriptCore's tiers but FTL, as the app's claude-wrapper runs it with
# iSH translating: the image is recorded in the mode the app runs it in (its
# compilers' code then claude.exe's too, its interpreter's still)
export BUN_JSC_useFTLJIT=0
claude=/opt/claude-code/bin/claude
until python3 -c "import socket; socket.create_connection(('127.0.0.1', 18082))" 2> /dev/null; do sleep 1; done

rm -rf project && mkdir project && cd project || exit 1
git init -q && git config user.email session@aojit && git config user.name session
cat > app.py <<'EOF'
limit = 10


def take(items):
    # TODO: stop at the first item that is done
    return [item for item in items[:limit] if item]


def total(items):
    return sum(len(str(item)) for item in take(items))
EOF
cat > test_app.py <<'EOF'
import unittest

import app


class TestApp(unittest.TestCase):
    def test_take(self):
        self.assertEqual(len(app.take(list(range(1, 50)))), app.limit)

    def test_total(self):
        self.assertGreater(app.total(['a', 'bb']), 0)


if __name__ == '__main__':
    unittest.main()
EOF
for n in 1 2 3 4 5; do
    printf 'def helper%d(value):\n    # TODO: check the value\n    return value * %d\n' "$n" "$n" > "helper$n.py"
done
git add -A && git commit -q -m "The project"
base=$(git rev-parse HEAD)

for _ in 1 2; do
    "$claude" -p "[session] Raise the limit to 20 and run the tests" --allowedTools "Bash Glob Grep Read" \
        < /dev/null | tail -c 200
    echo
    git reset -q --hard "$base"  # the session's changes, for the next run
done
[ -z "$mock" ] || kill "$mock"

# Claude Code (claude.exe: Bun, JavaScriptCore): the CLI starting, its help, and
# print mode answering through a model API of our own (../llm/mock_llm.py),
# plain and streamed JSON. Run directly, not through ish's wrapper (no proxy
# here), with no update or telemetry traffic.
cd /tmp/aojit/claude
python3 ../llm/mock_llm.py 18081 &
mock=$!
export HOME=/root
export ANTHROPIC_BASE_URL=http://127.0.0.1:18081 ANTHROPIC_API_KEY=sk-ant-mock \
    DISABLE_AUTOUPDATER=1 CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1
# JavaScriptCore's tiers but FTL, as the app's claude-wrapper runs it with
# iSH translating: the image is recorded in the mode the app runs it in (its
# compilers' code then claude.exe's too, its interpreter's still)
export BUN_JSC_useFTLJIT=0
claude=/opt/claude-code/bin/claude
# the model API up
until python3 -c "import socket; socket.create_connection(('127.0.0.1', 18081))" 2> /dev/null; do sleep 1; done
"$claude" --version < /dev/null
"$claude" --help < /dev/null > /dev/null
for prompt in "Plan the change to the loop" "Write the loop again" "What do the tests check"; do
    "$claude" -p "$prompt" < /dev/null | grep -c plan
done
"$claude" -p "Plan it as JSON" --output-format stream-json --verbose < /dev/null | tail -c 120; echo
kill $mock

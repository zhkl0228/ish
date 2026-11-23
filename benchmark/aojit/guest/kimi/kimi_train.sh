# Kimi Code (node): the CLI starting and its agent loop answering through a
# model API of our own (../llm/mock_llm.py), in prompt mode (no terminal
# needed), plain and streamed JSON, and node's own basics. Node runs in the mode
# ISH_NODE_MODE says: record it in the mode the app runs it in (with node's
# image, the interpreter).
cd /tmp/aojit/kimi
python3 ../llm/mock_llm.py 18080 &
mock=$!
export HOME=/root
# the mode the app runs node in with node's image (kernel/exec.c), unless the
# caller measures another
export ISH_NODE_MODE=${ISH_NODE_MODE:-interpreter}
export KIMI_MODEL_NAME=mock KIMI_MODEL_API_KEY=sk-mock KIMI_MODEL_PROVIDER_TYPE=openai \
    KIMI_MODEL_BASE_URL=http://127.0.0.1:18080/v1
# the model API up
until python3 -c "import socket; socket.create_connection(('127.0.0.1', 18080))" 2> /dev/null; do sleep 1; done
for i in 1 2 3; do node -e 'console.log(JSON.stringify({ok: true, n: process.argv.length}))'; done
kimi --version < /dev/null
kimi --help < /dev/null > /dev/null
for prompt in "Plan the change to the loop" "Write the loop again" "What do the tests check"; do
    kimi -p "$prompt" < /dev/null | grep -c plan
done
kimi -p "Plan it as JSON" --output-format stream-json < /dev/null | tail -c 120; echo
kill $mock

#!/usr/bin/env python3
# A model API for the agent CLIs' recording workloads (kimi, claude): OpenAI's
# chat completions (POST /v1/chat/completions) and Anthropic's messages
# (POST /v1/messages, /v1/messages/count_tokens), streamed (SSE) when asked,
# with a canned markdown answer at once. Anything else is a 404, and its path
# goes to stderr, so a workload's log says what else a client asked for.
#
# A Claude Code request whose first message says [session] and that offers the
# Bash tool gets SESSION's tool calls instead, one a turn (as many as the
# conversation has tool results so far), then the answer: what a coding
# session runs, git, grep, find, python and Claude Code's own Grep and Glob.
#
# One whose last prompt says "[long] <project>:" gets LONG's tool calls in that
# project, one a turn (as many as that prompt's calls the conversation has, by
# their ids: a resumed session's request has its messages merged, a prompt's
# tool results before its text), each after a line of text and with thinking
# when the request asks for it, then
# LONG_ANSWER: a long session's turns, as ../claude/claude_long.sh records
# them, prompt after prompt in one session (Read, Edit, Write, Bash with long
# output, a subagent). A tool the request does not offer is called all the
# same (Claude Code answers that it has none), and said on stderr.
#
# usage: python3 mock_llm.py PORT   (until killed)
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ANSWER = ('Here is a **plan**, with `code` and a list:\n\n'
          '1. Read the file `src/main.ts` and find the handler.\n'
          '2. Change the loop so it stops early.\n\n'
          '```ts\nfor (const item of items) {\n  if (item.done) break;\n  process(item);\n}\n```\n\n'
          'Then run the tests again.\n') * 3
WORDS = [w + ' ' for w in ANSWER.split(' ')]

# The tool calls of a session, in the order Claude Code is asked for them
SESSION = [
    ('Bash', {'command': 'git status && git log --oneline -5', 'description': 'Show the state of the repository'}),
    ('Glob', {'pattern': '**/*.py'}),
    ('Grep', {'pattern': 'def ', 'output_mode': 'content', '-n': True}),
    ('Bash', {'command': 'grep -rn "TODO" . | head -20; find . -name "*.py" | wc -l', 'description': 'Find the TODOs'}),
    ('Bash', {'command': 'ls -la && wc -l *.py && head -30 app.py', 'description': 'Look at the files'}),
    ('Bash', {'command': "sed -i 's/limit = 10/limit = 20/' app.py && git diff --stat && git diff | head -40",
              'description': 'Change the limit'}),
    ('Bash', {'command': 'python3 -m unittest -q test_app 2>&1 | tail -5', 'description': 'Run the tests'}),
    ('Bash', {'command': 'git add -A && git commit -q -m "Raise the limit" && git log --oneline -3',
              'description': 'Commit the change'}),
]


LONG_ANSWER = '''## Summary of the session

The limit is **20** now, the helpers check their values, and the tests pass.

| File | Change | Lines |
|------|--------|------:|
| `app.py` | the limit raised | 1 |
| `helper1.py` | the value checked | 1 |
| `NOTES.md` | written | 40 |

### What was run

1. `git status` and the log, to see where the project stood
2. the TODOs found with `grep`, and every Python file with `find`
3. the tests, before and after:
   - `test_take` checks the limit
   - `test_total` checks the sum

```diff
-limit = 10
+limit = 20
```

```python
def take(items):
    return [item for item in items[:limit] if item]
```

```bash
python3 -m unittest -q test_app
```

> The subagent read the helpers: each multiplies its value, and none checked it before.

Next: the [documentation](https://example.com/docs) says more of the limit.
''' * 2


def long_steps(project):
    """The tool calls of a long session's prompt, in project"""
    return [
        ('Bash', {'command': 'git status && git log --oneline -5', 'description': 'Show the state of the repository'}),
        ('Read', {'file_path': project + '/app.py'}),
        ('Bash', {'command': 'grep -rn "TODO" . | head -20; find . -name "*.py" | sort',
                  'description': 'Find the TODOs and the Python files'}),
        ('Edit', {'file_path': project + '/app.py', 'old_string': 'limit = 10', 'new_string': 'limit = 20'}),
        ('Read', {'file_path': project + '/big.txt', 'offset': 100, 'limit': 200}),
        ('Bash', {'command': 'python3 -m unittest -q test_app 2>&1 | tail -5', 'description': 'Run the tests'}),
        ('Write', {'file_path': project + '/NOTES.md',
                   'content': ''.join('- note %d: the limit and the helpers\n' % i for i in range(40))}),
        ('Read', {'file_path': project + '/helper1.py'}),
        ('Edit', {'file_path': project + '/helper1.py', 'old_string': '# TODO: check the value',
                  'new_string': 'assert value is not None'}),
        ('Bash', {'command': 'seq 1 2000', 'description': 'Count to 2000'}),
        ('Agent', {'description': 'Look at the helpers', 'prompt': 'Read the helpers and say what they do',
                   'subagent_type': 'general-purpose'}),
        ('Bash', {'command': 'git diff --stat && git diff | head -60', 'description': 'Show the change'}),
        ('Bash', {'command': 'git add -A && git commit -q -m "The limit and the helpers" && git log --oneline -3',
                  'description': 'Commit the change'}),
    ]


def blocks(message):
    content = message.get('content')
    return [{'type': 'text', 'text': content}] if isinstance(content, str) else content


def long_step(request):
    """For a [long] prompt: (its tag, its project, its tool call this turn or None for the
    answer, the tool results the conversation has); None for another request. The tag,
    toolu_long_<k>_ of the conversation's k-th [long] prompt, begins its calls' ids."""
    messages = request.get('messages') or []
    # what was typed: not Claude Code's own text in a user message, a reminder
    # or a slash command's record (<system-reminder>, <command-name>)
    typed = [b['text'] for m in messages if m.get('role') == 'user' for b in blocks(m)
             if b.get('type') == 'text' and not b['text'].lstrip().startswith('<')]
    # a prompt after a [long] one is another prompt: compacted, the context
    # keeps the [long] one
    if not typed or '[long] ' not in typed[-1]:
        return None
    prompts = [t for t in typed if '[long] ' in t]
    tag = 'toolu_long_%d_' % len(prompts)
    text = prompts[-1]
    project = text[text.index('[long] ') + len('[long] '):].split(':', 1)[0]
    every = [b for m in messages for b in blocks(m)]
    done = sum(1 for b in every if b.get('type') == 'tool_use' and b['id'].startswith(tag))
    results = sum(1 for b in every if b.get('type') == 'tool_result')
    steps = long_steps(project)
    step = steps[done] if done < len(steps) else None
    if step is not None and step[0] not in [t.get('name') for t in request.get('tools') or []]:
        print('mock_llm: [long] calls %s, which the request does not offer' % step[0], file=sys.stderr, flush=True)
    return tag, project, step, results


def session_step(request):
    """The tool call this request is to get, or None: a [session] conversation that offers Bash."""
    messages = request.get('messages') or []
    if not messages or '[session]' not in json.dumps(messages[0]):
        return None
    if 'Bash' not in [t.get('name') for t in request.get('tools') or []]:
        return None
    done = sum(1 for m in messages if isinstance(m.get('content'), list)
               for b in m['content'] if b.get('type') == 'tool_result')
    return SESSION[done] if done < len(SESSION) else None


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def send_json(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def start_sse(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()

    def chunk(self, text):
        data = text.encode()
        self.wfile.write(b'%x\r\n%s\r\n' % (len(data), data))
        self.wfile.flush()

    def end_sse(self):
        self.wfile.write(b'0\r\n\r\n')
        self.wfile.flush()

    def do_GET(self):
        if self.path.rstrip('/').endswith('/models'):
            self.send_json(200, {'object': 'list', 'data': [{'id': 'mock', 'object': 'model', 'type': 'model',
                                                             'display_name': 'Mock'}]})
        else:
            print('mock_llm: GET %s: 404' % self.path, file=sys.stderr, flush=True)
            self.send_json(404, {'error': {'type': 'not_found_error', 'message': self.path}})

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        request = json.loads(self.rfile.read(length) or b'{}')
        path = self.path.split('?')[0].rstrip('/')
        if path.endswith('/chat/completions'):
            self.openai(request)
        elif path.endswith('/messages/count_tokens'):
            self.send_json(200, {'input_tokens': 42})
        elif path.endswith('/messages'):
            self.anthropic(request)
        else:
            print('mock_llm: POST %s: 404' % self.path, file=sys.stderr, flush=True)
            self.send_json(404, {'error': {'type': 'not_found_error', 'message': self.path}})

    def openai(self, request):
        model = request.get('model', 'mock')
        if not request.get('stream'):
            self.send_json(200, {'id': 'c1', 'object': 'chat.completion', 'created': 0, 'model': model,
                                 'choices': [{'index': 0, 'message': {'role': 'assistant', 'content': ANSWER},
                                              'finish_reason': 'stop'}],
                                 'usage': {'prompt_tokens': 10, 'completion_tokens': len(WORDS), 'total_tokens': 10 + len(WORDS)}})
            return
        self.start_sse()
        for word in WORDS:
            self.chunk('data: %s\n\n' % json.dumps({'id': 'c1', 'object': 'chat.completion.chunk', 'created': 0, 'model': model,
                                                     'choices': [{'index': 0, 'delta': {'content': word}, 'finish_reason': None}]}))
        self.chunk('data: %s\n\n' % json.dumps({'id': 'c1', 'object': 'chat.completion.chunk', 'created': 0, 'model': model,
                                                 'choices': [{'index': 0, 'delta': {}, 'finish_reason': 'stop'}],
                                                 'usage': {'prompt_tokens': 10, 'completion_tokens': len(WORDS),
                                                           'total_tokens': 10 + len(WORDS)}}))
        self.chunk('data: [DONE]\n\n')
        self.end_sse()

    def anthropic(self, request):
        model = request.get('model', 'mock')
        long = long_step(request)
        if long is not None:
            self.anthropic_long(request, model, *long)
            return
        step = session_step(request)
        if step:
            self.anthropic_tool(request, model, step)
            return
        usage = {'input_tokens': 10, 'output_tokens': len(WORDS)}
        if not request.get('stream'):
            self.send_json(200, {'id': 'msg_1', 'type': 'message', 'role': 'assistant', 'model': model,
                                 'content': [{'type': 'text', 'text': ANSWER}], 'stop_reason': 'end_turn',
                                 'stop_sequence': None, 'usage': usage})
            return

        def event(name, body):
            self.chunk('event: %s\ndata: %s\n\n' % (name, json.dumps(body)))

        self.start_sse()
        event('message_start', {'type': 'message_start', 'message': {
            'id': 'msg_1', 'type': 'message', 'role': 'assistant', 'model': model, 'content': [],
            'stop_reason': None, 'stop_sequence': None, 'usage': {'input_tokens': 10, 'output_tokens': 1}}})
        event('content_block_start', {'type': 'content_block_start', 'index': 0,
                                      'content_block': {'type': 'text', 'text': ''}})
        for word in WORDS:
            event('content_block_delta', {'type': 'content_block_delta', 'index': 0,
                                          'delta': {'type': 'text_delta', 'text': word}})
        event('content_block_stop', {'type': 'content_block_stop', 'index': 0})
        event('message_delta', {'type': 'message_delta', 'delta': {'stop_reason': 'end_turn', 'stop_sequence': None},
                                'usage': {'output_tokens': len(WORDS)}})
        event('message_stop', {'type': 'message_stop'})
        self.end_sse()


    def anthropic_tool(self, request, model, step):
        name, tool_input = step
        tool_id = 'toolu_%02d' % SESSION.index(step)
        usage = {'input_tokens': 10, 'output_tokens': 20}
        if not request.get('stream'):
            self.send_json(200, {'id': 'msg_t', 'type': 'message', 'role': 'assistant', 'model': model,
                                 'content': [{'type': 'tool_use', 'id': tool_id, 'name': name, 'input': tool_input}],
                                 'stop_reason': 'tool_use', 'stop_sequence': None, 'usage': usage})
            return

        def event(kind, body):
            self.chunk('event: %s\ndata: %s\n\n' % (kind, json.dumps(body)))

        self.start_sse()
        event('message_start', {'type': 'message_start', 'message': {
            'id': 'msg_t', 'type': 'message', 'role': 'assistant', 'model': model, 'content': [],
            'stop_reason': None, 'stop_sequence': None, 'usage': {'input_tokens': 10, 'output_tokens': 1}}})
        event('content_block_start', {'type': 'content_block_start', 'index': 0,
                                      'content_block': {'type': 'tool_use', 'id': tool_id, 'name': name, 'input': {}}})
        event('content_block_delta', {'type': 'content_block_delta', 'index': 0,
                                      'delta': {'type': 'input_json_delta', 'partial_json': json.dumps(tool_input)}})
        event('content_block_stop', {'type': 'content_block_stop', 'index': 0})
        event('message_delta', {'type': 'message_delta', 'delta': {'stop_reason': 'tool_use', 'stop_sequence': None},
                                'usage': {'output_tokens': 20}})
        event('message_stop', {'type': 'message_stop'})
        self.end_sse()


    def anthropic_long(self, request, model, tag, project, step, results):
        """A [long] turn: thinking (when asked for), a line of text, and the tool call; or, after the
        last, LONG_ANSWER"""
        content = []
        if (request.get('thinking') or {}).get('type', 'disabled') != 'disabled':
            content.append({'type': 'thinking', 'thinking': 'The next step in %s, after %d results: %s.' % (
                project, results, step[0] if step else 'the summary'), 'signature': 'mock-signature-%d' % results})
        if step is None:
            content.append({'type': 'text', 'text': LONG_ANSWER})
            stop = 'end_turn'
        else:
            content.append({'type': 'text', 'text': 'Next, %s:' % step[0]})
            # the prompt's, and unique in the session
            content.append({'type': 'tool_use', 'id': '%s%04d' % (tag, results), 'name': step[0], 'input': step[1]})
            stop = 'tool_use'
        if not request.get('stream'):
            self.send_json(200, {'id': 'msg_l', 'type': 'message', 'role': 'assistant', 'model': model,
                                 'content': content, 'stop_reason': stop, 'stop_sequence': None,
                                 'usage': {'input_tokens': 10, 'output_tokens': 50}})
            return

        def event(kind, body):
            self.chunk('event: %s\ndata: %s\n\n' % (kind, json.dumps(body)))

        self.start_sse()
        event('message_start', {'type': 'message_start', 'message': {
            'id': 'msg_l', 'type': 'message', 'role': 'assistant', 'model': model, 'content': [],
            'stop_reason': None, 'stop_sequence': None, 'usage': {'input_tokens': 10, 'output_tokens': 1}}})
        for index, block in enumerate(content):
            if block['type'] == 'thinking':
                event('content_block_start', {'type': 'content_block_start', 'index': index,
                                              'content_block': {'type': 'thinking', 'thinking': '', 'signature': ''}})
                event('content_block_delta', {'type': 'content_block_delta', 'index': index,
                                              'delta': {'type': 'thinking_delta', 'thinking': block['thinking']}})
                event('content_block_delta', {'type': 'content_block_delta', 'index': index,
                                              'delta': {'type': 'signature_delta', 'signature': block['signature']}})
            elif block['type'] == 'text':
                event('content_block_start', {'type': 'content_block_start', 'index': index,
                                              'content_block': {'type': 'text', 'text': ''}})
                for word in block['text'].split(' '):
                    event('content_block_delta', {'type': 'content_block_delta', 'index': index,
                                                  'delta': {'type': 'text_delta', 'text': word + ' '}})
            else:
                event('content_block_start', {'type': 'content_block_start', 'index': index,
                                              'content_block': {'type': 'tool_use', 'id': block['id'],
                                                                'name': block['name'], 'input': {}}})
                event('content_block_delta', {'type': 'content_block_delta', 'index': index,
                                              'delta': {'type': 'input_json_delta',
                                                        'partial_json': json.dumps(block['input'])}})
            event('content_block_stop', {'type': 'content_block_stop', 'index': index})
        event('message_delta', {'type': 'message_delta', 'delta': {'stop_reason': stop, 'stop_sequence': None},
                                'usage': {'output_tokens': 50}})
        event('message_stop', {'type': 'message_stop'})
        self.end_sse()


ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), Handler).serve_forever()

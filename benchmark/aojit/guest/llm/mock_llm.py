#!/usr/bin/env python3
# A model API for the agent CLIs' recording workloads (kimi, claude): OpenAI's
# chat completions (POST /v1/chat/completions) and Anthropic's messages
# (POST /v1/messages, /v1/messages/count_tokens), streamed (SSE) when asked,
# with a canned markdown answer at once. Anything else is a 404, and its path
# goes to stderr, so a workload's log says what else a client asked for.
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


ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), Handler).serve_forever()

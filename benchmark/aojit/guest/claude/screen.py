#!/usr/bin/env python3
"""Claude Code's interactive screen, driven: claude in a pseudo-terminal of 120x40, and steps.

usage: screen.py CLAUDE [ARG...] -- STEP...

  ready          wait for the input box (its prompt, U+276F) and for the screen to go quiet
  say:TEXT       type TEXT, then Return
  press:KEY      press KEY: enter, down, up, escape or tab
  until:TEXT     wait for TEXT on a line of the screen written since the last Return, key
                 pressed or wheel turned (on the screen, or scrolled off it since)
  wheel:WAY:N    turn the mouse wheel N notches, WAY up or down, then wait for the screen to go
                 quiet: SGR reports at the screen's middle, one every 0.05 s, as the app sends a
                 drag's on claude's full screen, which asks for them (?1000h ?1006h) and scrolls
                 its conversation by them. Only to a claude that has asked: to one that has not,
                 they are keys typed into its input box
  end            wait for claude to exit (after say:/exit)

The screen is what claude's output draws (Screen: the cursor moves, erasing and scrolling it
uses), not its bytes: it draws a line again by writing only the cells that changed, moving the
cursor over the rest, so a line's text is not in its bytes as it reads.

A step not met in SCREEN_TIMEOUT seconds (600) ends it with the screen as it was, and claude
killed: the screen of another version of Claude Code than the one this was written for, which
a recording should not take as done: its message says FAIL, which tools/jit_aot/record.sh
looks for in a workload's output. SCREEN_LOG: a file to write all claude wrote to, as it ends.
"""
import codecs
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time
import unicodedata

TIMEOUT = int(os.environ.get('SCREEN_TIMEOUT', '600'))
ROWS, COLS = 40, 120
KEYS = {'enter': '\r', 'down': '\x1b[B', 'up': '\x1b[A', 'escape': '\x1b', 'tab': '\t'}
PROMPT = '❯'
# An SGR report's button for a notch of the wheel
WHEEL = {'up': 64, 'down': 65}
WHEEL_GAP = 0.05


class Screen:
    """A terminal's cells, as the text written to it draws them. Each line has a mark of
    whether it was written since the last clear_written; a line scrolled off the top is kept
    while it has one."""

    def __init__(self):
        self.lines = [[' '] * COLS for _ in range(ROWS)]
        self.written = [False] * ROWS
        self.scrolled = []
        self.row = self.col = 0
        self.saved = (0, 0)
        self.state = 'text'
        self.params = ''
        # the private modes set (CSI ? n h) and not reset since (CSI ? n l)
        self.modes = set()

    def clear_written(self):
        self.written = [False] * ROWS
        self.scrolled = []

    def text(self, only_written):
        rows = [''.join(line).rstrip() for line, w in zip(self.lines, self.written) if w or not only_written]
        return self.scrolled + rows if only_written else rows

    def scroll(self):
        if self.written[0]:
            self.scrolled.append(''.join(self.lines[0]).rstrip())
        self.lines = self.lines[1:] + [[' '] * COLS]
        self.written = self.written[1:] + [False]

    def linefeed(self):
        if self.row == ROWS - 1:
            self.scroll()
        else:
            self.row += 1

    def put(self, ch):
        width = 2 if unicodedata.east_asian_width(ch) in 'WF' else 0 if unicodedata.combining(ch) else 1
        if width == 0:
            return
        if self.col + width > COLS:
            self.col = 0
            self.linefeed()
        self.lines[self.row][self.col] = ch
        if width == 2 and self.col + 1 < COLS:
            self.lines[self.row][self.col + 1] = ''
        self.written[self.row] = True
        self.col += width

    def erase(self, row, start, end):
        for c in range(start, end):
            self.lines[row][c] = ' '
        self.written[row] = True

    def csi(self, final, params):
        private = params[:1] in ('?', '<', '>', '=')
        args = [int(p) if p.isdigit() else 0 for p in (params[1:] if private else params).split(';')]
        n = max(args[0], 1)
        if params[:1] == '?' and final in 'hl':
            for mode in args:
                if final == 'h':
                    self.modes.add(mode)
                else:
                    self.modes.discard(mode)
        if private or final in 'mhlqrtnc':
            return  # modes, colors, queries: no cells
        if final == 'A':
            self.row = max(self.row - n, 0)
        elif final == 'B':
            self.row = min(self.row + n, ROWS - 1)
        elif final == 'C':
            self.col = min(self.col + n, COLS - 1)
        elif final == 'D':
            self.col = max(self.col - n, 0)
        elif final == 'E':
            self.row, self.col = min(self.row + n, ROWS - 1), 0
        elif final == 'F':
            self.row, self.col = max(self.row - n, 0), 0
        elif final == 'G':
            self.col = min(n, COLS) - 1
        elif final == 'd':
            self.row = min(n, ROWS) - 1
        elif final in 'Hf':
            row, col = (args + [0])[:2]
            self.row, self.col = min(max(row, 1), ROWS) - 1, min(max(col, 1), COLS) - 1
        elif final == 'K':
            start, end = {0: (self.col, COLS), 1: (0, self.col + 1), 2: (0, COLS)}[args[0]]
            self.erase(self.row, start, end)
        elif final == 'J':
            if args[0] == 3:
                return  # the scrollback, which this has none of
            if args[0] == 0:
                self.erase(self.row, self.col, COLS)
                rows = range(self.row + 1, ROWS)
            elif args[0] == 1:
                self.erase(self.row, 0, self.col + 1)
                rows = range(0, self.row)
            else:
                rows = range(ROWS)
            for r in rows:
                self.erase(r, 0, COLS)
        elif final == 'X':
            self.erase(self.row, self.col, min(self.col + n, COLS))
        elif final == 'P':
            line = self.lines[self.row]
            self.lines[self.row] = line[:self.col] + line[self.col + n:] + [' '] * n
            self.written[self.row] = True
        elif final == '@':
            line = self.lines[self.row]
            self.lines[self.row] = (line[:self.col] + [' '] * n + line[self.col:])[:COLS]
            self.written[self.row] = True
        elif final == 'S':
            for _ in range(n):
                self.scroll()
        elif final == 's':
            self.saved = (self.row, self.col)
        elif final == 'u':
            self.row, self.col = self.saved
        else:
            raise SystemExit('FAIL screen.py: a sequence screen.py does not draw: ESC [ %s %s' % (params, final))

    def feed(self, text):
        for ch in text:
            if self.state == 'text':
                if ch == '\x1b':
                    self.state = 'esc'
                elif ch == '\r':
                    self.col = 0
                elif ch == '\n':
                    self.linefeed()
                elif ch == '\b':
                    self.col = max(self.col - 1, 0)
                elif ch == '\t':
                    self.col = min((self.col // 8 + 1) * 8, COLS - 1)
                elif ch >= ' ':
                    self.put(ch)
            elif self.state == 'esc':
                if ch == '[':
                    self.state, self.params = 'csi', ''
                elif ch in ']PX^_':
                    self.state = 'string'  # OSC, DCS and the like: up to BEL or ST
                elif ch in '()*+#':
                    self.state = 'charset'
                else:
                    if ch == '7':
                        self.saved = (self.row, self.col)
                    elif ch == '8':
                        self.row, self.col = self.saved
                    elif ch == 'M':
                        self.row = max(self.row - 1, 0)
                    self.state = 'text'
            elif self.state == 'csi':
                if '\x40' <= ch <= '\x7e':
                    self.csi(ch, self.params)
                    self.state = 'text'
                else:
                    self.params += ch
            elif self.state == 'string':
                if ch == '\x07':
                    self.state = 'text'
                elif ch == '\x1b':
                    self.state = 'string-esc'
            elif self.state == 'string-esc':
                self.state = 'text' if ch == '\\' else 'string'
            elif self.state == 'charset':
                self.state = 'text'


class Claude:
    def __init__(self, argv):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.update(TERM='xterm-256color', COLUMNS=str(COLS), LINES=str(ROWS))
            os.execvp(argv[0], argv)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0, 0))
        self.raw = b''
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.screen = Screen()

    def read_for(self, seconds):
        """What it writes in seconds; False once it has closed the terminal"""
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], max(0, end - time.time()))
            if not ready:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return False
            if not data:
                return False
            self.raw += data
            self.screen.feed(self.decoder.decode(data))
        return True

    def log(self):
        if os.environ.get('SCREEN_LOG'):
            with open(os.environ['SCREEN_LOG'], 'wb') as f:
                f.write(self.raw)

    def fail(self, what, waited=True):
        self.log()
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        rows = [r for r in self.screen.text(False) if r.strip()]
        when = ' in %ds' % TIMEOUT if waited else ''
        sys.exit('FAIL screen.py: %s%s; the screen:\n%s' % (what, when, '\n'.join(rows)))

    def until(self, text, only_written=True):
        end = time.time() + TIMEOUT
        while not any(text in line for line in self.screen.text(only_written)):
            if time.time() > end or not self.read_for(0.5):
                self.fail('no %r' % text)

    def quiet(self, seconds):
        end = time.time() + TIMEOUT
        while time.time() < end:
            before = len(self.raw)
            self.read_for(seconds)
            if len(self.raw) == before:
                return
        self.fail('never quiet for %gs' % seconds)

    def send(self, keys):
        self.screen.clear_written()
        os.write(self.fd, keys.encode())
        self.read_for(0.5)

    def wheel(self, way, notches):
        modes = self.screen.modes
        if not (modes & {1000, 1002, 1003} and 1006 in modes):
            self.fail('no SGR mouse reports asked for (private modes set: %s)' % sorted(modes), waited=False)
        self.screen.clear_written()
        report = ('\x1b[<%d;%d;%dM' % (WHEEL[way], COLS // 2, ROWS // 2)).encode()
        for _ in range(notches):
            os.write(self.fd, report)
            self.read_for(WHEEL_GAP)
        self.quiet(1)

    def end(self):
        end = time.time() + TIMEOUT
        while time.time() < end:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.log()
                if status != 0:
                    sys.exit('FAIL screen.py: claude exited with status %d' % status)
                return
            self.read_for(0.5)
        self.fail('claude did not exit')


def main():
    args = sys.argv[1:]
    at = args.index('--')
    claude = Claude(args[:at])
    for step in args[at + 1:]:
        kind, _, text = step.partition(':')
        if kind == 'ready':
            claude.until(PROMPT, only_written=False)
            claude.quiet(3)
        elif kind == 'say':
            claude.send(text)
            # until looks from the Return on: not at the text's echo
            claude.send('\r')
        elif kind == 'press':
            claude.send(KEYS[text])
        elif kind == 'until':
            claude.until(text)
        elif kind == 'wheel':
            way, _, notches = text.partition(':')
            claude.wheel(way, int(notches))
        elif kind == 'end':
            claude.end()
        else:
            sys.exit('FAIL screen.py: no step %r' % step)
        print('screen.py: %s' % step, flush=True)


if __name__ == '__main__':
    main()

// The terminal's page (app/terminal: term.js on xterm.js) in headless
// Chromium, with a stand-in for the app's message handlers (Terminal.m,
// TerminalView.m): output in UTF-8 split anywhere, the answers to programs,
// bracketed paste, OSC 52, titles, the application cursor mode, sizes, the
// scroll view's sync both ways, its drags as a full-screen program's wheel,
// long-press selection and its rect, Cmd-click
// links, wrapped lines, resizing, and no errors. WebKit is not Chromium, so
// this checks term.js, not the app.
//
// Run, after `npm install playwright && npx playwright install chromium`:
//   node tests/terminal/term-page-test.js app/terminal/term.html
// The browser needs its libraries (npx playwright install-deps) and a
// monospace font.
const { chromium } = require('playwright');
const path = require('path');

const page_url = 'file://' + path.resolve(process.argv[2]);
let passed = 0, failed = 0;
function check(name, ok, detail) {
    if (ok) { passed++; console.log('PASS ' + name); }
    else { failed++; console.log('FAIL ' + name + ': ' + detail); }
}
const utf8 = (s) => Buffer.from(s, 'utf8').toString('latin1');

(async () => {
    const browser = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
    const page = await browser.newPage({ viewport: { width: 400, height: 300 } });
    page.on('console', (m) => console.log('  console: ' + m.text()));
    page.on('pageerror', (e) => console.log('  pageerror: ' + e.message));
    await page.addInitScript(() => {
        window.__messages = [];
        const names = ['load', 'log', 'sendInput', 'sendBinary', 'resize', 'propUpdate', 'setTitle', 'copyText',
                       'syncFocus', 'focus', 'newScrollRange', 'newScrollTop', 'newWheelScrolling', 'openLink'];
        window.webkit = { messageHandlers: {} };
        for (const name of names)
            window.webkit.messageHandlers[name] = { postMessage: (body) => window.__messages.push([name, body]) };
    });
    await page.goto(page_url);
    await page.waitForFunction(() => window.__messages.some(([n]) => n == 'load'), null, { timeout: 10000 });
    const take = () => page.evaluate(() => window.__messages.splice(0));
    const write = (s) => page.evaluate((d) => exports.write(d), s);
    let msgs = await take();
    check('loads, says so', msgs.some(([n]) => n == 'load') && msgs.some(([n]) => n == 'syncFocus'), JSON.stringify(msgs));
    check('no errors logged at load', !msgs.some(([n, b]) => n == 'log' && /error/i.test(b)), JSON.stringify(msgs.filter(([n]) => n == 'log')));

    await page.evaluate(() => exports.updateStyle({ foregroundColor: '#fff', backgroundColor: '#000', fontFamily: 'monospace',
        fontSize: 14, colorPaletteOverrides: null, blinkCursor: false, cursorShape: 'BEAM' }));
    const style = await page.evaluate(() => [term.options.cursorStyle, term.options.theme.selectionBackground, term.options.fontSize]);
    check('updateStyle', style[0] == 'bar' && style[1] == 'rgba(255, 255, 255, 0.35)' && style[2] == 14, JSON.stringify(style));
    const renderer = await page.evaluate(() => !!document.querySelector('.xterm-screen canvas'));
    console.log('  renderer: ' + (renderer ? 'WebGL (canvas)' : 'DOM'));

    await take();
    await write(utf8('hello\r\n中文 ✓ 🙂\r\n'));
    const emoji = utf8('🙂');
    await write(emoji.slice(0, 2));
    await write(emoji.slice(2) + '\r\n');
    let text = await page.evaluate(() => exports.getText());
    check('UTF-8, split between writes too', text.includes('hello\n中文 ✓ 🙂\n🙂'), JSON.stringify(text.slice(0, 60)));

    await write('\x1b[c');
    msgs = await take();
    const da = msgs.filter(([n]) => n == 'sendInput').map(([, b]) => b).join('');
    check('device attributes answered to the input', da.startsWith('\x1b[?'), JSON.stringify(msgs));

    await write('\x1b[?2004h');
    await take();
    await page.evaluate(() => exports.paste('a\nb'));
    msgs = await take();
    const pasted = msgs.filter(([n]) => n == 'sendInput').map(([, b]) => b).join('');
    check('bracketed paste', pasted == '\x1b[200~a\rb\x1b[201~', JSON.stringify(pasted));
    await write('\x1b[?2004l');

    await write('\x1b]52;c;' + Buffer.from('copied ✓').toString('base64') + '\x07');
    await write('\x1b]0;my title\x07');
    await write('\x1b[?1h');
    msgs = await take();
    check('OSC 52 copies', msgs.some(([n, b]) => n == 'copyText' && b == 'copied ✓'), JSON.stringify(msgs));
    check('title', msgs.some(([n, b]) => n == 'setTitle' && b == 'my title'), JSON.stringify(msgs));
    check('application cursor', msgs.some(([n, b]) => n == 'propUpdate' && b[0] == 'applicationCursor' && b[1] === true), JSON.stringify(msgs));
    await write('\x1b[?1l');

    const size = await page.evaluate(() => [exports.getSize(), exports.getCharacterSize()]);
    check('size and cell size', size[0][0] > 10 && size[0][1] > 5 && size[1][0] > 0 && size[1][1] > 0, JSON.stringify(size));

    // scrolling
    await take();
    let lines = '';
    for (let i = 0; i < 200; i++) lines += 'line ' + i + '\r\n';
    await write(lines);
    msgs = await take();
    const ranges = msgs.filter(([n]) => n == 'newScrollRange').map(([, b]) => b);
    const tops = msgs.filter(([n]) => n == 'newScrollTop').map(([, b]) => b);
    const cellH = size[1][1];
    const len = await page.evaluate(() => [term.buffer.active.length, term.buffer.active.viewportY, term.buffer.active.baseY]);
    check('scroll range and top follow output', ranges.length > 0 && Math.abs(ranges[ranges.length - 1] - len[2] * cellH) < 0.5 &&
          tops.length > 0 && Math.abs(tops[tops.length - 1] - len[1] * cellH) < 0.5, JSON.stringify({ ranges: ranges.slice(-2), tops: tops.slice(-2), len, cellH }));
    await page.evaluate((y) => exports.newScrollTop(y), 10 * cellH);
    await page.waitForTimeout(50);
    msgs = await take();
    const vy = await page.evaluate(() => term.buffer.active.viewportY);
    check("the app's scroll moves it, not told back", vy == 10 && !msgs.some(([n]) => n == 'newScrollTop'), JSON.stringify({ vy, msgs }));
    await page.evaluate(() => exports.scrollToBottom());
    await page.waitForTimeout(50);
    msgs = await take();
    check('scrollToBottom told to the app', msgs.some(([n, b]) => n == 'newScrollTop' && Math.abs(b - len[2] * cellH) < 0.5), JSON.stringify(msgs));
    // The app's scroll view at its bottom (an offset of the whole range) is
    // the terminal's bottom: its last line, the cursor's, shown
    await page.evaluate((y) => exports.newScrollTop(y), 10 * cellH);
    await page.evaluate((y) => exports.newScrollTop(y), len[2] * cellH);
    await page.waitForTimeout(50);
    const atBottom = await page.evaluate(() => [term.buffer.active.viewportY, term.buffer.active.baseY]);
    check("the app's scroll at its bottom is the terminal's", atBottom[0] == atBottom[1], JSON.stringify(atBottom));

    // A full-screen program's screen (claude's): the app's drags are its
    // wheel, a notch a line of the finger's way
    await take();
    await write('\x1b[?1049h');
    msgs = await take();
    check('the alternate screen told to the app', msgs.some(([n, b]) => n == 'newWheelScrolling' && b === true), JSON.stringify(msgs));
    const mid = await page.evaluate(() => {
        const rect = document.querySelector('.xterm-screen').getBoundingClientRect();
        return [rect.left + rect.width / 2, rect.top + rect.height / 2];
    });
    const inputOf = (m) => m.filter(([n]) => n == 'sendInput').map(([, b]) => b).join('');
    await page.evaluate(([x, y, h]) => { exports.wheel(2.5 * h, x, y); exports.wheel(0.6 * h, x, y); }, [mid[0], mid[1], cellH]);
    let wheeled = inputOf(await take());
    check('a drag down, no mouse reports asked for: up arrows, a line each, parts added up',
          wheeled == '\x1b[A'.repeat(3), JSON.stringify(wheeled));
    await write('\x1b[?1000h\x1b[?1006h');
    await take();
    await page.evaluate(([x, y, h]) => exports.wheel(-2 * h, x, y), [mid[0], mid[1], cellH]);
    wheeled = inputOf(await take());
    check('a drag up, SGR mouse reports asked for (claude): the wheel turned down, at the finger',
          /^(\x1b\[<65;\d+;\d+M){2}$/.test(wheeled), JSON.stringify(wheeled));
    await page.evaluate(([x, y, h]) => exports.wheel(1.5 * h, x, y), [mid[0], mid[1], cellH]);
    wheeled = inputOf(await take());
    check('a drag down: the wheel turned up', /^\x1b\[<64;\d+;\d+M$/.test(wheeled), JSON.stringify(wheeled));
    await write('\x1b[?1006l\x1b[?1000l\x1b[?1049l');
    msgs = await take();
    check('the normal screen told to the app', msgs.some(([n, b]) => n == 'newWheelScrolling' && b === false), JSON.stringify(msgs));

    // selection and links, at points of the page
    await page.evaluate(() => exports.clearScrollback());
    await write('\x1b[H\x1b[2Jfoo bar/baz-1 qux\r\nsee https://example.com/a?b=1, ok\r\n');
    const pointOf = (col, row) => page.evaluate(([c, r]) => {
        const rect = document.querySelector('.xterm-screen').getBoundingClientRect();
        const [w, h] = exports.getCharacterSize();
        return [rect.left + (c + 0.5) * w, rect.top + (r + 0.5) * h];
    }, [col, row]);
    const vp = await page.evaluate(() => term.buffer.active.viewportY);
    const [sx, sy] = await pointOf(6, 0);
    await page.evaluate(([x, y]) => exports.selectWordAt(x, y), [sx, sy]);
    let sel = await page.evaluate(() => exports.getSelection());
    check('a long press selects the word', sel && sel.text == 'bar/baz-1', JSON.stringify(sel));
    const [ex, ey] = await pointOf(16, 0);
    await page.evaluate(([x, y]) => exports.extendSelectionTo(x, y), [ex, ey]);
    sel = await page.evaluate(() => exports.getSelection());
    check('dragged on, it grows', sel && sel.text == 'bar/baz-1 qux', JSON.stringify(sel));
    const [bx, by] = await pointOf(0, 0);
    await page.evaluate(([x, y]) => exports.extendSelectionTo(x, y), [bx, by]);
    sel = await page.evaluate(() => exports.getSelection());
    check('dragged back before the word, it keeps the word', sel && sel.text == 'foo bar/baz-1', JSON.stringify(sel));
    check('its rect is its row', sel && Math.abs(sel.rect[1] - (sy - size[1][1] / 2)) < 1 && Math.abs(sel.rect[3] - size[1][1]) < 1, JSON.stringify(sel && sel.rect));
    const native = await page.evaluate(() => { term.select(2, term.buffer.active.viewportY + 1, 3); return term.getSelectionPosition(); });
    console.log('  xterm getSelectionPosition after select(2, viewportY+1, 3): ' + JSON.stringify(native) + ', viewportY ' + vp);
    await page.evaluate(() => exports.clearSelection());

    await take();
    const [lx, ly] = await pointOf(10, 1);
    await page.evaluate(([x, y]) => exports.openLinkAt(x, y), [lx, ly]);
    msgs = await take();
    check('Cmd-click opens the link', msgs.some(([n, b]) => n == 'openLink' && b == 'https://example.com/a?b=1'), JSON.stringify(msgs));

    // a wrapped line: one line of text, a word across the wrap
    const cols = (await page.evaluate(() => exports.getSize()))[0];
    await write('\x1b[H\x1b[2J' + 'x'.repeat(cols - 3) + ' abcdef tail\r\n');
    text = await page.evaluate(() => exports.getText());
    check('a wrapped line is one line', text.includes('x'.repeat(cols - 3) + ' abcdef tail'), JSON.stringify(text.slice(-80)));
    const [wx, wy] = await pointOf(cols - 1, 0);
    await page.evaluate(([x, y]) => exports.selectWordAt(x, y), [wx, wy]);
    sel = await page.evaluate(() => exports.getSelection());
    check('a word across the wrap', sel && sel.text.replace(/\n/g, '') == 'abcdef', JSON.stringify(sel));

    // resize
    await take();
    await page.setViewportSize({ width: 600, height: 300 });
    await page.waitForTimeout(100);
    msgs = await take();
    check('a resize is told', msgs.some(([n]) => n == 'resize'), JSON.stringify(msgs));

    // clearScreen (the app's ⌘K): the line being typed stays, at the top, the
    // cursor where it was on it; nothing else on the screen
    const screen = () => page.evaluate(() => {
        const b = term.buffer.active;
        const rows = [];
        for (let i = 0; i < term.rows; i++) rows.push(b.getLine(b.baseY + i).translateToString(true));
        return { rows, x: b.cursorX, y: b.cursorY };
    });
    await page.evaluate(() => { exports.reset(); exports.clearScrollback(); });
    await write('one\r\ntwo\r\n$ abc\x1b[2D');
    await page.evaluate(() => exports.clearScreen());
    await page.waitForTimeout(50);
    let shown = await screen();
    check('clearScreen keeps the prompt line at the top, the cursor on it',
          shown.rows[0] == '$ abc' && shown.rows.slice(1).every((r) => r == '') && shown.x == 3 && shown.y == 0,
          JSON.stringify(shown));
    const width = await page.evaluate(() => term.cols);   // since the resize above
    await write('\r\n\r\nx\r\n$ ' + 'a'.repeat(width + 5));
    await page.evaluate(() => exports.clearScreen());
    await page.waitForTimeout(50);
    shown = await screen();
    check('clearScreen keeps all the rows of a wrapped line',
          shown.rows[0] == '$ ' + 'a'.repeat(width - 2) && shown.rows[1] == 'a'.repeat(7) &&
          shown.rows.slice(2).every((r) => r == '') && shown.x == 7 && shown.y == 1,
          JSON.stringify(shown));

    // serialize: what a restored tab shows, colors and all, not a full-screen
    // program's screen
    await page.evaluate(() => { exports.reset(); exports.clearScrollback(); });
    await write('plain \x1b[31mred\x1b[0m line\r\n$ ');
    let saved = await page.evaluate(() => exports.serialize());
    check('serialize keeps the text and its colors', saved.includes('plain') && saved.includes('red') && /\x1b\[[0-9;]*31m/.test(saved), JSON.stringify(saved));
    await write('\x1b[?1049hFULLSCREEN');
    saved = await page.evaluate(() => exports.serialize());
    check("serialize leaves out a full-screen program's screen", saved.includes('plain') && !saved.includes('FULLSCREEN'), JSON.stringify(saved));
    await write('\x1b[?1049l');

    await page.evaluate(() => { exports.setFocused(true); exports.setFocused(false); exports.reset(); exports.clearScreen(); exports.setAccessibilityEnabled(true); });
    msgs = await take();
    check('no errors', !msgs.some(([n, b]) => n == 'log' && /error/i.test(b)), JSON.stringify(msgs.filter(([n]) => n == 'log')));
    console.log(passed + ' passed, ' + failed + ' failed');
    await browser.close();
    process.exit(failed ? 1 : 0);
})().catch((e) => { console.log('harness error: ' + e.stack); process.exit(2); });

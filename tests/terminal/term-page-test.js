// The terminal's page (app/terminal: term.js on xterm.js) in headless
// Chromium, with a stand-in for the app's message handlers (Terminal.m,
// TerminalView.m): output in UTF-8 split anywhere, the answers to programs,
// bracketed paste, OSC 52, titles, the application cursor mode, sizes (and
// the size a font makes), the scroll view's sync both ways, Shift-Page Up,
// its drags as a full-screen program's wheel, long-press selection and its
// rect, a long press on a link, Cmd-click links, wrapped lines and wide
// characters, resizing, where the cells end (for the app's meters right of
// them), the bell and notifications, find, an input method's composition and
// the cursor's place, and no errors. WebKit is not Chromium,
// so this checks term.js, not the app.
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
                       'syncFocus', 'focus', 'newScrollRange', 'newScrollTop', 'newWheelScrolling', 'openLink',
                       'findResults', 'cursorRect', 'notify', 'screenEdge'];
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
        fontSize: 14, colorPaletteOverrides: null, blinkCursor: false, cursorShape: 'BEAM', minimumContrastRatio: 1 }));
    const style = await page.evaluate(() => [term.options.cursorStyle, term.options.theme.selectionBackground, term.options.fontSize]);
    check('updateStyle', style[0] == 'bar' && style[1] == 'rgba(255, 255, 255, 0.35)' && style[2] == 14, JSON.stringify(style));
    // a bigger font, fewer columns: the size it returns is the terminal's
    const zoomed = await page.evaluate(() => {
        const options = { foregroundColor: '#fff', backgroundColor: '#000', fontFamily: 'monospace',
                          colorPaletteOverrides: null, blinkCursor: false, cursorShape: 'BEAM', minimumContrastRatio: 1 };
        const big = exports.updateStyle({ ...options, fontSize: 28 });
        const back = exports.updateStyle({ ...options, fontSize: 14 });
        return { big, back, now: exports.getSize() };
    });
    const contrast = await page.evaluate(() => {
        exports.updateStyle({ foregroundColor: '#fff', backgroundColor: '#000', fontFamily: 'monospace', fontSize: 14,
                              colorPaletteOverrides: null, blinkCursor: false, cursorShape: 'BEAM', minimumContrastRatio: 4.5 });
        const raised = term.options.minimumContrastRatio;
        exports.updateStyle({ foregroundColor: '#fff', backgroundColor: '#000', fontFamily: 'monospace', fontSize: 14,
                              colorPaletteOverrides: null, blinkCursor: false, cursorShape: 'BEAM', minimumContrastRatio: 1 });
        return [raised, term.options.minimumContrastRatio];
    });
    check('Increase Contrast: the minimum contrast ratio, and back', contrast.join() == '4.5,1', JSON.stringify(contrast));
    check('updateStyle returns the size it makes', zoomed.big[0] < zoomed.back[0] && zoomed.back.join() == zoomed.now.join(),
          JSON.stringify(zoomed));
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
    await page.evaluate((y) => exports.newScrollTop(y), 100 * cellH);
    await page.evaluate(() => exports.scrollPages(-1));
    await page.waitForTimeout(50);
    const paged = await page.evaluate(() => [term.buffer.active.viewportY, term.rows]);
    check('Shift-Page Up: a page up, a line of it kept', paged[0] == 100 - (paged[1] - 1), JSON.stringify(paged));
    await page.evaluate(() => exports.scrollToTop());
    await page.waitForTimeout(50);
    check('Shift-Home: the top', (await page.evaluate(() => term.buffer.active.viewportY)) == 0, '');
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

    // a long press on a link: all of it, and what it opens, for the menu's Open Link
    await page.evaluate(([x, y]) => exports.selectWordAt(x, y), [lx, ly]);
    sel = await page.evaluate(() => exports.getSelection());
    check('a long press on a link selects it, ? and = too, the comma after not',
          sel && sel.text == 'https://example.com/a?b=1' && sel.link == 'https://example.com/a?b=1', JSON.stringify(sel));
    const [okx, oky] = await pointOf(32, 1);
    await page.evaluate(([x, y]) => exports.extendSelectionTo(x, y), [okx, oky]);
    sel = await page.evaluate(() => exports.getSelection());
    check('dragged on past it, it is no longer a link', sel && sel.text == 'https://example.com/a?b=1, ok' && sel.link === null, JSON.stringify(sel));
    await page.evaluate(([x, y]) => exports.selectWordAt(x, y), [sx, sy]);
    sel = await page.evaluate(() => exports.getSelection());
    check('a word is not a link', sel && sel.text == 'bar/baz-1' && sel.link === null, JSON.stringify(sel));
    await page.evaluate(() => term.select(0, term.buffer.active.viewportY + 1, 3));
    sel = await page.evaluate(() => exports.getSelection());
    check("another's selection is not the long press's", sel && sel.text == 'see' && sel.link === null, JSON.stringify(sel));
    await page.evaluate(() => exports.clearSelection());

    // after a wide character, across a wrap: the link's cells, not half of the character's
    const linkCols = (await page.evaluate(() => exports.getSize()))[0];
    const longUrl = 'https://example.com/' + 'p'.repeat(linkCols);
    await write(utf8('\x1b[H\x1b[2J链' + longUrl + ' x\r\n'));
    const [w1x, w1y] = await pointOf(linkCols - 1, 0);
    await page.evaluate(([x, y]) => exports.selectWordAt(x, y), [w1x, w1y]);
    sel = await page.evaluate(() => exports.getSelection());
    check('a link after a wide character, wrapped: all of it, nothing of the character',
          sel && sel.text.replace(/\n/g, '') == longUrl && sel.link == longUrl, JSON.stringify(sel));
    const [cx, cy] = await pointOf(1, 0);
    await take();
    await page.evaluate(([x, y]) => exports.openLinkAt(x, y), [cx, cy]);
    msgs = await take();
    check("the second half of the wide character is the character's, not the link's", !msgs.some(([n]) => n == 'openLink'), JSON.stringify(msgs));
    await page.evaluate(() => exports.clearSelection());

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

    // Where the cells end (screenEdge), told as the columns and the font change
    // it and again as the app attaches (resyncScroll): right of it, the room
    // the fit keeps for the scroll bar (14) and the page's margin (4), and
    // what is less than a column
    const edgeShown = () => page.evaluate(() => {
        const rect = term.element.querySelector('.xterm-screen').getBoundingClientRect();
        return { right: rect.right, cell: rect.width / term.cols, page: window.innerWidth };
    });
    const edgesIn = (list) => list.filter(([n]) => n == 'screenEdge').map(([, edge]) => edge);
    let shownEdge = await edgeShown();
    let edges = edgesIn(msgs);
    const room = shownEdge.page - shownEdge.right;
    check('the cells\' edge is told as the columns change', edges.length == 1 && edges[0] == shownEdge.right,
          JSON.stringify({ edges, shownEdge }));
    check('right of the cells: the scroll bar\'s room, the margin, less than a column',
          room >= 18 && room < 18 + shownEdge.cell, JSON.stringify({ room, shownEdge }));
    const fontOptions = { foregroundColor: '#fff', backgroundColor: '#000', fontFamily: 'monospace',
                          colorPaletteOverrides: null, blinkCursor: false, cursorShape: 'BEAM', minimumContrastRatio: 1 };
    await page.evaluate((options) => exports.updateStyle({ ...options, fontSize: 21 }), fontOptions);
    await page.waitForTimeout(100);
    msgs = await take();
    shownEdge = await edgeShown();
    edges = edgesIn(msgs);
    check('the cells\' edge is told as the font changes', edges.length == 1 && edges[0] == shownEdge.right,
          JSON.stringify({ edges, shownEdge }));
    await page.evaluate((options) => exports.updateStyle({ ...options, fontSize: 14 }), fontOptions);
    await page.waitForTimeout(100);
    await take();
    await page.evaluate(() => exports.resyncScroll());
    await page.waitForTimeout(100);
    msgs = await take();
    shownEdge = await edgeShown();
    edges = edgesIn(msgs);
    check('the cells\' edge is told again as the app attaches', edges.length == 1 && edges[0] == shownEdge.right,
          JSON.stringify({ edges, shownEdge }));

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

    // what a program asks to be told of: the bell, iTerm2's and ghostty's
    // notifications as Claude Code writes them; not ConEmu's progress
    await take();
    await write('\x07');
    await write('\x1b]9;Claude Code: Claude is waiting for your input\x07');
    await write('\x1b]9;4;3;\x07');
    await write('\x1b]777;notify;Claude Code;Claude needs your permission; to use Bash\x1b\\');
    await write('\x1b]777;preexec\x07');
    msgs = await take();
    const notices = msgs.filter(([n]) => n == 'notify').map(([, b]) => b);
    check('the bell, OSC 9 and OSC 777 notify told; OSC 9;4 progress not',
          JSON.stringify(notices) == JSON.stringify([['', ''], ['', 'Claude Code: Claude is waiting for your input'],
                                                     ['Claude Code', 'Claude needs your permission; to use Bash']]),
          JSON.stringify(notices));
    check('another OSC 777 logged with what it was', msgs.some(([n, b]) => n == 'log' && b.includes('"preexec"')), JSON.stringify(msgs));

    // find (the app's find bar): every match, the one found selected, which
    // of how many told to the app
    await page.evaluate(() => { exports.reset(); exports.clearScrollback(); });
    await write('alpha beta\r\nbeta gamma\r\nBETA\r\n');
    await take();
    const lastResult = async () => (await take()).filter(([n]) => n == 'findResults').map(([, b]) => b).pop();
    await page.evaluate(() => exports.find('beta', false, true));
    let found = await lastResult();
    let foundText = await page.evaluate(() => term.getSelection());
    check('find: every match, whatever its case, one of them found and selected',
          found && found[1] == 3 && found[0] >= 0 && foundText.toLowerCase() == 'beta', JSON.stringify({ found, foundText }));
    const first = found[0];
    await page.evaluate(() => exports.find('beta', false, false));
    found = await lastResult();
    check('find next: the next one', found && found[0] == (first + 1) % 3 && found[1] == 3, JSON.stringify(found));
    await page.evaluate(() => exports.find('beta', true, false));
    found = await lastResult();
    check('find previous: back to it', found && found[0] == first, JSON.stringify(found));
    await page.evaluate(() => exports.find('betaz', false, true));
    found = await lastResult();
    check('typed on to no match: none found', found && found[0] == -1 && found[1] == 0, JSON.stringify(found));
    await page.evaluate(() => exports.find('gamma', false, true));
    await page.waitForTimeout(100);   // drawn at the next frame
    const decorated = await page.evaluate(() => document.querySelectorAll('.xterm-decoration').length);
    await page.evaluate(() => exports.clearFind());
    const cleared = await page.evaluate(() => [term.hasSelection(), document.querySelectorAll('.xterm-decoration').length]);
    check('matches highlighted; clearFind takes them and the selection away', decorated > 0 && !cleared[0] && cleared[1] == 0,
          JSON.stringify({ decorated, cleared }));

    // an input method's composition, at the cursor; where the cursor is, told
    await write('$ ab');
    await page.waitForTimeout(50);
    msgs = await take();
    const told = msgs.filter(([n]) => n == 'cursorRect').map(([, b]) => b).pop();
    const expected = await page.evaluate(() => {
        const rect = document.querySelector('.xterm-screen').getBoundingClientRect();
        const [w, h] = exports.getCharacterSize();
        const b = term.buffer.active;
        return [rect.left + b.cursorX * w, rect.top + (b.baseY + b.cursorY - b.viewportY) * h, w, h];
    });
    check('where the cursor is, told to the app', told && told.every((v, i) => Math.abs(v - expected[i]) < 0.5),
          JSON.stringify({ told, expected }));
    const shownComposition = await page.evaluate(() => {
        exports.setComposition('ni hao');
        const c = document.getElementById('composition');
        const r = c.getBoundingClientRect();
        return { text: c.textContent, display: getComputedStyle(c).display, left: r.left, top: r.top };
    });
    check('a composition shows at the cursor', shownComposition.text == 'ni hao' && shownComposition.display == 'block' &&
          Math.abs(shownComposition.left - expected[0]) < 0.5 && Math.abs(shownComposition.top - expected[1]) < 0.5,
          JSON.stringify({ shownComposition, expected }));
    const hidden = await page.evaluate(() => { exports.setComposition(''); return getComputedStyle(document.getElementById('composition')).display; });
    check('and goes when it is typed or dropped', hidden == 'none', hidden);

    await page.evaluate(() => { exports.setFocused(true); exports.setFocused(false); exports.reset(); exports.clearScreen(); exports.setAccessibilityEnabled(true); });
    msgs = await take();
    check('no errors', !msgs.some(([n, b]) => n == 'log' && /error/i.test(b)), JSON.stringify(msgs.filter(([n]) => n == 'log')));
    console.log(passed + ' passed, ' + failed + ' failed');
    await browser.close();
    process.exit(failed ? 1 : 0);
})().catch((e) => { console.log('harness error: ' + e.stack); process.exit(2); });

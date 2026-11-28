// The terminal's page: xterm.js (xterm_all.js) drawing what the app gives it,
// and the bridge between the two.
//
// app -> page: exports.*, called by Terminal.m and TerminalView.m.
// page -> app: native.<name>(...), the message handlers of the Terminal and
// of the TerminalView showing it. Those of a TerminalView (focus, scrolling,
// links) are there only while one shows this terminal, so a message with no
// handler is dropped rather than thrown: a hidden terminal must keep taking
// output.
//
// The app has the keyboard (TerminalView): the page takes no keys, and its
// text field never has focus, so no keyboard of the page's shows. What
// xterm.js answers the programs (device attributes, cursor reports, mouse
// reports, pastes) goes to the terminal's input.
'use strict';

const native = new Proxy({}, {
    get(obj, prop) {
        return (...args) => {
            const handler = window.webkit.messageHandlers[prop];
            if (handler !== undefined)
                handler.postMessage(args.length == 0 ? null : args.length == 1 ? args[0] : args);
        };
    },
});
window.addEventListener('error', (e) => native.log('term.js error: ' + e.message + ' at ' + e.filename + ':' + e.lineno));

window.exports = {};

// The cursor shapes as the app's preferences name them (cursorShapeName)
const cursorStyles = {BLOCK: 'block', BEAM: 'bar', UNDERLINE: 'underline'};
// The 16 colors of the palette, in ANSI order, as xterm.js names them
const paletteNames = ['black', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white',
    'brightBlack', 'brightRed', 'brightGreen', 'brightYellow', 'brightBlue', 'brightMagenta', 'brightCyan', 'brightWhite'];

const term = new Terminal({
    allowProposedApi: true,
    allowTransparency: true,
    scrollback: 10000,
    fontFamily: 'Menlo',
    fontSize: 12,
    // Nothing shows until the app's colors come (updateStyle)
    theme: {background: '#00000000', foreground: '#00000000', cursor: '#00000000'},
    // The page is never focused (the app has the keyboard): the cursor is
    // drawn the same either way, setFocused saying whether it blinks
    cursorInactiveStyle: 'block',
    // Box drawing and block characters drawn to fill their cells, so frames
    // and block-character logos join up
    customGlyphs: true,
    rescaleOverlappingGlyphs: true,
    // OSC 8 links open in the app (a pointer's click on one)
    linkHandler: {
        activate: (event, uri) => openUrl(uri),
        allowNonHttpProtocols: true,
    },
});
window.term = term;

const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
term.loadAddon(new Unicode11Addon.Unicode11Addon());
term.unicode.activeVersion = '11';
const search = new SearchAddon.SearchAddon();
term.loadAddon(search);
term.open(document.getElementById('terminal'));

// WebGL draws it. A context lost (the app in the background a while) leaves
// xterm.js's DOM renderer drawing it, and WebGL comes back once the page shows
// again.
let webgl = null;
function loadWebgl() {
    if (webgl !== null)
        return;
    try {
        webgl = new WebglAddon.WebglAddon();
        webgl.onContextLoss(() => {
            native.log('term.js: WebGL context lost, drawing with the DOM renderer');
            webgl.dispose();
            webgl = null;
        });
        term.loadAddon(webgl);
    } catch (e) {
        native.log('term.js: no WebGL renderer (' + e + '), drawing with the DOM renderer');
        webgl = null;
    }
}
loadWebgl();
document.addEventListener('visibilitychange', () => {
    if (document.visibilityState == 'visible')
        loadWebgl();
});

// Touches are the app's (its scroll view scrolls the terminal, its long press
// selects): xterm.js's touch scrolling, its gestures on the document, gets
// none. A tap still comes to xterm.js as the mouse's click.
for (const type of ['touchstart', 'touchmove', 'touchend', 'touchcancel'])
    window.addEventListener(type, (e) => e.stopImmediatePropagation(), {capture: true});

// The page's text field takes no focus and no keys: the app's do
const textarea = term.textarea;
textarea.readOnly = true;
textarea.setAttribute('inputmode', 'none');
textarea.tabIndex = -1;
textarea.focus = () => {};
term.attachCustomKeyEventHandler(() => false);

let oldProps = {};
function syncProp(name, value) {
    if (oldProps[name] !== value) {
        oldProps[name] = value;
        native.propUpdate(name, value);
    }
}

// Output: the bytes as a string of Latin-1 characters, one per byte, which
// xterm.js decodes as UTF-8 (a character split between two writes too).
// Resolves once xterm.js has parsed them, which is when the app sends more.
exports.write = (data) => new Promise((resolve) => {
    const bytes = new Uint8Array(data.length);
    for (let i = 0; i < data.length; i++)
        bytes[i] = data.charCodeAt(i);
    term.write(bytes, () => {
        syncProp('applicationCursor', term.modes.applicationCursorKeysMode);
        resolve();
    });
});
term.onData((data) => native.sendInput(data));
term.onBinary((data) => native.sendBinary(data));

// A paste, bracketed if the program asked for that (term.paste): to the
// terminal's input
exports.paste = (text) => term.paste(text);

// xterm.js's size follows the page's, and the terminal's follows it
term.onResize(() => native.resize());
window.addEventListener('resize', () => fit.fit());
exports.getSize = () => [term.cols, term.rows];

// A program's title (OSC 0 or 2) names its tab (Terminal.windowTitle)
term.onTitleChange((title) => native.setTitle(title));

// OSC 52: a program's copy (tmux, vim, claude) goes to the clipboard. Only
// setting it: what the page reads back would be the program's own.
term.parser.registerOscHandler(52, (data) => {
    const semicolon = data.indexOf(';');
    if (semicolon < 0)
        return false;
    const payload = data.slice(semicolon + 1);
    if (payload == '?')
        return true;
    const binary = atob(payload);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++)
        bytes[i] = binary.charCodeAt(i);
    native.copyText(new TextDecoder().decode(bytes));
    return true;
});

// What a program asks to be told of (Terminal's attention: a mark on the tab,
// a notification while the app is away): the bell, and the notifications of
// iTerm2 (OSC 9, the text) and of ghostty (OSC 777, notify, a title and the
// text). Claude Code 2.1.331 writes each, as its preferredNotifChannel says:
// terminal_bell, iterm2 ("title: text", or the text), ghostty. OSC 9 is
// also ConEmu's progress (9;4;state;value), which claude writes as iterm2's
// where it takes a terminal to show one: not a notification.
term.onBell(() => native.notify(['', '']));
term.parser.registerOscHandler(9, (data) => {
    if (data.startsWith('4;'))
        return true;
    native.notify(['', data]);
    return true;
});
term.parser.registerOscHandler(777, (data) => {
    const parts = data.split(';');
    if (parts[0] != 'notify' || parts.length < 3) {
        native.log('term.js: OSC 777 other than notify;title;text, not shown: ' + JSON.stringify(data));
        return true;
    }
    native.notify([parts[1], parts.slice(2).join(';')]);
    return true;
});

// DEC mode 2031, as contour, kitty and ghostty have it: a program that sets it
// is told as the terminal turns dark or light (CSI ? 997 ; 1 n dark, 2 light;
// updateStyle), and CSI ? 996 n asks which it is. Claude Code 2.1.332's
// "auto" theme sets it and, told, asks the background again (OSC 11, which
// xterm.js answers from the theme): without it the system's appearance
// changed under a running claude was its own only at its next start.
let themeNotify = false;
// dark (updateStyle): undefined until the first style
let themeDark;
function reportTheme() {
    native.sendInput(`\x1b[?997;${themeDark ? 1 : 2}n`);
}
// set or reset with other modes: xterm.js's own handler goes on to them
term.parser.registerCsiHandler({prefix: '?', final: 'h'}, (params) => {
    if (params.includes(2031))
        themeNotify = true;
    return false;
});
term.parser.registerCsiHandler({prefix: '?', final: 'l'}, (params) => {
    if (params.includes(2031))
        themeNotify = false;
    return false;
});
term.parser.registerCsiHandler({prefix: '?', final: 'n'}, (params) => {
    if (params.length != 1 || params[0] != 996)
        return false;
    reportTheme();
    return true;
});
// a reset (RIS) resets it, as it does the modes xterm.js keeps
term.parser.registerEscHandler({final: 'c'}, () => {
    themeNotify = false;
    return false;
});

// focus: the app's. A tap on the terminal asks it for the keyboard; one while
// there is a selection is the selection's. Whether there was one is taken as
// the press goes down (capture), before xterm.js's own mousedown, on this
// element too and added first, has made the press a new selection: none.
// The app is told where the tap was, for it to give the keyboard to a tap in
// the input only where it knows one (TerminalView's inputTapsOnly): the line
// tapped, the first of the input's (the cursor's logical line, a command that
// wraps from the lines above it), both in the buffer's lines, and whether the
// alternate screen shows (a full-screen program's, whose input is anywhere).
let pressedOnSelection = false;
term.element.addEventListener('mousedown', () => {
    pressedOnSelection = term.hasSelection();
}, {capture: true});
term.element.addEventListener('mousedown', (e) => {
    if (pressedOnSelection)
        return;
    const buffer = term.buffer.active;
    const tapped = cellAt(e.clientX, e.clientY);
    let input = buffer.baseY + buffer.cursorY;
    while (input > 0 && buffer.getLine(input).isWrapped)
        input--;
    native.focus({line: tapped === null ? input : tapped.y, input, alternate: buffer.type === 'alternate'});
});
// Focused or not, as xterm.js takes it from its text field's events, which
// the field itself never has
exports.setFocused = (focus) => {
    textarea.dispatchEvent(new FocusEvent(focus ? 'focus' : 'blur'));
};

// Cells, in the page's points
function cellSize() {
    const screen = term.element.querySelector('.xterm-screen');
    const rect = screen.getBoundingClientRect();
    return {width: rect.width / term.cols, height: rect.height / term.rows, rect};
}
exports.getCharacterSize = () => {
    const cell = cellSize();
    return [cell.width, cell.height];
};

// Where the cells end, in the page's points (screenEdge): the app's meters
// (TerminalView's MonitorStripView) are in what the page leaves right of it,
// the room the fit keeps for xterm.js's scroll bar, which the app's scroll
// view shows instead (term.css), and what is less than a column. The cells
// change their size with the columns and the font.
let lastEdge;
function syncEdge() {
    const edge = cellSize().rect.right;
    if (edge === lastEdge)
        return;
    lastEdge = edge;
    native.screenEdge(edge);
}
new ResizeObserver(syncEdge).observe(term.element.querySelector('.xterm-screen'));

// scrolling: the app's scroll view stands for xterm.js's (ScrollbarView),
// whose range is the lines above the last screen (baseY) and whose offset is
// the top line's, in points. Its offset moves the terminal a line at a time;
// the terminal moving of itself (output, scrollToBottom) moves the scroll
// view. The range is the lines', not the buffer's height less the view's: the
// view is rarely a whole number of lines high, and with half a line or more
// left over the bottom of the scroll view came out a line above the bottom,
// the last line (the cursor's) under the keys over the keyboard.
// A full-screen program's screen (the alternate one) has no lines above it:
// there the app's drags are the program's wheel (newWheelScrolling, wheel).
let lastScrollRange, lastScrollTop, lastWheelScrolling;
// the line the app's scroll view put at the top, not to be told back to it
let appScrollLine = null;
function syncScroll() {
    const height = cellSize().height;
    const buffer = term.buffer.active;
    const wheelScrolling = buffer.type == 'alternate';
    if (wheelScrolling !== lastWheelScrolling)
        native.newWheelScrolling(wheelScrolling);
    lastWheelScrolling = wheelScrolling;
    const scrollRange = buffer.baseY * height;
    if (scrollRange != lastScrollRange)
        native.newScrollRange(scrollRange);
    lastScrollRange = scrollRange;
    if (buffer.viewportY === appScrollLine)
        return;
    appScrollLine = null;
    const scrollTop = buffer.viewportY * height;
    if (scrollTop != lastScrollTop)
        native.newScrollTop(scrollTop);
    lastScrollTop = scrollTop;
}
term.onScroll(syncScroll);
term.onWriteParsed(syncScroll);
term.onResize(syncScroll);
term.buffer.onBufferChange(syncScroll);
exports.newScrollTop = (y) => {
    const height = cellSize().height;
    if (height == 0)
        return;
    const line = Math.max(0, Math.round(y / height));
    appScrollLine = Math.min(line, term.buffer.active.baseY);
    lastScrollTop = y;
    term.scrollToLine(line);
};
exports.scrollToBottom = () => term.scrollToBottom();
// Shift with Page Up, Page Down and Home (TerminalView), as in xterm
exports.scrollPages = (pages) => term.scrollPages(pages);
exports.scrollToTop = () => term.scrollToTop();

// The app's drags in a full-screen program's screen (TerminalView's
// scrollPanned:), the finger's way down in points: the program's mouse wheel,
// a notch for each line the finger goes, turned up as the finger goes down (to
// what is above). xterm.js sends a notch as it does a wheel's: a report to a
// program that asked for the mouse's (claude, tmux, vim with mouse=a), at the
// cell under the finger; the arrow keys to one that did not (less, man). What
// is left of a line is kept for the next, unless the finger turns back.
let wheelPartial = 0;
exports.wheel = (dy, x, y) => {
    const cell = cellSize();
    if (cell.height == 0)
        return;
    if (Math.sign(dy) != Math.sign(wheelPartial))
        wheelPartial = 0;
    wheelPartial += dy;
    const notches = Math.trunc(wheelPartial / cell.height);
    wheelPartial -= notches * cell.height;
    const clientX = Math.min(Math.max(x, cell.rect.left), cell.rect.right - 1);
    const clientY = Math.min(Math.max(y, cell.rect.top), cell.rect.bottom - 1);
    for (let i = 0; i < Math.abs(notches); i++) {
        term.element.dispatchEvent(new WheelEvent('wheel', {
            deltaY: notches > 0 ? -1 : 1,
            deltaMode: WheelEvent.DOM_DELTA_LINE,
            clientX, clientY, bubbles: true, cancelable: true,
        }));
    }
};

// Called by the app when a TerminalView (re)attaches, since what it was told
// of the scroll, the cursor and the cells' edge while detached was dropped.
exports.resyncScroll = () => {
    lastScrollRange = lastScrollTop = lastWheelScrolling = undefined;
    appScrollLine = null;
    syncScroll();
    lastCursorRect = '';
    syncCursor();
    lastEdge = undefined;
    syncEdge();
};

// A color of the app's (#rgb or #rrggbb): its red, green and blue
function rgb(color) {
    let hex = color.slice(1);
    if (hex.length == 3)
        hex = hex.split('').map((digit) => digit + digit).join('');
    const value = parseInt(hex, 16);
    return [value >> 16, (value >> 8) & 255, value & 255];
}
// seen through: rgba()
function withAlpha(color, alpha) {
    return `rgba(${rgb(color).join(', ')}, ${alpha})`;
}
// fraction of the way to another: #rrggbb, the only form xterm.js's
// decorations take
function mix(color, other, fraction) {
    const [a, b] = [rgb(color), rgb(other)];
    return '#' + a.map((c, i) => Math.round(c + (b[i] - c) * fraction).toString(16).padStart(2, '0')).join('');
}
// the matches of a find, on the terminal's background
const findColor = '#ffd60a';

// minimumContrastRatio: 4.5 with the system's Increase Contrast on (colors
// lightened or darkened against their background to read), else 1
exports.updateStyle = ({foregroundColor, backgroundColor, fontFamily, fontSize, colorPaletteOverrides, blinkCursor, cursorShape,
                        minimumContrastRatio}) => {
    const theme = {
        foreground: foregroundColor,
        background: backgroundColor,
        cursor: foregroundColor,
        cursorAccent: backgroundColor,
        selectionBackground: withAlpha(foregroundColor, 0.35),
    };
    if (colorPaletteOverrides)
        colorPaletteOverrides.forEach((color, i) => theme[paletteNames[i]] = color);
    term.options.theme = theme;
    // dark as its background is, by luminance (Rec. 709), which a program
    // told of it reads back with OSC 11 (mode 2031)
    const [red, green, blue] = rgb(backgroundColor);
    const dark = 0.2126 * red + 0.7152 * green + 0.0722 * blue < 128;
    const turned = themeDark !== undefined && dark != themeDark;
    themeDark = dark;
    if (turned && themeNotify)
        reportTheme();
    term.options.fontFamily = fontFamily;
    term.options.fontSize = fontSize;
    term.options.cursorBlink = blinkCursor;
    term.options.cursorStyle = cursorStyles[cursorShape];
    term.options.minimumContrastRatio = minimumContrastRatio;
    fit.fit();
    syncScroll();
    syncCursor();
    findDecorations = {
        matchBackground: mix(backgroundColor, findColor, 0.3),
        matchOverviewRuler: findColor,
        activeMatchBackground: mix(backgroundColor, findColor, 0.6),
        activeMatchBorder: findColor,
        activeMatchColorOverviewRuler: findColor,
    };
    // the size it now has, which the app shows as the font changes
    return [term.cols, term.rows];
};

exports.clearScrollback = () => term.clear();
// The screen cleared but for the line being typed (the prompt and what follows
// it, all its rows when it wraps), which goes to the top with the cursor where
// it was on it: the program reading it is not told (^L would be its input, a
// form feed in a file `cat >` writes), and a shell would draw its prompt again
// only at the next line.
exports.clearScreen = () => {
    const buffer = term.buffer.active;
    const y = buffer.cursorY;
    let first = y, last = y;
    while (first > 0 && buffer.getLine(buffer.baseY + first).isWrapped)
        first--;
    while (last + 1 < term.rows && buffer.getLine(buffer.baseY + last + 1).isWrapped)
        last++;
    let seq = '';
    if (first > 0)
        seq += `\x1b[${first}S`;                      // the rows above it scrolled away
    if (last - first + 1 < term.rows)
        seq += `\x1b[${last - first + 2};1H\x1b[J`;   // the rows below it erased
    seq += `\x1b[${y - first + 1};${buffer.cursorX + 1}H`;
    term.write(seq);
};
exports.reset = () => term.reset();
exports.setAccessibilityEnabled = (enabled) => term.options.screenReaderMode = enabled;
// hterm's, for VoiceOver's announcements: xterm.js's screen reader mode needs none
exports.setUserGesture = () => {};

// The text of a line, and where in it each of its cells is: a wide character
// takes two cells and one place, both its cells at that place; a cell of
// nothing is a space
function lineText(line) {
    let text = '';
    const index = [];
    for (let x = 0; x < term.cols; x++) {
        const cell = line.getCell(x);
        if (cell === undefined) {
            index.push(text.length);
            text += ' ';
            continue;
        }
        if (cell.getWidth() == 0) {
            index.push(index[x - 1]);
            continue;
        }
        index.push(text.length);
        text += cell.getChars() || ' ';
    }
    return {text, index};
}

// The buffer's text, its wrapped lines joined
exports.getText = () => {
    const buffer = term.buffer.active;
    const lines = [];
    for (let y = 0; y < buffer.length; y++) {
        const line = buffer.getLine(y);
        // a line that wraps into the next keeps its last spaces
        const wraps = y + 1 < buffer.length && buffer.getLine(y + 1).isWrapped;
        const text = line.translateToString(!wraps);
        if (line.isWrapped && lines.length > 0)
            lines[lines.length - 1] += text;
        else
            lines.push(text);
    }
    return lines.join('\n');
};

// The cell at a point of the page, in the buffer: x its column, y its line.
// null before the page is laid out, its cells of no size.
function cellAt(x, y) {
    const cell = cellSize();
    if (cell.width == 0 || cell.height == 0)
        return null;
    const column = Math.min(term.cols - 1, Math.max(0, Math.floor((x - cell.rect.left) / cell.width)));
    const row = Math.min(term.rows - 1, Math.max(0, Math.floor((y - cell.rect.top) / cell.height)));
    return {x: column, y: term.buffer.active.viewportY + row};
}

// The logical line a buffer line is part of: the line it wraps from and the
// ones that wrap from it, as one text, and where each line's cells are in it
function logicalLine(y) {
    const buffer = term.buffer.active;
    let first = y, last = y;
    while (first > 0 && buffer.getLine(first).isWrapped)
        first--;
    while (last + 1 < buffer.length && buffer.getLine(last + 1).isWrapped)
        last++;
    let text = '';
    const cells = [];
    for (let line = first; line <= last; line++) {
        const row = lineText(buffer.getLine(line));
        cells.push(row.index.map((i) => text.length + i));
        text += row.text;
    }
    return {first, text, cells};
}

// links: a Cmd-click opens the link at a point of the page. OSC 8 links
// carry their URL (linkHandler); otherwise the URL is found in the text,
// following it across rows the terminal wrapped. A click can reach both, so
// a repeat of the same URL is dropped.
let lastOpened, lastOpenedTime = 0;
function openUrl(url) {
    const now = Date.now();
    if (url === lastOpened && now - lastOpenedTime < 1000)
        return;
    lastOpened = url;
    lastOpenedTime = now;
    native.openLink(url);
}
const urlPattern = /(?:[a-zA-Z][a-zA-Z0-9+.-]*:\/\/|mailto:|www\.)[^\s<>"'`]+/g;
// The URL the text shows at a cell of the buffer, across the rows the
// terminal wrapped it in: what it opens, and its first and last cells
// (cellIndex). null if the cell is not in one.
function linkAt(cell) {
    const line = logicalLine(cell.y);
    const offset = line.cells[cell.y - line.first][cell.x];
    for (const match of line.text.matchAll(urlPattern)) {
        const url = match[0].replace(/[.,;:!?)\]}]+$/, '');
        const start = match.index, end = match.index + url.length;
        if (offset < start || offset >= end)
            continue;
        let from = Infinity, to = -1;
        line.cells.forEach((row, r) => row.forEach((place, x) => {
            if (place >= start && place < end) {
                const index = (line.first + r) * term.cols + x;
                from = Math.min(from, index);
                to = Math.max(to, index);
            }
        }));
        return {url: url.startsWith('www.') ? 'http://' + url : url, from, to};
    }
    return null;
}
exports.openLinkAt = (x, y) => {
    const cell = cellAt(x, y);
    if (cell === null)
        return;
    const link = linkAt(cell);
    if (link !== null)
        openUrl(link.url);
};

// selection, by the app's long press (TerminalView): the word under it, or
// the whole URL if it is in one, then as far as the finger goes, either way
// of it. Cells are counted along the buffer, line after line; a word goes on
// into a line it wraps into.
const wordBreaks = ' \t\'"`()[]{}<>|;';
// the word long pressed: its first and last cells, and for a URL, what it opens
let anchor = null;
let selected = null; // what this selected: its first and last cells
function cellIndex(cell) {
    return cell.y * term.cols + cell.x;
}
// xterm.js tells of a selection as it makes it (onSelectionChange): one
// this makes is this one's, any other (a pointer's drag, a find) not
let selecting = false;
function selectCells(from, to) {
    selected = {from, to};
    selecting = true;
    term.select(from % term.cols, Math.floor(from / term.cols), to - from + 1);
    selecting = false;
}
// The character a cell shows: '' for the second half of a wide one
function charAt(index) {
    const line = term.buffer.active.getLine(Math.floor(index / term.cols));
    const cell = line && line.getCell(index % term.cols);
    if (cell === undefined)
        return ' ';
    if (cell.getWidth() == 0)
        return '';
    return cell.getChars() || ' ';
}
function inWord(index) {
    const char = charAt(index);
    return char === '' || (char !== ' ' && !wordBreaks.includes(char));
}
// Whether two cells next to each other are of one line: of one row, or of
// rows the later of which the earlier wraps into
function joined(a, b) {
    const later = Math.max(Math.floor(a / term.cols), Math.floor(b / term.cols));
    return Math.floor(a / term.cols) == Math.floor(b / term.cols) || term.buffer.active.getLine(later).isWrapped;
}
exports.selectWordAt = (x, y) => {
    const cell = cellAt(x, y);
    if (cell === null)
        return;
    const link = linkAt(cell);
    if (link !== null) {
        anchor = link;
        selectCells(link.from, link.to);
        return;
    }
    const at = cellIndex(cell);
    const cells = term.buffer.active.length * term.cols;
    let from = at, to = at;
    if (inWord(at)) {
        while (from > 0 && joined(from - 1, from) && inWord(from - 1))
            from--;
        while (to + 1 < cells && joined(to, to + 1) && inWord(to + 1))
            to++;
    }
    anchor = {from, to};
    selectCells(from, to);
};
exports.extendSelectionTo = (x, y) => {
    const cell = cellAt(x, y);
    if (anchor === null || cell === null)
        return;
    const at = cellIndex(cell);
    selectCells(Math.min(at, anchor.from), Math.max(at, anchor.to));
};
term.onSelectionChange(() => {
    if (!selecting)
        anchor = selected = null;
});
// What is selected, where its rows are in the page (where the app shows its
// menu), and the URL it opens if it is a URL long pressed and no more
exports.getSelection = () => {
    if (!term.hasSelection())
        return null;
    let first, last;
    if (selected !== null) {
        first = Math.floor(selected.from / term.cols);
        last = Math.floor(selected.to / term.cols);
    } else {
        // xterm.js's own (a pointer's drag): its rows
        const position = term.getSelectionPosition();
        first = position.start.y;
        last = position.end.y;
    }
    const cell = cellSize();
    const viewport = term.buffer.active.viewportY;
    const top = cell.rect.top + (first - viewport) * cell.height;
    const bottom = cell.rect.top + (last - viewport + 1) * cell.height;
    const link = selected !== null && anchor.url !== undefined && selected.from == anchor.from && selected.to == anchor.to
        ? anchor.url : null;
    return {text: term.getSelection(), rect: [cell.rect.left, top, cell.rect.width, bottom - top], link};
};
exports.clearSelection = () => term.clearSelection();

// find in what the terminal holds (the app's find bar): every match
// highlighted, the next or the previous one selected and scrolled to; as the
// text is typed (incremental), the one found stays while it still matches.
// Which of how many it is goes to the app (findResults: -1 for none found),
// again as output changes the matches.
let findDecorations;
exports.find = (text, backwards, incremental) => {
    const options = {caseSensitive: false, incremental, decorations: findDecorations};
    if (backwards)
        search.findPrevious(text, options);
    else
        search.findNext(text, options);
};
search.onDidChangeResults(({resultIndex, resultCount}) => native.findResults([resultIndex, resultCount]));
exports.clearFind = () => {
    search.clearDecorations();
    term.clearSelection();
};

// What an input method is putting together (the app's marked text: pinyin
// before it is characters), drawn at the cursor and underlined until it is
// typed or dropped. Where the cursor is goes to the app (cursorRect, in the
// page's points), for the input method's candidates to be by it.
const composition = document.createElement('div');
composition.id = 'composition';
document.body.appendChild(composition);
function cursorCell() {
    const cell = cellSize();
    const buffer = term.buffer.active;
    const row = buffer.baseY + buffer.cursorY - buffer.viewportY;
    return [cell.rect.left + buffer.cursorX * cell.width, cell.rect.top + row * cell.height, cell.width, cell.height];
}
function placeComposition() {
    if (composition.textContent == '')
        return;
    const [x, y, , height] = cursorCell();
    const right = cellSize().rect.right;
    composition.style.top = y + 'px';
    composition.style.height = composition.style.lineHeight = height + 'px';
    composition.style.left = Math.max(0, Math.min(x, right - composition.offsetWidth)) + 'px';
}
exports.setComposition = (text) => {
    composition.textContent = text;
    composition.style.display = text == '' ? 'none' : 'block';
    if (text == '')
        return;
    composition.style.fontFamily = term.options.fontFamily;
    composition.style.fontSize = term.options.fontSize + 'px';
    composition.style.color = term.options.theme.foreground;
    composition.style.background = term.options.theme.background;
    term.scrollToBottom();
    placeComposition();
};
let lastCursorRect = '', cursorFrame = null;
function syncCursor() {
    if (cursorFrame !== null)
        return;
    cursorFrame = requestAnimationFrame(() => {
        cursorFrame = null;
        placeComposition();
        const rect = cursorCell();
        if (rect.join() == lastCursorRect)
            return;
        lastCursorRect = rect.join();
        native.cursorRect(rect);
    });
}
term.onCursorMove(syncCursor);
term.onScroll(syncCursor);
term.onResize(syncCursor);

fit.fit();
native.load();
native.syncFocus();

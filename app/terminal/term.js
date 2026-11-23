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

// focus: the app's. A tap on the terminal gives it the keyboard; one while
// there is a selection is the selection's.
term.element.addEventListener('mousedown', () => {
    if (!term.hasSelection())
        native.focus();
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

// scrolling: the app's scroll view stands for xterm.js's (ScrollbarView),
// whose height is the lines' and whose offset is the top line's, in points.
// Its offset moves the terminal a line at a time; the terminal moving of
// itself (output, scrollToBottom) moves the scroll view.
let lastScrollHeight, lastScrollTop;
// the line the app's scroll view put at the top, not to be told back to it
let appScrollLine = null;
function syncScroll() {
    const height = cellSize().height;
    const buffer = term.buffer.active;
    const scrollHeight = buffer.length * height;
    if (scrollHeight != lastScrollHeight)
        native.newScrollHeight(scrollHeight);
    lastScrollHeight = scrollHeight;
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
// Called by the app when a TerminalView (re)attaches, since scroll updates
// sent while detached were dropped.
exports.resyncScroll = () => {
    lastScrollHeight = lastScrollTop = undefined;
    appScrollLine = null;
    syncScroll();
};

// A color of the app's (#rgb or #rrggbb), seen through: rgba()
function withAlpha(color, alpha) {
    let hex = color.slice(1);
    if (hex.length == 3)
        hex = hex.split('').map((digit) => digit + digit).join('');
    const value = parseInt(hex, 16);
    return `rgba(${value >> 16}, ${(value >> 8) & 255}, ${value & 255}, ${alpha})`;
}

exports.updateStyle = ({foregroundColor, backgroundColor, fontFamily, fontSize, colorPaletteOverrides, blinkCursor, cursorShape}) => {
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
    term.options.fontFamily = fontFamily;
    term.options.fontSize = fontSize;
    term.options.cursorBlink = blinkCursor;
    term.options.cursorStyle = cursorStyles[cursorShape];
    fit.fit();
    syncScroll();
};

exports.clearScrollback = () => term.clear();
exports.clearScreen = () => term.write('\x1b[H\x1b[2J');
exports.reset = () => term.reset();
exports.setAccessibilityEnabled = (enabled) => term.options.screenReaderMode = enabled;
// hterm's, for VoiceOver's announcements: xterm.js's screen reader mode needs none
exports.setUserGesture = () => {};

// The text of a line, and where in it each of its cells is: a wide character
// takes two cells and one place, a cell of nothing is a space
function lineText(line) {
    let text = '';
    const index = [];
    for (let x = 0; x < term.cols; x++) {
        const cell = line.getCell(x);
        index.push(text.length);
        if (cell === undefined) {
            text += ' ';
            continue;
        }
        if (cell.getWidth() == 0)
            continue;
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
exports.openLinkAt = (x, y) => {
    const cell = cellAt(x, y);
    if (cell === null)
        return;
    const line = logicalLine(cell.y);
    const offset = line.cells[cell.y - line.first][cell.x];
    for (const match of line.text.matchAll(urlPattern)) {
        const url = match[0].replace(/[.,;:!?)\]}]+$/, '');
        if (offset >= match.index && offset <= match.index + url.length) {
            openUrl(url.startsWith('www.') ? 'http://' + url : url);
            return;
        }
    }
};

// selection, by the app's long press (TerminalView): the word under it, then
// as far as the finger goes, either way of the word. Cells are counted along
// the buffer, line after line; a word goes on into a line it wraps into.
const wordBreaks = ' \t\'"`()[]{}<>|;';
let anchor = null; // the word long pressed: its first and last cells
let selected = null; // what this selected: its first and last cells
function cellIndex(cell) {
    return cell.y * term.cols + cell.x;
}
function selectCells(from, to) {
    selected = {from, to};
    term.select(from % term.cols, Math.floor(from / term.cols), to - from + 1);
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
    if (!term.hasSelection())
        anchor = selected = null;
});
// What is selected, and where its rows are in the page: where the app shows
// its menu (Copy)
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
    return {text: term.getSelection(), rect: [cell.rect.left, top, cell.rect.width, bottom - top]};
};
exports.clearSelection = () => term.clearSelection();

// find in what the terminal holds
exports.findNext = (text) => search.findNext(text);
exports.findPrevious = (text) => search.findPrevious(text);
exports.clearFind = () => search.clearDecorations();

fit.fit();
native.load();
native.syncFocus();

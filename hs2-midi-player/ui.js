/*
 * HS2 MIDI Player: on-device UI (a Schwung interactive tool, full screen).
 *
 * Plays MIDI files on the Hypersonic 2 engine's 16 parts (part N = channel N) and MIDI from a USB device the same
 * way. Every screen change, menu move and value change is spoken through Schwung's screen reader.
 *
 * Player screen:  Play = play / stop (Follow Move: presses Move's Play), Shift+Play = back to the start,
 *                 Loop = loop on / off, Jog = one bar back / on, Shift+Jog = 8 bars, Click or Menu = menu,
 *                 Shift+Click = speak the status.
 * Pads:           the bottom two rows are parts 1-16 (bottom row 1-8): press = mute, Shift+press = solo.
 *                 Off = not in the file, dim = in the file, bright = playing, red = muted, yellow = solo.
 * Knobs 1-8:      level of parts 1-8, with Shift parts 9-16. Touching a knob speaks its part and level.
 * Menus:          Jog = move, Click = open / edit / run, Menu = finish editing / up one level.
 * Back:           the host hides the tool and it keeps playing (suspend_keeps_js); Shift+Back exits it.
 *
 * DSP keys: README.md ("DSP keys"). One set_param per user action (Schwung's param channel holds one request).
 */

import * as os from 'os';
import { isCapacitiveTouchMessage, decodeDelta, decodeAcceleratedDelta, setLED, setButtonLED } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';
import {
    buildFilepathBrowserState,
    refreshFilepathBrowser,
    moveFilepathBrowserSelection,
    activateFilepathBrowserItem
} from '/data/UserData/schwung/shared/filepath_browser.mjs';

/* Move controls (shared/constants.mjs values) */
const CC_JOG = 14, CC_JOG_CLICK = 3, CC_SHIFT = 49, CC_MENU = 50, CC_PLAY = 85, CC_LOOP = 58;
const KNOB_CCS = [71, 72, 73, 74, 75, 76, 77, 78];
const KNOB_TOUCH_NOTES = [0, 1, 2, 3, 4, 5, 6, 7];
const PAD_FIRST = 68;          /* bottom-left pad; the bottom two rows are parts 1-16 */
const PARTS = 16;
const LIBRARY_ROOT = '/data/UserData/UserLibrary';
const MIDI_EXTENSIONS = ['.mid', '.midi', '.kar', '.rmi'];

/* pad colours (shared/constants.mjs palette) */
const LED_OFF = 0, LED_DIM = 124, LED_USED = 118, LED_PLAY = 120, LED_MUTE = 127, LED_SOLO = 7, LED_PLAY_GREEN = 8;  /* Vivid Yellow = 7 */

const W = 128;
const CHARS = 21;

/* ── DSP glue ─────────────────────────────────────────────────────── */

function gp(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? '' : String(v);
}
function sp(key, value) {
    host_module_set_param(key, String(value));
}
function gj(key, def) {
    try {
        const s = gp(key);
        return s ? JSON.parse(s) : def;
    } catch (e) {
        return def;
    }
}

/* ── speech ───────────────────────────────────────────────────────── */

function speakable(text) {
    return String(text)
        .replace(/\.mid[i]?\b/gi, '')
        .replace(/(^|[\s(,:\[])-(?=\.?\d)/g, '$1minus ')
        .replace(/%/g, ' percent')
        .replace(/_/g, ' ')
        .replace(/\s+/g, ' ')
        .trim();
}
function say(text) {
    if (text) announce(speakable(text));
}

/* ── state ────────────────────────────────────────────────────────── */

let view = 'home';          // 'home' | 'menu' | 'browser'
let stack = [];             // menus: {title, items, sel, build}
let editing = false;
let shift = false;
let needsRedraw = true;
let tickN = 0;
let ready = 0;              // 0 scanning, 1 ready, -1 no content
let lastStatus = '';
let song = { loaded: 0 };
let pos = { play: 0 };
let parts = [];
let cfg = { tempo: 0, gm: 1, usb: 1, loop: 0, preload: 1, ringout: 1, master: 0.5, lastdir: '' };
let cpu = [0, 0, 0];
let browser = null;
let overlay = null;         // {label, value, ticks}
let lastKnob = { idx: -1, at: 0 };
let announcedSong = '';     // the file whose "ready" was spoken
let wasPlaying = false, wasEnd = false, wasPrep = 0;
let padLeds = new Array(PARTS).fill(-1);
let injectQueue = [];       // Move button presses to send (Follow Move), one per 50 ms
let lastInject = 0;

/* ── formatting ───────────────────────────────────────────────────── */

function clock(sec) {
    sec = Math.max(0, Math.floor(sec || 0));
    const m = Math.floor(sec / 60), s = sec % 60;
    return `${m}:${s < 10 ? '0' : ''}${s}`;
}
function spokenTime(sec) {
    sec = Math.max(0, Math.round(sec || 0));
    const m = Math.floor(sec / 60), s = sec % 60;
    if (m === 0) return `${s} seconds`;
    return `${m} minute${m === 1 ? '' : 's'}${s ? ` ${s} seconds` : ''}`;
}
function songName() {
    if (!song.loaded) return 'No file';
    return song.title || String(song.name || '').replace(/\.(mid|midi|kar|rmi)$/i, '');
}
function partName(p) {
    const i = parts[p];
    if (!i) return `Part ${p + 1}`;
    if (i.missing && i.name === 'empty') return i.missing;
    return i.name || 'empty';
}
function partSpeech(p) {
    const i = parts[p] || {};
    let s = `Part ${p + 1}, ${partName(p)}`;
    if (i.solo) s += ', solo';
    if (i.mute) s += ', muted';
    if (i.lock) s += ', locked';
    if (!i.used) s += ', not in the file';
    return s;
}
function percent(v) {
    return `${Math.round((v || 0) * 100)}%`;
}
function panText(v) {
    const c = Math.round(((v || 0.5) - 0.5) * 200);
    if (Math.abs(c) < 1) return 'centre';
    return c < 0 ? `${-c}% left` : `${c}% right`;
}
function positionSpeech() {
    if (!song.loaded) return 'No file open';
    let s = `Bar ${pos.bar || 1} of ${pos.bars || song.bars}`;
    if (pos.beat > 1) s += `, beat ${pos.beat}`;
    return s;
}
function stateWord() {
    if (pos.prep > 0) return 'Preparing';
    if (pos.wait) return 'Waiting for the bar';
    if (pos.play) return 'Playing';
    return pos.end ? 'At the end' : 'Stopped';
}
function statusSpeech() {
    if (ready !== 1) return lastStatus || 'Starting';
    if (!song.loaded) return `No file open. Click for the menu, then Open MIDI File.${song.error ? ' ' + song.error : ''}`;
    const tempo = cfg.tempo ? `following Move at ${Math.round(pos.bpm || 0)} BPM` : `${Math.round(pos.bpm || 0)} BPM`;
    return `${songName()}. ${stateWord()}, ${positionSpeech()}, ${clock(pos.sec)} of ${clock(pos.len)}, ${tempo}. ` +
        `CPU ${cpu[0]} percent, peak ${cpu[1]}, ${cpu[2]} voices.`;
}

/* ── polling ──────────────────────────────────────────────────────── */

function refreshPos() {
    const p = gj('pos', null);
    if (p) pos = p;
}
function refreshParts() {
    const p = gj('parts', null);
    if (Array.isArray(p)) parts = p;
}
function refreshCfg() {
    const c = gj('cfg', null);
    if (c) cfg = c;
}
function refreshSong() {
    const s = gj('song', null);
    if (s) song = s;
}
function refreshCpu() {
    const c = gp('cpu').split(',').map(x => parseInt(x, 10) || 0);
    if (c.length >= 3) cpu = c;
}
function refreshAll() {
    refreshSong();
    refreshPos();
    refreshParts();
    refreshCfg();
    refreshCpu();
}

/* ── transport ────────────────────────────────────────────────────── */

function queueMovePlay() {
    if (typeof move_midi_inject_to_move !== 'function') return false;
    injectQueue.push([0x0B, 0xB0, CC_PLAY, 127]);
    injectQueue.push([0x0B, 0xB0, CC_PLAY, 0]);
    return true;
}
function drainInject() {
    if (!injectQueue.length) return;
    const now = Date.now();
    if (now - lastInject < 50) return;
    lastInject = now;
    try { move_midi_inject_to_move(injectQueue.shift()); } catch (e) { injectQueue = []; }
}

function togglePlay() {
    if (ready !== 1) return say(lastStatus || 'Starting');
    if (!song.loaded) return say('No file open. Click for the menu, then Open MIDI File.');
    if (cfg.tempo) {
        if (queueMovePlay()) say(pos.play ? "Stopping Move's transport" : "Starting Move's transport");
        else say("Following Move: press Move's Play button");
        return;
    }
    sp('transport', 'toggle');
    refreshPos();
    if (pos.prep > 0) say(`Preparing ${pos.prep} patches. Plays when ready.`);
    else if (pos.play) say(`Playing from ${positionSpeech().toLowerCase()}`);
    else say(`Stopped at ${positionSpeech().toLowerCase()}`);
    wasPlaying = !!pos.play;
    needsRedraw = true;
}
function toStart() {
    if (!song.loaded) return say('No file open');
    sp('transport', 'rewind');
    refreshPos();
    say(`Back to the start${pos.play ? ', playing' : ''}`);
    needsRedraw = true;
}
function toggleLoop() {
    sp('cfg:loop', 'toggle');
    refreshCfg();
    say(cfg.loop ? 'Loop on' : 'Loop off');
    needsRedraw = true;
}
function stepBars(d) {
    if (!song.loaded) return say('No file open');
    sp('step_bar', d);
    refreshPos();
    say(positionSpeech());
    needsRedraw = true;
}

/* ── menus ────────────────────────────────────────────────────────── */

function sub(label, build) {
    return { kind: 'sub', label, build };
}
function action(label, run) {
    return { kind: 'action', label, run };
}
function info(label, get) {
    return { kind: 'info', label, get };
}
/* a value: get() gives its text, step(d) changes it; a toggle flips on any step */
function value(label, get, step, toggle) {
    return { kind: 'value', label, get, step, toggle: !!toggle };
}
function cfgToggle(label, key, onText, offText) {
    return value(label, () => (cfg[key] ? onText : offText), () => {
        sp(`cfg:${key}`, 'toggle');
        refreshCfg();
    }, true);
}

function rootMenu() {
    return {
        title: 'HS2 MIDI Player',
        items: [
            action('Open MIDI File', openBrowser),
            action(pos.play ? 'Stop' : 'Play', () => {
                togglePlay();
                rebuild(cur());
            }),
            action('Back to Start', toStart),
            cfgToggle('Loop', 'loop', 'on', 'off'),
            sub('Parts', partsMenu),
            cfgToggle('Tempo', 'tempo', 'Follow Move', 'from the file'),
            cfgToggle('Program Changes', 'gm', 'General MIDI', 'Hypersonic banks'),
            cfgToggle('USB MIDI Input', 'usb', 'on', 'off'),
            value('Master Volume', () => percent(cfg.master), d => {
                sp('cfg:master', (d > 0 ? '+' : '') + d);
                refreshCfg();
            }),
            cfgToggle('Preload Samples', 'preload', 'on', 'off'),
            cfgToggle('Effect Tails', 'ringout', 'ring out', 'cut'),
            info('File Info', () => fileInfoText()),
            info('Status', () => { refreshCpu(); return `CPU ${cpu[0]}%, peak ${cpu[1]}%, ${cpu[2]} voices`; }),
            action('All Notes Off', () => {
                sp('panic', '1');
                say('All notes off');
            }),
            action('Rescan Content', () => {
                sp('rescan', '1');
                ready = 0;
                say('Rescanning content');
            })
        ]
    };
}

function fileInfoText() {
    if (!song.loaded) return song.error ? `No file: ${song.error}` : 'No file open';
    return `${song.name}, ${song.tracks} tracks, ${song.parts} parts, ${song.bars} bars, ` +
        `${spokenTime(song.len)}, starts at ${Math.round(song.bpm)} BPM`;
}

function partsMenu() {
    refreshParts();
    const items = [];
    for (let p = 0; p < PARTS; p++) {
        items.push(sub(`${p + 1} ${partName(p)}`, () => partMenu(p)));
    }
    return { title: 'Parts', items };
}

function partMenu(p) {
    const pi = () => parts[p] || {};
    return {
        title: `Part ${p + 1}`,
        items: [
            info('Patch', () => `${partName(p)}${pi().bank ? ', ' + pi().bank : ''}`),
            sub('Choose Patch', () => bankMenu(p)),
            value('Lock Patch', () => (pi().lock ? 'on' : 'off'), () => { sp(`lock:${p}`, 'toggle'); refreshParts(); }, true),
            value('Mute', () => (pi().mute ? 'on' : 'off'), () => { sp(`mute:${p}`, 'toggle'); refreshParts(); }, true),
            value('Solo', () => (pi().solo ? 'on' : 'off'), () => { sp(`solo:${p}`, 'toggle'); refreshParts(); }, true),
            value('Level', () => percent(pi().level), d => { sp(`level:${p}`, (d > 0 ? '+' : '') + d); refreshParts(); }),
            value('Pan', () => panText(pi().pan), d => { sp(`pan:${p}`, (d > 0 ? '+' : '') + d); refreshParts(); }),
            info('Voices', () => { refreshParts(); return String(pi().voices || 0); })
        ]
    };
}

function bankMenu(p) {
    const banks = gj('banks', []);
    const cur0 = parts[p] ? parts[p].bn : -1;
    let sel = 0;
    const items = banks.map((b, r) => {
        if (b[0] === cur0) sel = r;
        return sub(`${b[1]} (${b[2]})`, () => patchList(p, r, b));
    });
    return { title: `Part ${p + 1} Banks`, items, sel };
}

function patchList(p, r, b) {
    const list = gj(`bankp:${r}`, []);
    let sel = 0;
    const items = list.map((e, i) => {
        if (parts[p] && parts[p].bn === b[0] && parts[p].num === e[0] + 1) sel = i;
        return action(e[1], () => {
            sp(`load:${p}`, `${b[0]}.${e[0]}`);
            say(`Loading ${e[1]} into part ${p + 1}, locked`);
        });
    });
    return { title: b[1], items, sel };
}

function cur() {
    return stack[stack.length - 1];
}
function pushMenu(build) {
    const m = build();
    const items = m.items && m.items.length ? m.items : [info('Empty', () => '')];
    stack.push({ title: m.title, items, sel: Math.min(m.sel || 0, items.length - 1), build });
    editing = false;
    view = 'menu';
    say(`${m.title}. ${itemSpeech(items[cur().sel])}`);
    needsRedraw = true;
}
function rebuild(menu) {
    if (!menu || !menu.build) return;
    const m = menu.build();
    if (m.items && m.items.length) {
        menu.items = m.items;
        menu.title = m.title;
        menu.sel = Math.min(menu.sel, m.items.length - 1);
    }
}
function popMenu() {
    editing = false;
    stack.pop();
    if (stack.length === 0) {
        view = 'home';
        say(`Player. ${songName()}, ${stateWord().toLowerCase()}, ${positionSpeech().toLowerCase()}`);
    } else {
        rebuild(cur());
        say(`${cur().title}. ${itemSpeech(cur().items[cur().sel])}`);
    }
    needsRedraw = true;
}

function valueOf(it) {
    if (!it) return '';
    if (it.kind === 'value' || it.kind === 'info') return it.get();
    if (it.kind === 'sub') return '>';
    return '';
}
function itemSpeech(it) {
    if (!it) return '';
    switch (it.kind) {
        case 'value': return `${it.label}: ${it.get()}`;
        case 'info': return `${it.label}: ${it.get()}`;
        case 'sub': return `${it.label}, submenu`;
        case 'action': return `${it.label}, button`;
        default: return it.label;
    }
}

function menuMove(delta) {
    const m = cur();
    const n = Math.max(0, Math.min(m.items.length - 1, m.sel + delta));
    if (n !== m.sel) {
        m.sel = n;
        say(itemSpeech(m.items[n]));
    } else if (delta !== 0) {
        say(delta < 0 ? `Top. ${itemSpeech(m.items[n])}` : `End. ${itemSpeech(m.items[n])}`);
    }
    needsRedraw = true;
}

function menuClick() {
    const it = cur().items[cur().sel];
    if (!it) return;
    if (it.kind === 'sub') {
        pushMenu(it.build);
    } else if (it.kind === 'action') {
        it.run();
    } else if (it.kind === 'value') {
        if (it.toggle) {
            it.step(1);
            say(`${it.label}: ${it.get()}`);
        } else if (!editing) {
            editing = true;
            say(`Editing ${it.label}: ${it.get()}`);
        } else {
            editing = false;
            say(`${it.label}: ${it.get()}`);
        }
    } else if (it.kind === 'info') {
        say(itemSpeech(it));
    }
    needsRedraw = true;
}

function menuInput(cc, val) {
    const m = cur();
    const it = m.items[m.sel];
    if (cc === CC_JOG) {
        const d = decodeDelta(val);
        if (d === 0) return;
        if (editing && it && it.kind === 'value') {
            it.step(shift ? Math.sign(d) : decodeAcceleratedDelta(val, 'hs2mp-edit'));
            say(it.get());
            needsRedraw = true;
        } else {
            menuMove(d);
        }
        return;
    }
    if (val === 0) return;
    if (cc === CC_JOG_CLICK) {
        if (shift && !editing) popMenu();
        else menuClick();
    } else if (cc === CC_MENU) {
        if (editing) {
            editing = false;
            say(`${it.label}: ${it.get()}`);
            needsRedraw = true;
        } else {
            popMenu();
        }
    } else if (cc === CC_PLAY) {
        if (shift) toStart();
        else togglePlay();
        rebuild(m);
    } else if (cc === CC_LOOP) {
        toggleLoop();
    }
}

/* ── file browser (Schwung's shared one, on UserLibrary) ──────────── */

const FS = {
    readdir(path) {
        const out = os.readdir(path) || [];
        if (Array.isArray(out[0])) return out[0];
        if (Array.isArray(out)) return out;
        return [];
    },
    stat(path) {
        return os.stat(path);
    }
};

function browserItemSpeech(item) {
    if (!item) return 'Empty folder';
    if (item.kind === 'up') return 'Up one folder';
    if (item.kind === 'dir') return `Folder ${item.label.replace(/^\[|\]$/g, '')}`;
    return item.label;
}
function folderName(path) {
    if (!path || path === LIBRARY_ROOT) return 'User Library';
    return path.substring(path.lastIndexOf('/') + 1);
}
function openBrowser() {
    refreshCfg();
    const start = cfg.lastdir && cfg.lastdir.indexOf(LIBRARY_ROOT) === 0 ? cfg.lastdir : LIBRARY_ROOT;
    browser = buildFilepathBrowserState({ root: LIBRARY_ROOT, filter: MIDI_EXTENSIONS, name: 'MIDI Files', start_path: start }, '');
    refreshFilepathBrowser(browser, FS);
    view = 'browser';
    const n = browser.items.filter(i => i.kind === 'file').length;
    say(`Open MIDI File. ${folderName(browser.currentDir)}, ${n} file${n === 1 ? '' : 's'}. ${browserItemSpeech(browser.items[browser.selectedIndex])}`);
    needsRedraw = true;
}
function browserInput(cc, val) {
    if (cc === CC_JOG) {
        const d = decodeDelta(val);
        if (d === 0 || !browser.items.length) return;
        const before = browser.selectedIndex;
        moveFilepathBrowserSelection(browser, d);
        const item = browser.items[browser.selectedIndex];
        say(browser.selectedIndex === before ? `${d < 0 ? 'Top' : 'End'}. ${browserItemSpeech(item)}` : browserItemSpeech(item));
        needsRedraw = true;
        return;
    }
    if (val === 0) return;
    if (cc === CC_MENU) {
        view = stack.length ? 'menu' : 'home';
        say(stack.length ? `${cur().title}. ${itemSpeech(cur().items[cur().sel])}` : 'Player');
        needsRedraw = true;
        return;
    }
    if (cc !== CC_JOG_CLICK) return;
    if (shift) {  // up one folder
        const up = browser.items.find(i => i.kind === 'up');
        if (!up) return say('Top folder');
        browser.selectedIndex = browser.items.indexOf(up);
    }
    const r = activateFilepathBrowserItem(browser);
    if (r.action === 'open') {
        refreshFilepathBrowser(browser, FS);
        const n = browser.items.filter(i => i.kind === 'file').length;
        say(`${folderName(browser.currentDir)}, ${n} file${n === 1 ? '' : 's'}. ${browserItemSpeech(browser.items[browser.selectedIndex])}`);
    } else if (r.action === 'select') {
        sp('file_path', r.value);
        announcedSong = '';
        stack = [];
        view = 'home';
        say(`Opening ${r.filename}`);
    } else {
        say('Empty folder');
    }
    needsRedraw = true;
}

/* ── player screen ────────────────────────────────────────────────── */

function homeInput(cc, val) {
    if (cc === CC_JOG) {
        const d = decodeDelta(val);
        if (d !== 0) stepBars(shift ? d * 8 : d);
        return;
    }
    if (val === 0) return;
    switch (cc) {
        case CC_PLAY: if (shift) toStart(); else togglePlay(); break;
        case CC_LOOP: toggleLoop(); break;
        case CC_JOG_CLICK:
            if (shift) {
                refreshAll();
                say(statusSpeech());
            } else {
                pushMenu(rootMenu);
            }
            break;
        case CC_MENU: pushMenu(rootMenu); break;
        default: break;
    }
}

function knobPart(k) {
    return shift ? k + 8 : k;
}
function knobTurn(k, val) {
    if (ready !== 1) return;
    const d = decodeAcceleratedDelta(val, KNOB_CCS[k]);
    if (d === 0) return;
    const p = knobPart(k);
    sp(`level:${p}`, (d > 0 ? '+' : '') + d);
    refreshParts();
    const level = percent(parts[p] && parts[p].level);
    const now = Date.now();
    const label = `Part ${p + 1} ${partName(p)}`;
    say(lastKnob.idx === p && now - lastKnob.at < 1500 ? level : `${label} level ${level}`);
    lastKnob = { idx: p, at: now };
    overlay = { label: `${p + 1} ${partName(p)}`, value: `Level ${level}`, ticks: 60 };
    needsRedraw = true;
}
function knobTouch(k) {
    if (ready !== 1) return;
    refreshParts();
    const p = knobPart(k);
    const level = percent(parts[p] && parts[p].level);
    say(`Part ${p + 1} ${partName(p)} level ${level}`);
    lastKnob = { idx: p, at: Date.now() };
    overlay = { label: `${p + 1} ${partName(p)}`, value: `Level ${level}`, ticks: 60 };
    needsRedraw = true;
}

function padPress(p) {
    if (ready !== 1) return;
    sp(shift ? `solo:${p}` : `mute:${p}`, 'toggle');
    refreshParts();
    const i = parts[p] || {};
    say(`Part ${p + 1}, ${partName(p)}, ${shift ? (i.solo ? 'solo' : 'solo off') : (i.mute ? 'muted' : 'unmuted')}`);
    updatePads(true);
}

/* ── LEDs ─────────────────────────────────────────────────────────── */

/* solo yellow, muted red, playing bright white, in the file grey, otherwise off */
let lastNotes = new Array(PARTS).fill(0);
function updatePads(force) {
    for (let p = 0; p < PARTS; p++) {
        const i = parts[p];
        let c = LED_OFF;
        if (i) {
            const active = (i.voices || 0) > 0 || (i.notes || 0) !== lastNotes[p];
            lastNotes[p] = i.notes || 0;
            if (i.solo) c = LED_SOLO;
            else if (i.mute) c = LED_MUTE;
            else if (active) c = LED_PLAY;
            else if (i.used) c = LED_USED;
        }
        if (force || padLeds[p] !== c) {
            setLED(PAD_FIRST + p, c, !!force);
            padLeds[p] = c;
        }
    }
}
function updateButtons() {
    setButtonLED(CC_PLAY, pos.play ? LED_PLAY_GREEN : LED_DIM);
    setButtonLED(CC_LOOP, cfg.loop ? LED_PLAY : LED_DIM);
}

/* ── drawing ──────────────────────────────────────────────────────── */

function cut(s, n = CHARS) {
    s = String(s || '');
    return s.length > n ? s.substring(0, n - 1) + '~' : s;
}
function wrap(text, width, maxLines) {
    const words = String(text || '').split(/\s+/);
    const lines = [];
    let line = '';
    for (const w of words) {
        if ((line + ' ' + w).trim().length > width) {
            if (line) lines.push(line);
            line = w;
        } else {
            line = (line + ' ' + w).trim();
        }
        if (lines.length >= maxLines) break;
    }
    if (line && lines.length < maxLines) lines.push(line);
    return lines;
}

function drawHome() {
    if (ready !== 1) {
        drawMenuHeader('HS2 MIDI Player');
        wrap(lastStatus || 'Starting...', CHARS, 5).forEach((l, i) => print(2, 16 + i * 9, l, 1));
        return;
    }
    const st = pos.prep > 0 ? `PREP ${pos.prep}` : pos.wait ? 'WAIT' : pos.play ? 'PLAY' : 'STOP';
    drawMenuHeader(cfg.tempo ? 'HS2MP Sync' : 'HS2 MIDI Player', st);
    if (!song.loaded) {
        wrap(song.error ? `Could not open: ${song.error}` : 'No file. Click: menu, Open MIDI File', CHARS, 3)
            .forEach((l, i) => print(2, 17 + i * 9, l, 1));
    } else {
        fill_rect(0, 16, W, 11, 1);
        print(3, 18, cut(songName(), 20), 0);
        const bars = pos.bars || song.bars || 1, len = pos.len !== undefined ? pos.len : song.len;
        const bpm = pos.bpm !== undefined ? pos.bpm : song.bpm;
        print(2, 30, cut(`Bar ${pos.bar || 1}.${pos.beat || 1}/${bars}  ${clock(pos.sec)}/${clock(len)}`), 1);
        print(2, 39, cut(`${Math.round(bpm || 0)}BPM${cfg.loop ? ' Loop' : ''}  CPU${cpu[0]}% V${cpu[2]}`), 1);
        /* the 16 parts: a block each, filled while playing */
        for (let p = 0; p < PARTS; p++) {
            const i = parts[p] || {};
            const x = 2 + p * 8, y = 48;
            if ((i.voices || 0) > 0 && !i.mute) fill_rect(x, y, 6, 4, 1);
            else if (i.used) {
                fill_rect(x, y + 3, 6, 1, 1);
            }
        }
    }
    drawMenuFooter('Play Click:menu');
}

function drawMenu() {
    const m = cur();
    drawMenuHeader(cut(m.title, editing ? 16 : CHARS), editing ? 'EDIT' : '');
    drawMenuList({
        items: m.items,
        selectedIndex: m.sel,
        listArea: { topY: menuLayoutDefaults.listTopY, bottomY: menuLayoutDefaults.listBottomWithFooter },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        getLabel: it => (it ? it.label : ''),
        getValue: (it, i) => {
            const v = valueOf(it);
            return editing && i === m.sel ? `[${v}]` : v;
        },
        announce: false
    });
    drawMenuFooter(editing ? 'Jog:adjust Menu:done' : 'Click:open Menu:up');
}

function drawBrowser() {
    drawMenuHeader(cut(folderName(browser.currentDir), CHARS));
    drawMenuList({
        items: browser.items.length ? browser.items : [{ label: '(no MIDI files)' }],
        selectedIndex: browser.selectedIndex,
        listArea: { topY: menuLayoutDefaults.listTopY, bottomY: menuLayoutDefaults.listBottomWithFooter },
        getLabel: it => (it ? it.label : ''),
        getValue: () => '',
        announce: false
    });
    drawMenuFooter('Click:open Menu:back');
}

function drawOverlay() {
    if (!overlay) return;
    const x = 8, y = 18, w = W - 16, h = 26;
    fill_rect(x, y, w, h, 0);
    fill_rect(x, y, w, 1, 1);
    fill_rect(x, y + h - 1, w, 1, 1);
    fill_rect(x, y, 1, h, 1);
    fill_rect(x + w - 1, y, 1, h, 1);
    print(x + 4, y + 4, cut(overlay.label, 18), 1);
    print(x + 4, y + 15, cut(overlay.value, 18), 1);
}

function draw() {
    clear_screen();
    if (view === 'browser' && browser) drawBrowser();
    else if (view === 'menu' && stack.length) drawMenu();
    else drawHome();
    drawOverlay();
    if (typeof host_flush_display === 'function') host_flush_display();
}

/* ── lifecycle ────────────────────────────────────────────────────── */

function pollReady() {
    const r = parseInt(gp('ready'), 10);
    const status = gp('status');
    const newReady = isNaN(r) ? 0 : r;
    if (status !== lastStatus) {
        lastStatus = status;
        if (newReady !== 1) say(status);
        needsRedraw = true;
    }
    if (newReady !== ready) {
        ready = newReady;
        if (ready === 1) {
            refreshAll();
            say(`HS2 MIDI Player ready. ${song.loaded ? statusSpeech() : 'Click for the menu, then Open MIDI File.'}`);
        }
        needsRedraw = true;
    }
}

/* What the file and the transport did by themselves: opened, prepared, reached the end. */
function speakChanges() {
    if (song.loaded && pos.prep === 0 && announcedSong !== song.path) {
        announcedSong = song.path;
        say(`${songName()}: ${song.parts} parts, ${song.bars} bars, ${spokenTime(song.len)}, ${Math.round(song.bpm)} BPM. ` +
            (cfg.tempo ? "Following Move: press Play to start Move's transport." : 'Press Play.'));
    } else if (!song.loaded && song.error && announcedSong !== 'error:' + song.error) {
        announcedSong = 'error:' + song.error;
        say(`Could not open the file: ${song.error}`);
    }
    if (pos.end && !wasEnd && !pos.play) say('End of file');
    if (pos.play && !wasPlaying && wasPrep > 0 && pos.prep === 0) say('Playing');
    if (cfg.tempo && !!pos.play !== wasPlaying && pos.prep === 0 && !pos.end) say(pos.play ? 'Playing with Move' : 'Stopped with Move');
    wasEnd = !!pos.end;
    wasPlaying = !!pos.play;
    wasPrep = pos.prep || 0;
}

function init() {
    view = 'home';
    stack = [];
    editing = false;
    shift = false;
    overlay = null;
    ready = 0;
    lastStatus = '';
    padLeds = new Array(PARTS).fill(-1);
    pollReady();
    if (ready === 1) {
        refreshAll();
        announcedSong = song.loaded ? song.path : '';
        wasPlaying = !!pos.play;
        say(`HS2 MIDI Player. ${statusSpeech()}`);
    }
    needsRedraw = true;
}

function tick() {
    tickN++;
    drainInject();
    if (ready !== 1) {
        if (tickN % 15 === 0) pollReady();
    } else {
        if (tickN % 6 === 0) {
            refreshSong();  /* before the position: a file that just opened has its position too */
            refreshPos();
            speakChanges();
            if (view === 'home') needsRedraw = true;
        }
        if (tickN % 8 === 0) {
            refreshParts();
            updatePads(false);
            updateButtons();
        }
        if (tickN % 30 === 0) {
            refreshCpu();
            refreshCfg();
            if (gp('ready') !== '1') pollReady();
        }
    }
    if (overlay && --overlay.ticks <= 0) {
        overlay = null;
        needsRedraw = true;
    }
    if (needsRedraw) {
        needsRedraw = false;
        draw();
    }
}

function onMidiMessageInternal(data) {
    if (!data || data.length < 3) return;
    const status = data[0] & 0xF0, d1 = data[1], d2 = data[2];
    if (status === 0x90 && d2 > 0) {
        const t = KNOB_TOUCH_NOTES.indexOf(d1);
        if (t >= 0) {
            knobTouch(t);
            return;
        }
        if (d1 >= PAD_FIRST && d1 < PAD_FIRST + PARTS) {
            padPress(d1 - PAD_FIRST);
            return;
        }
    }
    if (isCapacitiveTouchMessage(data)) return;
    if (status !== 0xB0) return;
    if (d1 === CC_SHIFT) {
        shift = d2 > 0;
        return;
    }
    const k = KNOB_CCS.indexOf(d1);
    if (k >= 0) {
        knobTurn(k, d2);
        return;
    }
    if (view === 'browser' && browser) browserInput(d1, d2);
    else if (view === 'menu' && stack.length) menuInput(d1, d2);
    else homeInput(d1, d2);
}

function onMidiMessageExternal() {}  /* USB MIDI goes to the DSP */

globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiMessageInternal;
globalThis.onMidiMessageExternal = onMidiMessageExternal;

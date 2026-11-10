/*
 * Hypersonic 2 for Schwung: on-device UI.
 *
 * Owns all of its rendering, navigation and speech (the radiogarden / wwvsim pattern for externally installed
 * modules: chain_params in module.json, no ui_hierarchy). Every screen change, menu move and value change is spoken
 * through Schwung's screen reader, with texts reworded for speech ("minus 12 dB", "C sharp 3", "1/8 dotted").
 *
 * Home screen (the active part):  Jog = patch, Shift+Jog = bank, Shift+Click = next part (4 wraps to 1),
 *                                 Click = menu (its first item is Active Part).
 * Schwung does not pass Move's arrow buttons to module screens (seen on hardware), so nothing depends on them; the
 * arrow and -/+ handlers below only help where a Schwung version does pass them.
 * Menus:                          Jog / Up / Down = move, Click = open / edit / run, Left/Right = adjust,
 *                                 Menu (or Shift+Click, or Left on a submenu) = up one level.
 * Knobs 1-6 = the active part's Hyperknobs, knob 7 = part level, knob 8 = part pan (on every screen).
 * Touching a knob speaks its value. The host's Back button always leaves the module.
 *
 * The DSP keys are documented in README.md ("DSP keys"). Values are edited through addresses: n:<addr> name,
 * t:<addr> value text, step:<addr> = delta. One set_param per user action (Schwung's param channel holds one
 * request at a time).
 */

import { isCapacitiveTouchMessage, decodeDelta, decodeAcceleratedDelta } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader, drawMenuList, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce } from '/data/UserData/schwung/shared/screen_reader.mjs';

/* Move controls (shared/constants.mjs values) */
const CC_JOG = 14, CC_JOG_CLICK = 3, CC_SHIFT = 49, CC_MENU = 50;
const CC_DOWN = 54, CC_UP = 55, CC_LEFT = 62, CC_RIGHT = 63;
const KNOB_CCS = [71, 72, 73, 74, 75, 76, 77, 78];
const KNOB_TOUCH_NOTES = [0, 1, 2, 3, 4, 5, 6, 7];

const W = 128;
const CHARS = 21;
const PARTS = 4;

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

/* Display texts reworded for text to speech: signs and sharps are dropped or misread by TTS engines otherwise. */
function speakable(text) {
    return String(text)
        .replace(/-oo\b/g, 'minus infinity')
        .replace(/\+oo\b/g, 'plus infinity')
        .replace(/\b([A-G])#(-?\d)/g, '$1 sharp $2')
        .replace(/\b([A-G])-(\d)/g, '$1 minus $2')
        .replace(/(^|[\s(,:\[])-(?=\.?\d)/g, '$1minus ')
        .replace(/(^|[\s(,:\[])\+(?=\.?\d)/g, '$1plus ')
        .replace(/(\d+\/\d+)T\b/g, '$1 triplet')
        .replace(/(\d+\/\d+)D\b/g, '$1 dotted')
        .replace(/ > /g, ' to ')
        .replace(/%/g, ' percent')
        .replace(/\s+/g, ' ')
        .trim();
}
function say(text) {
    if (text) announce(speakable(text));
}

/* ── state ────────────────────────────────────────────────────────── */

let view = 'home';          // 'home' | 'menu'
let stack = [];             // menus: {title, items, sel, build}
let editing = false;
let shift = false;
let needsRedraw = true;
let tickN = 0;
let ready = 0;              // DSP scan state: 0 scanning, 1 ready, -1 no content
let lastStatus = '';
let active = 0;
let info = { empty: 1, name: 'empty', bank: '', knobs: [], ch: 0, loading: 0 };
let wasLoading = false;
let vcache = {};            // addr -> value text, for one redraw
let overlay = null;         // {label, value, ticks}
let lastKnob = { idx: -1, at: 0 };
let rebuildAt = -1;
let buttonLogLeft = 40;     /* diagnostics: which buttons reach this UI (Schwung's debug log) */

/* ── part info ────────────────────────────────────────────────────── */

function channelText(ch) {
    return ch >= 16 ? 'all channels' : `channel ${ch + 1}`;
}
function refreshInfo() {
    const i = gj(`pinfo:${active}`, null);
    if (!i) return false;
    const changed = JSON.stringify(i) !== JSON.stringify(info);
    info = i;
    return changed;
}
function partSummary() {
    const name = info.empty ? 'empty' : info.name;
    return `Part ${active + 1}, ${channelText(info.ch || 0)}, ${name}`;
}
function knobAddr(k) {
    if (k < 6) return `P${active}.${9 + k}`;
    return k === 6 ? `P${active}.2` : `P${active}.3`;
}
function knobLabel(k) {
    if (k === 6) return 'Level';
    if (k === 7) return 'Pan';
    const n = info.knobs && info.knobs[k] ? info.knobs[k] : '';
    return n ? n : `Hyperknob ${k + 1}`;
}

/* ── menus ────────────────────────────────────────────────────────── */

function sub(label, build) {
    return { kind: 'sub', label, build };
}
function action(label, run) {
    return { kind: 'action', label, run };
}
function note(label) {
    return { kind: 'info', label };
}
function paramItem(row) {
    return { kind: 'param', addr: row[0], label: row[1], ro: !!row[2] };
}

/* A menu from a DSP layout (pages of [addr, name, readOnly]): one page = its parameters, several = a page list. */
function layoutMenu(title, key) {
    return () => {
        const pages = gj(key, []);
        if (pages.length === 0) return { title, items: [note('Nothing to edit')] };
        if (pages.length === 1) return { title: pages[0].t || title, items: pages[0].p.map(paramItem) };
        return {
            title,
            items: pages.map(pg => sub(pg.t, () => ({ title: pg.t, items: pg.p.map(paramItem) })))
        };
    };
}

function bankMenu(p) {
    const banks = gj('banks', []);  // [number, name, first, count]
    if (banks.length === 0) return { title: 'Banks', items: [note('No patches found')] };
    let sel = 0;
    banks.forEach((b, r) => {
        if (info.pos >= b[2] && info.pos < b[2] + b[3]) sel = r;
    });
    return {
        title: 'Banks',
        sel,
        items: banks.map((b, r) => sub(`${b[1]} (${b[3]})`, () => patchList(p, r, b)))
    };
}
function patchList(p, r, b) {
    const names = gj(`bankp:${r}`, []);
    const sel = info.pos >= b[2] && info.pos < b[2] + b[3] ? info.pos - b[2] : 0;
    return {
        title: b[1],
        sel,
        items: names.map((n, i) => action(`${i + 1} ${n}`, () => {
            sp(`load:${p}`, b[2] + i);
            say(`Loading ${n}`);
        }))
    };
}

function elementsMenu(p) {
    const els = gj(`elems:${p}`, []);  // [setter, type, typeName, name]
    if (els.length === 0) return { title: 'Elements', items: [note('No elements: the part is empty')] };
    return {
        title: 'Elements',
        items: els.map(([e, , tname, name]) => sub(`${e} ${tname}: ${name}`, layoutMenu(`${tname}: ${name}`, `elay:${p}.${e}`)))
    };
}

function rootMenu() {
    const p = active;
    return {
        title: `Part ${p + 1}: ${info.empty ? 'empty' : info.name}`,
        items: [
            paramItem(['A', 'Active Part', 0]),
            sub('Browse Patches', () => bankMenu(p)),
            sub('Part Setup', layoutMenu('Part Setup', `lay:setup.${p}`)),
            sub('Hyperknobs', layoutMenu('Hyperknobs', `lay:knobs.${p}`)),
            sub('Elements', () => elementsMenu(p)),
            sub('Arpeggiator', layoutMenu('Arpeggiator', `lay:arp.${p}`)),
            sub('Keyboard', layoutMenu('Keyboard', `lay:keys.${p}`)),
            sub('Voice and Glide', layoutMenu('Voice and Glide', `lay:voice.${p}`)),
            sub('Microtuning', layoutMenu('Microtuning', `lay:tuning.${p}`)),
            sub('Patch Parameters', layoutMenu('Patch Parameters', `play:${p}`)),
            sub('Patch Send Effects', layoutMenu('Patch Send Effects', `lay:sends.${p}`)),
            sub('Global Effects', layoutMenu('Global Effects', 'lay:gfx')),
            sub('Settings', layoutMenu('Settings', 'lay:settings')),
            action('All Notes Off', () => { sp('panic', 1); say('All notes off'); }),
            action('Rescan Content', () => { sp('rescan', 1); say('Rescanning content'); ready = 0; })
        ]
    };
}

function cur() {
    return stack[stack.length - 1];
}
function pushMenu(build) {
    const m = build();
    const items = m.items && m.items.length ? m.items : [note('Empty')];
    stack.push({ title: m.title, items, sel: Math.min(m.sel || 0, items.length - 1), build });
    editing = false;
    vcache = {};
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
    vcache = {};
    if (stack.length === 0) {
        view = 'home';
        refreshInfo();
        say(`Home. ${partSummary()}`);
    } else {
        rebuild(cur());  // titles and names may have changed below (effect types, mod sources)
        say(`${cur().title}. ${itemSpeech(cur().items[cur().sel])}`);
    }
    needsRedraw = true;
}

function valueOf(it) {
    if (!it) return '';
    if (it.kind === 'param') {
        if (!(it.addr in vcache)) vcache[it.addr] = gp(`t:${it.addr}`);
        return vcache[it.addr];
    }
    if (it.kind === 'sub') return '>';
    return '';
}
function itemSpeech(it) {
    if (!it) return '';
    switch (it.kind) {
        case 'param': return `${it.label}: ${valueOf(it)}${it.ro ? ', read only' : ''}`;
        case 'sub': return `${it.label}, submenu`;
        case 'action': return `${it.label}, button`;
        default: return it.label;
    }
}

function adjust(it, delta) {
    if (!it || it.kind !== 'param' || delta === 0) return;
    if (it.ro) {
        say(`${it.label} is read only`);
        return;
    }
    sp(`step:${it.addr}`, delta);
    delete vcache[it.addr];
    if (it.addr === 'A') {  /* the active part: everything in this menu now belongs to it */
        const p = parseInt(gp('part'), 10);
        active = isNaN(p) ? active : p;
        refreshInfo();
        vcache = {};
        rebuild(cur());
        say(partSummary());
        needsRedraw = true;
        return;
    }
    say(valueOf(it));
    if (it.addr[0] === 'F' || it.addr[0] === 'C') rebuildAt = tickN + 40;  // preset loads change knob names
    needsRedraw = true;
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
    } else if (it.kind === 'param') {
        if (it.ro) {
            say(`${it.label}: ${valueOf(it)}, read only`);
        } else if (!editing) {
            editing = true;
            say(`Editing ${it.label}: ${valueOf(it)}`);
        } else {
            editing = false;
            delete vcache[it.addr];
            say(`${it.label}: ${valueOf(it)}`);
        }
    } else {
        say(it.label);
    }
    needsRedraw = true;
}

function menuInput(cc, val) {
    const m = cur();
    const it = m.items[m.sel];
    if (cc === CC_JOG) {
        const d = decodeDelta(val);
        if (d === 0) return;
        if (editing) adjust(it, shift ? Math.sign(d) : decodeAcceleratedDelta(val, 'hs2-edit'));
        else menuMove(d);
        return;
    }
    if (val === 0) return;  // button release
    switch (cc) {
        case CC_UP: if (!editing) menuMove(-1); else adjust(it, 1); break;
        case CC_DOWN: if (!editing) menuMove(1); else adjust(it, -1); break;
        case CC_JOG_CLICK: if (shift && !editing) popMenu(); else menuClick(); break;
        case CC_MENU:
            if (editing) {
                editing = false;
                say(`${it.label}: ${valueOf(it)}`);
                needsRedraw = true;
            } else {
                popMenu();
            }
            break;
        case CC_LEFT:
        case CC_RIGHT: {
            const d = cc === CC_RIGHT ? 1 : -1;
            if (it && it.kind === 'param') adjust(it, shift ? d * 10 : d);
            else if (cc === CC_LEFT) popMenu();
            else if (it && it.kind === 'sub') pushMenu(it.build);
            break;
        }
        default: break;
    }
}

/* ── home screen ──────────────────────────────────────────────────── */

/* Select part n (0-3) and speak it. */
function selectPart(n, edgeWord) {
    if (n === active) {
        say(`${edgeWord || ''}${partSummary()}`);
        return;
    }
    active = n;
    sp('part', active);
    refreshInfo();
    say(partSummary());
    needsRedraw = true;
}

function homeInput(cc, val) {
    if (cc === CC_JOG) {
        const d = decodeDelta(val);
        if (d === 0 || ready !== 1) return;
        const bankBefore = gp(`t:K${active}`);
        if (shift) {
            sp(`step:K${active}`, Math.sign(d));
            say(`${gp(`t:K${active}`)}. ${gp(`t:B${active}`)}`);
        } else {
            sp(`step:B${active}`, d);
            const bank = gp(`t:K${active}`);
            const name = gp(`t:B${active}`);
            say(bank !== bankBefore ? `${bank}. ${name}` : name);
        }
        refreshInfo();
        needsRedraw = true;
        return;
    }
    if (val === 0) return;
    switch (cc) {
        case CC_LEFT:
        case CC_RIGHT: {
            const n = Math.max(0, Math.min(PARTS - 1, active + (cc === CC_RIGHT ? 1 : -1)));
            selectPart(n, n === 0 ? 'First part. ' : 'Last part. ');
            break;
        }
        case CC_UP: {
            refreshInfo();
            let s = partSummary();
            if (!info.empty && info.bank) s += `. Bank ${info.bank}, patch ${info.num}`;
            if (info.loading) s += '. Loading';
            say(s);
            break;
        }
        case CC_JOG_CLICK:
            if (shift && ready === 1) {  /* Shift + Click: next part (wraps 4 -> 1) */
                selectPart((active + 1) % PARTS);
                break;
            }
        /* falls through */
        case CC_DOWN:
        case CC_MENU:
            if (ready !== 1) {
                say(lastStatus || 'Hypersonic 2 is not ready');
                break;
            }
            view = 'menu';
            stack = [];
            refreshInfo();
            pushMenu(rootMenu);
            break;
        default: break;
    }
}

/* ── knobs (every screen) ─────────────────────────────────────────── */

function knobTurn(k, val) {
    if (ready !== 1) return;
    const d = shift ? Math.sign(decodeDelta(val)) : decodeAcceleratedDelta(val, KNOB_CCS[k]);
    if (d === 0) return;
    const addr = knobAddr(k);
    sp(`step:${addr}`, d);
    const value = gp(`t:${addr}`);
    const label = knobLabel(k);
    const now = Date.now();
    /* The name once per gesture; while turning, only the value. */
    say(lastKnob.idx === k && now - lastKnob.at < 1500 ? value : `${label}: ${value}`);
    lastKnob = { idx: k, at: now };
    overlay = { label, value, ticks: 60 };
    vcache = {};
    needsRedraw = true;
}
function knobTouch(k) {
    if (ready !== 1) return;
    const value = gp(`t:${knobAddr(k)}`);
    const label = knobLabel(k);
    say(`${label}: ${value}`);
    lastKnob = { idx: k, at: Date.now() };
    overlay = { label, value, ticks: 60 };
    needsRedraw = true;
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
        drawMenuHeader('Hypersonic 2');
        wrap(lastStatus || 'Starting...', CHARS, 5).forEach((l, i) => print(2, 16 + i * 9, l, 1));
        return;
    }
    const ch = info.ch >= 16 ? 'All' : `Ch${(info.ch || 0) + 1}`;
    const right = info.empty ? '' : `${String(info.bn).padStart(3, '0')}:${info.num}`;
    drawMenuHeader(`Part ${active + 1} ${ch}`, right);
    print(2, 17, cut(info.empty ? '' : info.bank), 1);
    const name = gp(`t:B${active}`) || (info.empty ? 'empty' : info.name);
    fill_rect(0, 27, W, 11, 1);
    print(3, 29, cut(name, 20), 0);
    const knobs = (info.knobs || []).map((n, i) => n ? `${i + 1}${n.substring(0, 5)}` : '').filter(Boolean).join(' ');
    print(2, 41, cut(info.loading ? 'Loading...' : knobs), 1);
    drawMenuFooter('Jog:patch </>:part');
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
        announce: false  /* this UI speaks its own, reworded texts */
    });
    drawMenuFooter(editing ? 'Jog:adjust Menu:done' : 'Click:open Menu:up');
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
    if (view === 'menu' && stack.length) drawMenu();
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
            const p = parseInt(gp('part'), 10);
            active = isNaN(p) ? 0 : Math.max(0, Math.min(PARTS - 1, p));
            refreshInfo();
            say(`Hypersonic 2 ready. ${partSummary()}`);
        }
        needsRedraw = true;
    }
}

function init() {
    view = 'home';
    stack = [];
    editing = false;
    shift = false;
    overlay = null;
    vcache = {};
    ready = 0;
    lastStatus = '';
    pollReady();
    if (ready === 1) {
        refreshInfo();
        say(`Hypersonic 2. ${partSummary()}`);
    }
    needsRedraw = true;
}

function tick() {
    tickN++;
    if (ready !== 1) {
        if (tickN % 15 === 0) pollReady();
    } else if (tickN % 12 === 0) {
        if (refreshInfo()) needsRedraw = true;
        if (wasLoading && !info.loading) {  // a load finished: report a failure (the status says why)
            const st = gp('status');
            if (st.indexOf('could not') >= 0) say(st);
        }
        wasLoading = !!info.loading;
    }
    if (view === 'menu' && tickN % 45 === 0) {  // values the knobs or MIDI changed
        vcache = {};
        needsRedraw = true;
    }
    if (rebuildAt >= 0 && tickN >= rebuildAt) {
        rebuildAt = -1;
        if (view === 'menu') rebuild(cur());
        vcache = {};
        needsRedraw = true;
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

    /* Knob touch (capacitive notes 0-7): speak the value */
    if (status === 0x90 && d2 > 0) {
        const t = KNOB_TOUCH_NOTES.indexOf(d1);
        if (t >= 0) {
            knobTouch(t);
            return;
        }
    }
    if (isCapacitiveTouchMessage(data)) return;
    if (status !== 0xB0) return;

    if (buttonLogLeft > 0 && d2 > 0 && KNOB_CCS.indexOf(d1) < 0 && d1 !== CC_JOG) {
        buttonLogLeft--;
        console.log(`hs2 ui: button cc ${d1} value ${d2}${shift ? ' (shift)' : ''}`);
    }
    if (d1 === CC_SHIFT) {
        shift = d2 > 0;
        return;
    }
    const k = KNOB_CCS.indexOf(d1);
    if (k >= 0) {
        knobTurn(k, d2);
        return;
    }
    if (view === 'menu' && stack.length) menuInput(d1, d2);
    else homeInput(d1, d2);
}

function onMidiMessageExternal() {}

globalThis.init = init;
globalThis.tick = tick;
globalThis.onMidiMessageInternal = onMidiMessageInternal;
globalThis.onMidiMessageExternal = onMidiMessageExternal;

/* The shadow UI's module loader prefers this object (the chain_ui pattern). */
globalThis.chain_ui = { init, tick, onMidiMessageInternal, onMidiMessageExternal };

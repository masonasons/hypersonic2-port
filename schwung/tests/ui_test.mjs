// UI test: runs schwung/ui.js (with Schwung's own shared libraries) against the real DSP through dsp_bridge, presses
// Move's controls and checks what the screen reader is told. Prints the whole spoken transcript.
//
//   node schwung/tests/ui_test.mjs <module.so> <content> <schwung shared dir> <dsp_bridge> <work dir> <ui.js>
import fs from 'node:fs';
import path from 'node:path';
import { execSync, spawn } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const [lib, content, shared, bridge, work, uiPath] = process.argv.slice(2);
fs.mkdirSync(work, { recursive: true });
const fin = path.join(work, 'ui.in'), fout = path.join(work, 'ui.out');
for (const f of [fin, fout]) try { fs.unlinkSync(f); } catch (e) { /* none */ }
execSync(`mkfifo "${fin}" "${fout}"`);
const child = spawn(bridge, [lib, content, fin, fout], { stdio: ['ignore', 'inherit', 'inherit'] });
const wfd = fs.openSync(fin, 'w');
const rfd = fs.openSync(fout, 'r');
let rbuf = '';
function call(line) {
    fs.writeSync(wfd, line + '\n');
    while (!rbuf.includes('\n')) {
        const b = Buffer.alloc(65536);
        const n = fs.readSync(rfd, b, 0, b.length, null);
        if (n > 0) rbuf += b.toString('utf8', 0, n);
    }
    const i = rbuf.indexOf('\n');
    const r = rbuf.slice(0, i);
    rbuf = rbuf.slice(i + 1);
    return r;
}
const sleeper = new Int32Array(new SharedArrayBuffer(4));
const sleep = ms => Atomics.wait(sleeper, 0, 0, ms);

/* Host globals the shadow UI provides */
const spoken = [];
let screen = [];
let setCalls = 0;
globalThis.host_module_get_param = k => { const r = call('g ' + k); return r === '\x01' ? null : r; };
globalThis.host_module_set_param = (k, v) => { setCalls++; call('s ' + k + '\t' + v); return true; };
globalThis.host_send_screenreader = t => { spoken.push(t); if (process.env.HS2_TRACE) console.log("  say: " + t); };
globalThis.shadow_get_display_mode = () => 1;
globalThis.clear_screen = () => { screen = []; };
globalThis.print = (x, y, t) => screen.push({ x, y, t: String(t) });
globalThis.fill_rect = () => {};
globalThis.set_pixel = () => {};
globalThis.draw_rect = () => {};
globalThis.text_width = t => String(t).length * 6;
globalThis.host_flush_display = () => {};
globalThis.move_midi_internal_send = () => {};

const src = fs.readFileSync(uiPath, 'utf8').replaceAll('/data/UserData/schwung/shared/', pathToFileURL(shared).href + '/');
const uiCopy = path.join(work, 'ui_under_test.mjs');
fs.writeFileSync(uiCopy, src);
await import(pathToFileURL(uiCopy).href);
const ui = globalThis.chain_ui;

let fails = 0, passes = 0;
function check(ok, msg) {
    if (ok) passes++;
    else { fails++; console.log('FAIL: ' + msg); }
}
const tick = (n = 1) => { for (let i = 0; i < n; i++) { ui.tick(); sleep(3); } };
const send = (...b) => { ui.onMidiMessageInternal(b); tick(); };
const cc = (n, v) => send(0xB0, n, v);
const press = n => { cc(n, 127); cc(n, 0); };
const jog = d => cc(14, d > 0 ? d : 128 + d);
const knob = (k, d) => cc(71 + k, d > 0 ? d : 128 + d);
const touch = k => { send(0x90, k, 127); send(0x90, k, 0); };
const last = () => spoken[spoken.length - 1] || '';
const MENU = 50, CLICK = 3, LEFT = 62, RIGHT = 63, UP = 55, DOWN = 54, SHIFT = 49;
function waitSpoken(re, ticks = 3000) {
    for (let i = 0; i < ticks; i++) {
        if (spoken.some(s => re.test(s))) return true;
        tick();
    }
    return false;
}
/* Move to the item whose speech matches re in the current menu (from the top). */
function goto(re) {
    jog(-60);
    for (let i = 0; i < 80; i++) {
        if (re.test(last())) return true;
        const before = spoken.length;
        jog(1);
        if (spoken.length === before || /^End\./.test(last())) return re.test(last());
    }
    return false;
}
function screenText() {
    return screen.slice().sort((a, b) => a.y - b.y || a.x - b.x).map(s => s.t).join(' | ');
}

/* ---- start */
ui.init();
check(waitSpoken(/ready/), 'module announces it is ready');
check(/Hypersonic 2 ready\. Part 1, channel 1, /.test(spoken.join(' / ')), 'ready announcement names the part');
tick(300);

/* ---- home: browse */
jog(1);
check(last() === 'Test Sample', `next patch: "${last()}"`);
jog(1);
check(last() === '001 More Tests. Second Bank', `next bank is spoken: "${last()}"`);
jog(-2);
check(last() === '000 Test Patches. Test Analog', `back with its bank: "${last()}"`);
tick(200);
check(/Part 1 Ch1/.test(screenText()) && /Test Analog/.test(screenText()), `home screen: ${screenText()}`);
jog(1);
press(UP);
check(/^Part 1, channel 1, (Test Sample|Test Analog)/.test(last()), `where am I: "${last()}"`);
jog(-1);
tick(200);

/* ---- knobs */
knob(0, 1);
check(/^Thump: 5\d percent$/.test(last()), `Hyperknob 1 by its patch name: "${last()}"`);
knob(0, 1);
check(/^\d+ percent$/.test(last()), `while turning only the value (accelerated): "${last()}"`);
touch(1);
check(last() === 'Bright: 50 percent', `touch speaks: "${last()}"`);
knob(3, 1);
check(/^Hyperknob 4: /.test(last()), `an unnamed Hyperknob: "${last()}"`);
knob(7, 1);
check(/^Pan: right \d+$/.test(last()), `knob 8 = pan: "${last()}"`);
knob(7, -1);
knob(6, -1);
check(/^Level: \d+ percent, minus [\d.]+ dB$/.test(last()), `knob 7 = level, minus spoken: "${last()}"`);

/* ---- parts: Shift + Click on the home screen, and the menu's Active Part */
cc(SHIFT, 127);
press(CLICK);
cc(SHIFT, 0);
check(last() === 'Part 2, channel 2, Test Sample', `shift+click next part: "${last()}"`);
cc(SHIFT, 127); press(CLICK); press(CLICK); press(CLICK); cc(SHIFT, 0);
check(/^Part 1, channel 1, Test Analog/.test(last()), `wraps to part 1: "${last()}"`);
press(CLICK);
check(last() === 'Part 1: Test Analog. Active Part: Part 1', `menu starts with the part: "${last()}"`);
press(CLICK);
jog(1);
check(last() === 'Part 2, channel 2, Test Sample', `active part from the menu: "${last()}"`);
jog(-1);
check(/^Part 1, channel 1, Test Analog/.test(last()), `back to part 1: "${last()}"`);
press(MENU);
press(MENU);
check(/^Home\. Part 1/.test(last()), `home: "${last()}"`);
press(RIGHT);
check(last() === 'Part 2, channel 2, Test Sample', `part 2: "${last()}"`);
press(LEFT);
check(/^Part 1, channel 1, Test Analog/.test(last()), `back to part 1: "${last()}"`);
press(LEFT);
check(/^First part\./.test(last()), `first part edge: "${last()}"`);

/* ---- shift + jog: banks */
cc(SHIFT, 127);
jog(1);
cc(SHIFT, 0);
check(last() === '001 More Tests. Second Bank', `bank step: "${last()}"`);
cc(SHIFT, 127);
jog(-1);
cc(SHIFT, 0);
check(last() === '000 Test Patches. Test Analog', `bank step back: "${last()}"`);
tick(200);

/* ---- menu */
press(CLICK);
check(last() === 'Part 1: Test Analog. Active Part: Part 1', `menu opens: "${last()}"`);
check(goto(/^Part Setup, submenu/), 'Part Setup item');
press(CLICK);
check(/^Part Setup\. Part 1 MIDI Channel: channel 1/.test(last()), `Part Setup flat page: "${last()}"`);
check(goto(/^Level Trim: /), 'Level Trim item');
check(last() === 'Level Trim: minus 6.0 dB', `minus spoken: "${last()}"`);
check(goto(/^Part 1 MIDI Channel/), 'channel item');
press(CLICK);
check(last() === 'Editing Part 1 MIDI Channel: channel 1', `edit starts: "${last()}"`);
jog(1);
check(last() === 'channel 2', `edit value: "${last()}"`);
jog(-1);
press(MENU);
check(last() === 'Part 1 MIDI Channel: channel 1', `edit done: "${last()}"`);
press(MENU);
check(/^Part 1: Test Analog\. Part Setup, submenu/.test(last()), `up a level: "${last()}"`);

check(goto(/^Keyboard, submenu/), 'Keyboard');
press(CLICK);
check(/^Keyboard\. Key Low: C minus 2$/.test(last()), `note names spoken: "${last()}"`);
press(MENU);

/* elements */
check(goto(/^Elements, submenu/), 'Elements');
press(CLICK);
check(last() === 'Elements. 1 Analog: Osc, submenu', `elements list: "${last()}"`);
press(CLICK);
check(last() === 'Analog: Osc. Main, submenu', `element pages: "${last()}"`);
check(goto(/^Filter, submenu/), 'Filter page');
press(CLICK);
check(/^Filter\. /.test(last()), `filter page: "${last()}"`);
check(goto(/^Cutoff: /), `cutoff: "${last()}"`);
const cutoffBefore = last();
press(CLICK);
check(/^Editing Cutoff: /.test(last()), `editing cutoff: "${last()}"`);
jog(-1);
jog(-1);
const cutoffValue = last();
press(MENU);
check(last() !== cutoffBefore && /^Cutoff: /.test(last()), `cutoff changed: ${cutoffBefore} -> ${last()} (${cutoffValue})`);
press(RIGHT);
check(last() !== '', `right arrow quick adjust: "${last()}"`);
press(MENU);
check(goto(/^Mod 1: .* to .*, submenu|^Mod 1: off, submenu/), `mod page title: "${last()}"`);
check(goto(/^Element Type/) === false, 'element type is not a page');
check(goto(/^Main, submenu/), 'Main page');
press(CLICK);
check(goto(/^Element Type: Analog, read only/), `read only item: "${last()}"`);
press(CLICK);
check(/read only/.test(last()), `read only click: "${last()}"`);
press(MENU);
press(MENU);
check(/^Elements\. /.test(last()), `back to elements: "${last()}"`);
jog(1);
check(last() === '2 Effect: Delay, submenu', `insert effect element: "${last()}"`);
press(CLICK);
press(CLICK);
check(/^Effect\. Mute: /.test(last()), `effect page: "${last()}"`);
check(goto(/^Effect Type: Delay, read only/), `effect type: "${last()}"`);
jog(1);
check(!/undefined|null|NaN/.test(last()), `effect knob: "${last()}"`);
cc(SHIFT, 127);
press(CLICK);  /* shift+click = up */
cc(SHIFT, 0);
check(/^Effect: Delay\. /.test(last()) || /^Elements\./.test(last()), `shift+click goes up: "${last()}"`);
press(MENU);
press(MENU);
check(/^Part 1: Test Analog\. /.test(last()), `root again: "${last()}"`);

/* arp */
check(goto(/^Arpeggiator, submenu/), 'Arpeggiator');
press(CLICK);
check(/^Arpeggiator\. Arpeggiator: OFF/.test(last()), `arp page: "${last()}"`);
check(goto(/^Arp Rate: /), 'arp rate');
press(RIGHT);
check(/^1\/16 dotted$|^1\/8 triplet$|^1\/16$|^1\/4 triplet$/.test(last()), `arp rate spoken: "${last()}"`);
press(MENU);

/* patch parameters */
check(goto(/^Patch Parameters, submenu/), 'Patch Parameters');
press(CLICK);
check(goto(/^Arp Step 1, submenu/), 'arp step page');
press(CLICK);
check(/^Arp Step 1\. Note: /.test(last()), `arp step note: "${last()}"`);
press(MENU);
press(MENU);

/* global effects */
check(goto(/^Global Effects, submenu/), 'Global Effects');
press(CLICK);
check(/^Global Effects\. Effect 1: /.test(last()), `global effects: "${last()}"`);
press(CLICK);
check(/^Effect 1: .*\. Effect 1 Preset: /.test(last()), `effect slot page: "${last()}"`);
press(CLICK);
jog(1);
check(last() === 'Delay/Test Delay', `preset chosen: "${last()}"`);
press(MENU);
tick(150);
check(goto(/^Effect 1 /), 'effect items');
press(MENU);
check(/Effect 1: Stereo Delay, submenu/.test(last()), `rebuilt page title after the preset: "${last()}"`);
press(MENU);

/* settings */
check(goto(/^Settings, submenu/), 'Settings');
press(CLICK);
check(/^Settings\. Master Volume: /.test(last()), `settings: "${last()}"`);
check(goto(/^Master Tune: /), 'master tune');
press(LEFT);
check(last() === 'minus 1 semitones', `master tune down: "${last()}"`);
press(RIGHT);
press(MENU);

/* browse patches menu */
check(goto(/^Browse Patches, submenu/), 'Browse Patches');
press(CLICK);
check(last() === 'Banks. 000 Test Patches (2), submenu', `banks: "${last()}"`);
press(CLICK);
check(last() === '000 Test Patches. 1 Test Analog, button', `patch list on the current patch: "${last()}"`);
jog(1);
press(CLICK);
check(last() === 'Loading Test Sample', `load from the list: "${last()}"`);
press(MENU);
press(MENU);
press(MENU);
check(/^Home\. Part 1, channel 1, Test Sample/.test(last()) || /^Home\. Part 1, channel 1, /.test(last()), `home: "${last()}"`);
check(waitSpoken(/zzz/, 200) === false, 'settles');
check(/Test Sample/.test(screenText()), `home shows the new patch: ${screenText()}`);

/* MIDI from Move tracks keeps the UI alive */
call('m 144 60 100');
tick(50);
call('m 128 60 0');
tick(50);

console.log('\n--- spoken transcript ---');
spoken.forEach((s, i) => console.log(String(i + 1).padStart(3) + '  ' + s));
console.log(`\n${passes} passed, ${fails} failed (${setCalls} set_param calls)`);
fs.writeSync(wfd, 'q\n');
sleep(200);
child.kill();
process.exit(fails ? 1 : 0);

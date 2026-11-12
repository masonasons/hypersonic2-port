// UI test for HS2 MIDI Player: runs hs2-midi-player/ui.js (with Schwung's own shared libraries) against the real DSP
// through schwung/tests/dsp_bridge (its fake Move clock runs at 120 BPM), presses Move's controls and checks what the
// screen reader is told. Prints the whole spoken transcript.
//
//   node ui_test.mjs <hs2mp.so> <content> <schwung shared dir> <dsp_bridge> <work dir> <ui.js> <MIDI files dir>
import fs from 'node:fs';
import path from 'node:path';
import { execSync, spawn } from 'node:child_process';
import { pathToFileURL } from 'node:url';

const [lib, content, shared, bridge, work, uiPath, midiDir] = process.argv.slice(2);
fs.rmSync(work, { recursive: true, force: true });
fs.mkdirSync(work, { recursive: true });
/* a user library with one folder of MIDI files (and a file the browser must not list) */
const library = path.join(work, 'UserLibrary');
fs.mkdirSync(path.join(library, 'Songs'), { recursive: true });
for (const f of ['song.mid', 'sixteen.mid']) fs.copyFileSync(path.join(midiDir, f), path.join(library, 'Songs', f));
fs.writeFileSync(path.join(library, 'Songs', 'notes.txt'), 'not MIDI');
const moduleDir = path.join(work, 'module');
fs.mkdirSync(moduleDir);

const fin = path.join(work, 'ui.in'), fout = path.join(work, 'ui.out');
execSync(`mkfifo "${fin}" "${fout}"`);
const child = spawn(bridge, [lib, content, fin, fout], {
    stdio: ['ignore', 'inherit', 'inherit'], env: { ...process.env, HS2_MODULE_DIR: moduleDir }
});
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
const injected = [];
const leds = {};
let screen = [];
let setCalls = 0;
globalThis.host_module_get_param = k => { const r = call('g ' + k); return r === '\x01' ? null : r; };
globalThis.host_module_set_param = (k, v) => { setCalls++; call('s ' + k + '\t' + v); return true; };
globalThis.host_send_screenreader = t => { spoken.push(t); if (process.env.HS2_TRACE) console.log('  say: ' + t); };
globalThis.shadow_get_display_mode = () => 1;
globalThis.clear_screen = () => { screen = []; };
globalThis.print = (x, y, t) => screen.push({ x, y, t: String(t) });
globalThis.fill_rect = () => {};
globalThis.set_pixel = () => {};
globalThis.draw_rect = () => {};
globalThis.text_width = t => String(t).length * 6;
globalThis.host_flush_display = () => {};
globalThis.move_midi_internal_send = m => { if (m && m.length >= 4) leds[`${m[1] & 0xf0}:${m[2]}`] = m[3]; };
globalThis.move_midi_inject_to_move = p => injected.push(p.join(','));

/* QuickJS's os module, from node's fs */
const osStub = path.join(work, 'os_stub.mjs');
fs.writeFileSync(osStub, `import fs from 'node:fs';
export function readdir(p) { try { return [fs.readdirSync(p), 0]; } catch (e) { return [[], -2]; } }
export function stat(p) { try { return [{ mode: fs.statSync(p).mode }, 0]; } catch (e) { return [null, -2]; } }
`);
const src = fs.readFileSync(uiPath, 'utf8')
    .replaceAll('/data/UserData/schwung/shared/', pathToFileURL(shared).href + '/')
    .replace("import * as os from 'os';", `import * as os from '${pathToFileURL(osStub).href}';`)
    .replaceAll("'/data/UserData/UserLibrary'", JSON.stringify(library));
const uiCopy = path.join(work, 'ui_under_test.mjs');
fs.writeFileSync(uiCopy, src);
await import(pathToFileURL(uiCopy).href);
const ui = { init: globalThis.init, tick: globalThis.tick, onMidiMessageInternal: globalThis.onMidiMessageInternal };

let fails = 0, passes = 0;
function check(ok, msg) {
    if (ok) passes++;
    else { fails++; console.log('FAIL: ' + msg); }
}
const tick = (n = 1) => { for (let i = 0; i < n; i++) { ui.tick(); sleep(3); } };
const send = (...b) => { ui.onMidiMessageInternal(b); tick(); };
const cc = (n, v) => send(0xB0, n, v);
const press = n => { cc(n, 127); cc(n, 0); };
const shiftPress = n => { cc(SHIFT, 127); press(n); cc(SHIFT, 0); };
const jog = d => cc(14, d > 0 ? d : 128 + d);
const knob = (k, d) => cc(71 + k, d > 0 ? d : 128 + d);
const touch = k => { send(0x90, k, 127); send(0x90, k, 0); };
const pad = p => { send(0x90, 68 + p, 100); send(0x80, 68 + p, 0); };
const last = () => spoken[spoken.length - 1] || '';
const MENU = 50, CLICK = 3, SHIFT = 49, PLAY = 85, LOOP = 58;
function waitSpoken(re, ticks = 3000) {
    for (let i = 0; i < ticks; i++) {
        if (spoken.some(s => re.test(s))) return true;
        tick();
    }
    return false;
}
function waitNew(re, ticks = 3000) {  // spoken after now
    const from = spoken.length;
    for (let i = 0; i < ticks; i++) {
        if (spoken.slice(from).some(s => re.test(s))) return true;
        tick();
    }
    return false;
}
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
check(waitSpoken(/HS2 MIDI Player ready\. Click for the menu, then Open MIDI File/), `ready: "${last()}"`);

/* ---- Play without a file explains what to do */
press(PLAY);
check(/^No file open\. Click for the menu, then Open MIDI File/.test(last()), `play without a file: "${last()}"`);

/* ---- open a file with the shared browser */
press(CLICK);
check(/^HS2 MIDI Player\. Open MIDI File, button$/.test(last()), `root menu: "${last()}"`);
press(CLICK);
check(/^Open MIDI File\. User Library, 0 files\. Folder Songs$/.test(last()), `browser: "${last()}"`);
press(CLICK);
check(/^Songs, 2 files\. Up one folder$/.test(last()), `into Songs (the text file is not listed): "${last()}"`);
jog(1);
check(last() === 'sixteen', `first file, spoken without .mid: "${last()}"`);
jog(1);
check(last() === 'song', `second file: "${last()}"`);
jog(1);
check(last() === 'End. song', `end of the list: "${last()}"`);
const opened = spoken.length;
press(CLICK);
check(spoken[opened] === 'Opening song', `opening (spoken without .mid): "${spoken[opened]}"`);
check(waitSpoken(/^Test Song: 3 parts, 4 bars, 12 seconds, 120 BPM\. Press Play\.$/), `file ready: "${last()}"`);
tick(10);
check(/Test Song/.test(screenText()) && /Bar 1\.1\/4  0:00\/0:12/.test(screenText()) && /120BPM/.test(screenText()), `player screen: ${screenText()}`);
check(JSON.parse(call('g cfg')).lastdir.endsWith('/Songs'), 'the folder is remembered');

/* ---- transport */
press(PLAY);
check(/^Playing from bar 1 of 4$/.test(last()), `play: "${last()}"`);
tick(250);
shiftPress(CLICK);
check(/^Test Song\. Playing, Bar \d of 4.*, 0:0\d of 0:12, 120 BPM\. CPU \d+ percent, peak \d+, \d+ voices\.$/.test(last()),
    `status: "${last()}"`);
press(PLAY);
check(/^Stopped at bar \d of 4/.test(last()), `stop: "${last()}"`);
shiftPress(PLAY);
check(last() === 'Back to the start', `to the start: "${last()}"`);
jog(1);
check(last() === 'Bar 2 of 4', `jog = one bar: "${last()}"`);
cc(SHIFT, 127); jog(1); cc(SHIFT, 0);
check(last() === 'Bar 4 of 4', `shift+jog = 8 bars (clamped): "${last()}"`);
jog(-1);
check(last() === 'Bar 3 of 4', `jog back: "${last()}"`);
press(LOOP);
check(last() === 'Loop on', `loop: "${last()}"`);
press(LOOP);
check(last() === 'Loop off', `loop off: "${last()}"`);

/* ---- pads: mute and solo */
tick(30);
pad(0);
check(/^Part 1, GM 4, muted$/.test(last()), `pad 1 mutes part 1: "${last()}"`);
pad(0);
check(/^Part 1, GM 4, unmuted$/.test(last()), `again: "${last()}"`);
cc(SHIFT, 127); pad(1); cc(SHIFT, 0);
check(/^Part 2, GM 7, solo$/.test(last()), `shift+pad 2 solos part 2 (GM 7 from bar 2 on): "${last()}"`);
cc(SHIFT, 127); pad(1); cc(SHIFT, 0);
check(/^Part 2, GM 7, solo off$/.test(last()), `solo off: "${last()}"`);
tick(20);
check(leds['144:77'] === 118, `pad of part 10 (drums) lit as used: ${leds['144:77']}`);
check(leds['144:72'] === 0 || leds['144:72'] === undefined, `pad of part 5 (unused) off: ${leds['144:72']}`);

/* ---- knobs: part levels */
touch(0);
check(/^Part 1 GM 4 level \d+ percent$/.test(last()), `knob 1 touch: "${last()}"`);
knob(0, -3);
check(/^\d+ percent$/.test(last()), `knob 1 turn speaks the level only: "${last()}"`);
cc(SHIFT, 127); touch(1); cc(SHIFT, 0);
check(/^Part 10 Standard Kit level \d+ percent$/.test(last()), `shift: knob 2 = part 10: "${last()}"`);

/* ---- parts menu: choose a patch by hand */
press(CLICK);
check(goto(/^Parts, submenu$/), 'Parts in the menu');
press(CLICK);
check(/^Parts\. 1 GM 4, submenu$/.test(last()), `parts list: "${last()}"`);
jog(1);
check(/^2 GM 7, submenu$/.test(last()), `part 2: "${last()}"`);
jog(-1);
press(CLICK);
check(/^Part 1\. Patch: GM 4, 078 GM$/.test(last()), `part menu: "${last()}"`);
jog(1);
press(CLICK);
check(/^Part 1 Banks\. 078 GM \(8\), submenu$/.test(last()), `banks start at the part's bank: "${last()}"`);
check(goto(/^000 Test Patches \(2\), submenu$/), 'bank 000');
press(CLICK);
jog(1);
check(last() === 'Test Pad, button', `patch list: "${last()}"`);
press(CLICK);
check(last() === 'Loading Test Pad into part 1, locked', `load by hand: "${last()}"`);
press(MENU);
press(MENU);
check(waitNew(/Lock Patch: on/, 400) || /Part 1\. /.test(last()), `back in the part menu: "${last()}"`);
check(goto(/^Lock Patch: on$/), `locked: "${last()}"`);
press(CLICK);
check(last() === 'Lock Patch: off', `unlock: "${last()}"`);
check(goto(/^Level: \d+%?/) || /^Level: \d+ percent/.test(last()), `level item: "${last()}"`);
press(CLICK);
check(/^Editing Level: \d+ percent$/.test(last()), `edit level: "${last()}"`);
jog(2);
check(/^\d+ percent$/.test(last()), `level changes: "${last()}"`);
press(MENU);
check(/^Level: \d+ percent$/.test(last()), `done: "${last()}"`);
check(goto(/^Pan: centre$/), `pan: "${last()}"`);
press(MENU);
press(MENU);

/* ---- settings */
check(goto(/^Tempo: from the file$/), `tempo item: "${last()}"`);
const followAt = spoken.length;
press(CLICK);
check(spoken[followAt] === 'Tempo: Follow Move', `follow Move: "${spoken[followAt]}"`);
/* the bridge's Move clock is running: the file plays along at once */
check(waitNew(/^Playing with Move$/, 400) || spoken.slice(followAt).includes('Playing with Move'), 'plays along with Move');
const before = injected.length;
press(PLAY);
tick(40);
check(spoken.slice(-3).some(s => s === "Stopping Move's transport"), `play presses Move's Play: "${spoken.slice(-3).join(' / ')}"`);
check(spoken.filter(s => s === 'Playing with Move').length === 1, `"Playing with Move" said once: ${spoken.filter(s => s === 'Playing with Move').length}`);
check(injected.slice(before).join(' ') === '11,176,85,127 11,176,85,0', `injected ${injected.slice(before).join(' ')}`);
press(CLICK);
check(last() === 'Tempo: from the file', `back to the file's tempo: "${last()}"`);
check(goto(/^Program Changes: General MIDI$/), `GM item: "${last()}"`);
check(goto(/^USB MIDI Input: on$/), `USB item: "${last()}"`);
check(goto(/^Master Volume: \d+ percent$/), `master: "${last()}"`);
check(goto(/^File Info: song, 3 tracks, 3 parts, 4 bars, 12 seconds, starts at 120 BPM$/), `file info: "${last()}"`);
press(CLICK);
check(/^File Info: /.test(last()), 'file info on click');
check(goto(/^Status: CPU \d+ percent, peak \d+ percent, \d+ voices$/), `status item: "${last()}"`);
press(MENU);
check(/^Player\. Test Song, /.test(last()), `back to the player: "${last()}"`);

console.log('\n--- spoken transcript ---');
spoken.forEach((s, i) => console.log(String(i + 1).padStart(3) + '  ' + s));
console.log(`\n${passes} passed, ${fails} failed (${setCalls} set_param calls)`);
fs.writeSync(wfd, 'q\n');
sleep(200);
child.kill();
process.exit(fails ? 1 : 0);

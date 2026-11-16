'use strict';
/**
 * esd.js - the Engine Sound service (runs on the head unit's own Node 8, /usr/bin/node).
 *
 * It owns the synthesizer process (es-synth, beside this file) and a control socket for the app:
 *
 *   /run/lfb-enginesound/ctl.sock   (0660, group tm_oap_app: the OAP apps' group)
 *
 * The app sends newline-terminated lines. Lines the service handles itself:
 *   OUT <alsa device>|none   play into that ALSA device (the synth is started / restarted on it); none stops it
 *   TONE <alsa device>       routing test: 10 s of beeps into that device (the synth is paused meanwhile)
 *   STATUS                   one JSON status line back
 *   SHM                      the SmartX endpoints found in /dev/shm, as JSON
 * Every other line (P / E / T / V / M / S, see synth/README.md) goes to the synth's stdin as it is. The last
 * E / T / V / M lines are replayed when the synth starts again, so a restart keeps the engine, turbo, volume and
 * mute state. Replies are single JSON lines starting with '{'.
 *
 * Runs as nobody with the groups mgu_alsa (SmartX shared memory) and tm_oap_app (the socket), see the unit file.
 */
const fs = require('fs');
const net = require('net');
const path = require('path');
const { spawn } = require('child_process');

const VERSION = '0.1.0';
const RUN_DIR = process.env.ES_RUN_DIR || '/run/lfb-enginesound';
const SOCK = path.join(RUN_DIR, 'ctl.sock');
const SYNTH = process.env.ES_SYNTH || path.join(__dirname, 'es-synth');
const SHM = process.env.ES_SHM || '/dev/shm';
const STICKY = ['E', 'T', 'V', 'M'];

const log = (m) => process.stderr.write(new Date().toISOString().slice(11, 19) + ' esd: ' + m + '\n');

/* ---------------- the synth process ---------------- */
const state = { device: null, tone: null, sticky: {}, recent: [], starts: 0, lastStart: 0, child: null, retryT: null };
const remember = (line) => { state.recent.push(line); if (state.recent.length > 30) state.recent.shift(); };

function startSynth() {
    if (state.child || !state.device || state.tone) return;
    const now = Date.now();
    // a synth that keeps dying: back off up to 10 s
    const wait = now - state.lastStart < 3000 ? Math.min(10000, 1000 * Math.pow(2, Math.min(state.starts, 4))) : 0;
    if (wait) {
        if (!state.retryT) state.retryT = setTimeout(() => { state.retryT = null; startSynth(); }, wait);
        return;
    }
    state.starts = now - state.lastStart < 3000 ? state.starts + 1 : 0;
    state.lastStart = now;
    const args = ['--out', 'alsa:' + state.device];
    log('start ' + path.basename(SYNTH) + ' ' + args.join(' '));
    let child;
    try { child = spawn(SYNTH, args, { stdio: ['pipe', 'ignore', 'pipe'] }); } catch (e) { log('spawn: ' + e); return; }
    state.child = child;
    // (a spawn that fails, ENOENT / EACCES, gives 'error' and no 'exit')
    child.on('error', (e) => {
        log('synth: ' + e.message); remember('synth: ' + e.message);
        if (state.child === child) { state.child = null; startSynth(); }
    });
    child.stdin.on('error', () => {});
    let buf = '';
    child.stderr.on('data', (d) => {
        buf += d.toString();
        let i;
        while ((i = buf.indexOf('\n')) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); if (l) { remember(l); log('synth: ' + l); } }
        if (buf.length > 4096) buf = '';
    });
    child.on('exit', (code, sig) => {
        log('synth exited ' + (sig || code));
        remember('exited ' + (sig || code));
        if (state.child === child) state.child = null;
        startSynth();
    });
    STICKY.forEach((k) => { if (state.sticky[k]) toSynth(state.sticky[k]); });
}

function stopSynth(cb) {
    const child = state.child;
    if (!child) { if (cb) cb(); return; }
    state.child = null;
    let done = false;
    const fin = () => { if (!done) { done = true; if (cb) cb(); } };
    child.removeAllListeners('exit');
    child.on('exit', fin);
    try { child.stdin.write('Q\n'); } catch (e) { /* gone */ }
    setTimeout(() => { try { child.kill('SIGTERM'); } catch (e) {} }, 600);
    setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) {} fin(); }, 1500);
}

function toSynth(line) {
    if (!state.child) return;
    try { state.child.stdin.write(line + '\n'); } catch (e) { /* restarts on exit */ }
}

function setDevice(dev) {
    const d = dev && dev !== 'none' ? dev : null;
    if (d === state.device) return;
    state.device = d;
    stopSynth(() => startSynth());
}

function tone(dev) {
    if (state.tone) return;
    stopSynth(() => {
        log('tone ' + dev);
        let child;
        try { child = spawn(SYNTH, ['--tone', 'alsa:' + dev], { stdio: ['ignore', 'ignore', 'pipe'] }); } catch (e) { log('tone: ' + e); startSynth(); return; }
        state.tone = { dev, child };
        child.on('error', (e) => remember('tone: ' + e.message));
        child.stderr.on('data', (d) => d.toString().split('\n').filter(Boolean).forEach((l) => remember('tone: ' + l)));
        const end = () => { if (state.tone && state.tone.child === child) { state.tone = null; startSynth(); } };
        child.on('exit', end);
        setTimeout(() => { try { child.kill('SIGTERM'); } catch (e) {} }, 12000);
    });
}

/* ---------------- SmartX endpoints ---------------- */
// SmartX keeps a shared-memory file per ALSA endpoint, named after the endpoint (e.g. CarLifeMedia_CF_48_STEREO...).
function shmEndpoints() {
    let names = [];
    try { names = fs.readdirSync(SHM); } catch (e) { return { error: String(e.code || e), endpoints: [] }; }
    const seen = {};
    names.forEach((n) => {
        const m = /^(?:smartx[_.-]?)?([A-Za-z0-9]+(?:_[A-Za-z0-9]+)*?_CF_\d+_[A-Z_]+?)(?:_connection|_ringbuffer|_rb|_ctrl|_shm)?$/.exec(n);
        if (m) seen[m[1]] = true;
    });
    return { endpoints: Object.keys(seen).sort(), files: names.filter((n) => /_CF_|smartx/i.test(n)).slice(0, 120) };
}

function status() {
    return {
        type: 'status', version: VERSION, device: state.device, synth: state.child ? 'running' : state.device ? 'starting' : 'stopped',
        tone: state.tone ? state.tone.dev : null, sticky: state.sticky, log: state.recent.slice(-12),
        synthFile: fs.existsSync(SYNTH),
    };
}

/* ---------------- control socket ---------------- */
function handle(line, reply) {
    line = line.trim();
    if (!line) return;
    const sp = line.indexOf(' '), cmd = sp < 0 ? line : line.slice(0, sp), arg = sp < 0 ? '' : line.slice(sp + 1).trim();
    if (cmd === 'OUT') return setDevice(arg);
    if (cmd === 'TONE') return arg && tone(arg);
    if (cmd === 'STATUS') return reply(status());
    if (cmd === 'SHM') return reply(Object.assign({ type: 'shm' }, shmEndpoints()));
    if (!/^[PETVMS]$/.test(cmd)) return;
    if (STICKY.indexOf(cmd) >= 0) state.sticky[cmd] = line;
    toSynth(line);
}

function listen() {
    try { fs.mkdirSync(RUN_DIR); } catch (e) { /* there (RuntimeDirectory) */ }
    try { fs.unlinkSync(SOCK); } catch (e) { /* none */ }
    const server = net.createServer((c) => {
        let buf = '';
        const reply = (o) => { try { c.write(JSON.stringify(o) + '\n'); } catch (e) {} };
        c.on('data', (d) => {
            buf += d.toString();
            let i;
            while ((i = buf.indexOf('\n')) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); try { handle(l, reply); } catch (e) { log('line: ' + e); } }
            if (buf.length > 65536) buf = '';
        });
        c.on('error', () => {});
        // the app gone: the synth fades out by itself once no P line comes (stale data)
    });
    server.on('error', (e) => { log('socket: ' + e); setTimeout(listen, 3000); });
    server.listen(SOCK, () => {
        try { fs.chmodSync(SOCK, 0o660); } catch (e) { log('chmod: ' + e); }
        log('listening on ' + SOCK + ' (version ' + VERSION + ')');
    });
}

function quit() {
    log('stopping');
    if (state.tone) { try { state.tone.child.kill('SIGTERM'); } catch (e) {} }
    stopSynth(() => process.exit(0));
    setTimeout(() => process.exit(0), 3000);
}
process.on('SIGTERM', quit);
process.on('SIGINT', quit);
process.on('uncaughtException', (e) => log('uncaught: ' + (e && e.stack || e)));

if (require.main === module) listen();
module.exports = { handle, shmEndpoints, status, state };

'use strict';
// node test/service.test.js: the service (tool/lfb-enginesound/esd.js) with a fake synth, driven by the app's own
// client (apps/lfb_enginesound/daemon.js) over a real socket: every control line reaches the synth, and the kept
// ones (preset, turbo, volume, mute, every sound setting) are replayed when the synth or the connection restarts.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'es-svc-'));
const logFile = path.join(dir, 'synth.log'), fake = path.join(dir, 'es-synth-fake');
// the fake synth: its arguments, then every stdin line, into the log; Q ends it
fs.writeFileSync(fake, '#!/usr/bin/env node\n' +
    "const fs = require('fs'), log = (l) => fs.appendFileSync(" + JSON.stringify(logFile) + ", l + '\\n');\n" +
    "log('START ' + process.argv.slice(2).join(' '));\n" +
    "let b = ''; process.stdin.on('data', (d) => { b += d; let i; while ((i = b.indexOf('\\n')) >= 0) { const l = b.slice(0, i); b = b.slice(i + 1); log(l); if (l === 'Q') process.exit(0); } });\n");
fs.chmodSync(fake, 0o755);
const env = Object.assign({}, process.env, { ES_RUN_DIR: dir, ES_SYNTH: fake, ES_SHM: dir });
const esd = spawn(process.execPath, [path.join(__dirname, '../tool/lfb-enginesound/esd.js')], { env, stdio: 'ignore' });
process.env.ES_CTL_SOCK = path.join(dir, 'ctl.sock');
const { Daemon } = require('../apps/lfb_enginesound/daemon');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const lines = () => (fs.existsSync(logFile) ? fs.readFileSync(logFile, 'utf8').split('\n').filter(Boolean) : []);
/** The log's lines after its last START. */
const run = () => { const l = lines(); return l.slice(l.map((x) => x.indexOf('START')).lastIndexOf(0)); };
async function until(what, fn) {
    for (let i = 0; i < 100; i++) { if (fn()) return; await sleep(50); }
    assert.fail(what + '; synth log: ' + JSON.stringify(lines()));
}

(async () => {
    const d = new Daemon({ info() {}, warn() {} });
    // sent before the connection is up: the kept lines arrive once it is (the app's start)
    d.send('ENGINE simulate'); d.send('E v8'); d.send('T 1'); d.send('V 0.90'); d.send('M 0');
    d.send('X bov 0.50'); d.send('X bov 0.75'); d.send('X bass 1.20');
    d.send('OUT devA');
    await until('the synth starts on devA with the kept lines', () => {
        const r = run();
        return r[0] === 'START --out alsa:devA' && ['E v8', 'T 1', 'V 0.90', 'M 0', 'X bov 0.75', 'X bass 1.20'].every((x) => r.indexOf(x) >= 0);
    });
    assert(run().indexOf('X bov 0.50') < 0, 'only the last value of a sound setting is kept');
    // live lines: P, S and a sound setting pass through
    d.send('P 2500.0 0.500 0.575 3 80.0'); d.send('S 3 4'); d.send('X turbo 0.30');
    await until('P, S and X lines reach the synth', () => ['P 2500.0 0.500 0.575 3 80.0', 'S 3 4', 'X turbo 0.30'].every((x) => run().indexOf(x) >= 0));
    // lines that are not the protocol's never reach it
    d.send('Z 1'); d.send('rm -rf'); d.send('X bad;name 1');
    // a new device: the synth restarts and gets the kept lines again, the sound settings included
    d.send('OUT devB');
    await until('the synth restarts on devB with the kept lines', () => {
        const r = run();
        return r[0] === 'START --out alsa:devB' && ['E v8', 'T 1', 'V 0.90', 'M 0', 'X bov 0.75', 'X bass 1.20', 'X turbo 0.30'].every((x) => r.indexOf(x) >= 0);
    });
    assert(run().every((x) => !/^P /.test(x) && !/^S /.test(x)), 'P and S lines are not replayed');
    assert(lines().every((x) => !/^Z|rm|bad/.test(x)), 'unknown lines dropped');
    const st = await new Promise((r) => { d.once('change', () => r(d.status)); d.send('STATUS'); });
    assert.strictEqual(st.synth, 'running');
    assert.strictEqual(st.device, 'devB');
    console.log('service.test.js: ok');
    esd.kill('SIGTERM');
    setTimeout(() => process.exit(0), 300);
})().catch((e) => { console.error(e); esd.kill('SIGKILL'); process.exit(1); });

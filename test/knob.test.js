'use strict';
// node test/knob.test.js: the volume knob over the music (apps/lfb_enginesound/knob.js, core.js hold / trim) with a
// fake car: its main volume is the music's while only the music plays and our source's while the engine is connected.
const assert = require('assert');
const { Core } = require('../apps/lfb_enginesound/core');
const { Knob, POLL_MS, QUIET_MS, PAUSE_MS, CAP, UP_MAX } = require('../apps/lfb_enginesound/knob');
const { VolTest, findDef, inputNames, argsFor, defLines } = require('../apps/lfb_enginesound/voltest');

function make(over) {
    const vals = Object.assign({ enabled: true, engine: 'v8', soundEngine: 'simulate', volume: 50, mix: true, knob: 'engine', rpmMap: 'locked', maxTorque: 400, turbo: true }, over || {});
    const sent = [];
    let t = 1e6;
    const now = () => t;
    // the car: two volumes, the knob moves the one of what plays on top; a connect is CS_CONNECTED 300 ms later
    const car = { music: 30, overlay: 40, conn: false, connAt: 0, readable: true,
        number() { return car.call !== null ? car.call : car.conn ? car.overlay : car.music; },
        turn(steps) { if (car.call !== null) car.call += steps; else if (car.conn) car.overlay += steps; else car.music += steps; },
        // its AudioManager's volume calls: setVolume is the volume of what is on top, setConnectionVolumeStep one
        // connection's (7: the music's); `deaf`: calls that answer E_OK and do nothing; a phone call (its volume on top)
        call: null, hasMusic: true, deaf: {}, stepsOwn: false, floor: false, setsMusic: false, calls: [],
        am(method, a) {
            car.calls.push(method + ' ' + JSON.stringify(a));
            if (car.fail && car.fail[method]) return Promise.reject(new Error('E_NOT_POSSIBLE'));
            if (!car.deaf[method]) {
                if (method === 'setVolume') {
                    if (car.setsMusic) car.music = a.volume; else if (car.call !== null) car.call = a.volume; else if (car.conn) { if (!(car.floor && a.volume < car.music)) car.overlay = a.volume; } else car.music = a.volume;
                }
                if (method === 'setConnectionVolumeStep') { if (car.stepsOwn) car.overlay += a.volumeStep; else if (a.mainConnectionID === 7) car.music += a.volumeStep; }
            }
            return Promise.resolve(method === 'getVolume' ? { volume: car.number(), amError: 'E_OK' } : { amError: 'E_OK' });
        } };
    const route = { state: 'off', connState: null, route: null,
        connect(r) { route.route = r; route.state = 'connected'; route.connState = null; car.connAt = t; car.conn = true; return Promise.resolve(true); },
        disconnect() { route.route = null; route.state = 'off'; route.connState = null; car.conn = false; return Promise.resolve(); } };
    const log = { info() {}, warn() {} };
    const settings = { get: (k) => vals[k], set: (k, v) => { vals[k] = v; core.onSetting(k); } };
    const core = new Core({ settings, log, daemon: { send: (l) => sent.push(l), shm: null }, route, now });
    core._applySettings();       // (what core.start() sends first, without its timer)
    const link = () => (route.state !== 'connected' || !route.route || route.route.kind !== 'mix' ? 'off' : route.connState === 'CS_CONNECTED' ? 'connected' : 'linked');
    const ok = (r) => r.amError === 'E_OK';
    const music = () => (car.hasMusic ? { id: 7, source: 266 } : null);
    // (the list of connections: one reading late, as in the car)
    let asked = 0;
    const clear = (since) => { if (asked <= since) { asked = t; return null; } return car.call === null; };
    let voltest = null;
    const knob = new Knob({ core, settings, log, now, link,
        direct: {
            ready: () => (voltest && voltest.result && voltest.result.ok ? voltest.result.pin : null),
            set: (v) => car.am('setVolume', { sinkID: 1, volume: v }).then(ok),
            step: (d) => (music() ? car.am('setConnectionVolumeStep', { mainConnectionID: 7, volumeStep: d }).then(ok) : null),
            clear,
        } });
    /** ms of the app running: core.tick every 10 ms, the number read and knob.tick every POLL_MS; each(t) before every step. */
    const run = (ms, each) => {
        for (const end = t + ms; t < end;) {
            t += 10;
            if (each) each(t);
            if (route.state === 'connected' && !route.connState && t - car.connAt >= 300) route.connState = 'CS_CONNECTED';
            core.feed({ rpm: 1500, speed: 50, gear: 3, torque: 100 });
            core.tick();
            if (t % POLL_MS === 0) { if (knob.wanted() && car.readable) knob.feed(car.number(), t - 20); knob.tick(); }
        }
    };
    voltest = new VolTest({ core, knob, log, link, sink: 1, now, defs: (over || {}).defs || {}, saved: (over || {}).saved || null,
        am: car.am, music, clear: () => car.call === null, done: (res) => { if (res.ok) vals.knob = 'direct'; knob.resetDirect(); },
        sleep: (ms) => { run(ms); return Promise.resolve(); } });
    return { core, knob, car, route, sent, vals, settings, run, now, voltest };
}
const last = (sent, re) => sent.filter((l) => re.test(l)).pop();

// engine mode (the default): as the car does it. The knob moves the engine while it plays, nothing steps aside,
// and the report says where it was left
{
    const { core, knob, car, route, sent, run } = make();
    run(3000);
    assert.strictEqual(knob.mode, 'watching');
    assert(!knob.report().some((l) => /not readable/.test(l)), 'the first reading is waited for: ' + knob.report());
    run(500, (t) => { if (t % 100 === 0) car.turn(1); });
    run(1500);
    assert.strictEqual(core.held, false);
    assert.strictEqual(route.state, 'connected');
    assert.strictEqual(car.overlay, 45, 'every step reached the engine');
    assert.strictEqual(knob.handovers, 0);
    assert.strictEqual(last(sent, /^V /), 'V 0.90', 'the gain is the volume setting');
    assert(knob.report().some((l) => /engine 40 -> 45$/.test(l)), knob.report().join('\n'));
    assert(knob.report()[0].indexOf('KNOB engine, watching, vol 45, 0.0 dB, 0 hand-overs') === 0, knob.report()[0]);
    // for the speakers: turned over the cap (to the car's top, 55), the synth turns down what is over
    assert.strictEqual(CAP, 48);
    run(1000, (t) => { if (t % 100 === 0) car.turn(1); });
    run(1000);
    assert.strictEqual(car.overlay, 55);
    assert.strictEqual(last(sent, /^V /), 'V ' + (0.9 * Math.pow(10, -7 / 20)).toFixed(2));
    assert.strictEqual(knob.text(), 'Car volume 55, knob: engine, engine -7 dB');
    run(1000, (t) => { if (t % 100 === 0) car.turn(-1); });
    run(1000);
    assert.strictEqual(last(sent, /^V /), 'V 0.90');
    assert.strictEqual(knob.text(), 'Car volume 45, knob: engine');
}
// music mode: a turn of the knob makes the engine step aside, the rest of the turn is the music's, and it comes
// back QUIET_MS after the knob rests
{
    const { core, knob, car, route, sent, settings, run } = make({ knob: 'music' });
    run(3000);
    assert.strictEqual(knob.mode, 'watching');
    assert.strictEqual(route.state, 'connected');
    // one step every 100 ms for a second
    let steps = 0;
    run(1000, (t) => { if (t % 100 === 0) { car.turn(1); steps++; } });
    assert.strictEqual(steps, 10);
    assert.strictEqual(core.held, true, 'stepped aside');
    assert.strictEqual(route.state, 'off', 'the route is released while the knob turns');
    assert.strictEqual(core.reason, 'adjusting the music volume');
    assert(car.overlay - 40 <= 5, 'only the first steps reached the engine: ' + (car.overlay - 40));
    assert(car.music - 30 >= 5, 'the rest was the music: ' + (car.music - 30));
    assert.strictEqual(car.overlay - 40 + car.music - 30, 10);
    assert.strictEqual(last(sent, /^M /), 'M 1');
    run(QUIET_MS - 400);
    assert.strictEqual(core.held, true, 'still aside before the knob rested long enough');
    run(600);
    assert.strictEqual(core.held, false);
    run(2000);
    assert.strictEqual(route.state, 'connected', 'back');
    assert.strictEqual(last(sent, /^M /), 'M 0');
    assert.strictEqual(knob.mode, 'watching');
    assert.strictEqual(knob.handovers, 1);
    assert.strictEqual(last(sent, /^V /), 'V 0.90', 'the gain is the volume setting');
    assert(knob.report().some((l) => /engine aside/.test(l)) && knob.report().some((l) => /engine back/.test(l)));
    assert(knob.report().some((l) => new RegExp('engine on: ' + car.overlay + ' \\(music ' + car.music + '\\)$').test(l)), knob.report().join('\n'));
    // switching to engine mode while aside brings the engine back at once
    car.turn(-2);
    run(600);
    assert.strictEqual(core.held, true);
    settings.set('knob', 'engine');
    run(POLL_MS + 20);
    assert.strictEqual(core.held, false);
    // and the switch to music mode itself is no turn of the knob
    run(3000);
    settings.set('knob', 'music');
    run(1000);
    assert.strictEqual(knob.handovers, 2);
}
// exclusive after mix: nothing is turned down any more
{
    const { knob, car, sent, settings, run } = make();
    car.overlay = 55;
    run(3000);
    assert.strictEqual(knob.trim, -7);
    settings.set('mix', false);
    run(1000);
    assert.strictEqual(knob.trim, 0);
    assert.strictEqual(last(sent, /^V /), 'V 0.90');
}
// the engine stops (or the app is switched off) while aside: nothing stays held
{
    const { core, knob, car, vals, run } = make({ knob: 'music' });
    run(3000);
    car.turn(3);
    run(600);
    assert.strictEqual(core.held, true);
    vals.enabled = false;
    run(600);
    assert.strictEqual(core.held, false);
    assert.strictEqual(knob.mode, 'off');
    assert.strictEqual(core.reason, 'off');
}
// exclusive (no mix): the knob is the car's business
{
    const { core, knob, car, run } = make({ mix: false, knob: 'music' });
    run(3000);
    car.turn(5);
    run(1000);
    assert.strictEqual(knob.mode, 'off');
    assert.strictEqual(core.held, false);
    assert.strictEqual(knob.handovers, 0);
}
// the volume cannot be read: nothing is held, the knob is left to the car
{
    const { core, knob, car, route, run } = make({ knob: 'music' });
    run(3000);
    car.turn(3);
    run(600);
    assert.strictEqual(core.held, true);
    car.readable = false;
    run(4000);
    assert.strictEqual(core.held, false);
    assert.strictEqual(knob.mode, 'blind');
    run(3000);
    assert.strictEqual(route.state, 'connected');
    car.readable = true;
    run(3000);
    assert.strictEqual(knob.mode, 'watching');
}
// a number that moves by itself right after every return (not a hand on the knob): after three in a row the
// hand-over rests, the engine keeps playing
{
    const { core, knob, car, route, run } = make({ knob: 'music' });
    let seen = 'settling';
    run(40000, () => {
        if (knob.mode === 'watching' && seen !== 'watching') car.overlay += 4;      // (jumps as soon as it is watched)
        seen = knob.mode;
    });
    assert.strictEqual(knob.handovers, 2, 'two hand-overs, the third is recognised');
    assert.strictEqual(core.held, false);
    assert.strictEqual(route.state, 'connected');
    assert(knob.report()[0].indexOf('resting') > 0, knob.report()[0]);
    // a real turn after the rest is a turn again
    run(PAUSE_MS);
    car.turn(2);
    run(600);
    assert.strictEqual(core.held, true);
}
// the test tone owns the route: the knob stays out of it
{
    const { core, knob, car, run } = make({ knob: 'music' });
    run(3000);
    core.test();
    run(1000);
    car.turn(4);
    run(1000);
    assert.strictEqual(core.held, false);
    assert.strictEqual(knob.mode, 'off');
    core.endTest();
}

/* ---------- the volume test and "direct": the knob stays the music's without a pause ---------- */

// a method's arguments from the car's definition, whatever its shape
{
    const cat = { methods: { setVolume: { id: 9, inputs: [{ name: 'sinkID', type: 'u16' }, { name: 'volume', type: 'i16' }], outputs: [{ name: 'amError' }] } },
        list: [{ name: 'setConnectionVolumeStep', in: { connectionID: 'u16', volStep: 'i16' } }] };
    assert.deepStrictEqual(inputNames(findDef(cat, 'setVolume')), ['sinkID', 'volume']);
    assert.deepStrictEqual(inputNames(findDef(cat, 'setConnectionVolumeStep')), ['connectionID', 'volStep']);
    assert.strictEqual(inputNames(findDef(cat, 'getVolume')), null);
    assert.strictEqual(inputNames({ id: 4 }), null);
    assert.deepStrictEqual(argsFor(['connectionID', 'volStep'], { conn: 7, value: -2, sink: 1 }), { args: { connectionID: 7, volStep: -2 } });
    assert.deepStrictEqual(argsFor(['sinkID', 'volume'], { sink: 1, value: 30 }), { args: { sinkID: 1, volume: 30 } });
    assert.deepStrictEqual(argsFor(['sinkID', 'ramp'], { sink: 1, value: 30 }), { unknown: 'ramp' });
    assert(defLines({ setVolume: cat.methods.setVolume, getVolume: null }, 64).some((l) => /getVolume no definition found/.test(l)));
}

(async () => {
    // the test passes in a car whose calls do what their names say: only down and back, and then direct is on
    {
        const { core, knob, car, vals, run, voltest } = make();
        run(3000);
        const res = await voltest.run();
        assert.strictEqual(res.ok, true, res.lines.join('\n'));
        assert.strictEqual(res.pin, 40);
        assert.strictEqual(car.music, 30, 'the music is where it was');
        assert.strictEqual(car.overlay, 40, 'the engine too');
        assert(car.calls.every((c) => !/"volume":(4[1-9]|[5-9]\d)/.test(c) && !/"volumeStep":[1-9]/.test(c)), 'nothing was ever turned up: ' + car.calls.join(' | '));
        assert(/passed/.test(res.lines[0]) && res.lines.some((l) => /music 30, engine 40/.test(l)) && res.lines.some((l) => /music after: 28, put back: 30 ok/.test(l)), res.lines.join('\n'));
        assert.strictEqual(vals.knob, 'direct');
        assert.strictEqual(core.held, false);
        assert.strictEqual(knob.testing, false);
        run(3000);
        assert.strictEqual(knob.mode, 'watching');
        // a turn of the knob: every step reaches the music, the engine is back where it was, no pause
        car.calls.length = 0;
        let muted = false;
        run(1000, (t) => { if (t % 100 === 0) car.turn(1); if (core.held) muted = true; });
        run(1500);
        assert.strictEqual(car.overlay, 40, 'the engine stays');
        assert.strictEqual(car.music, 40, 'all ten steps are the music\'s');
        assert.strictEqual(muted, false, 'and nothing stepped aside');
        assert.strictEqual(knob.trim, 0);
        assert(knob.report().some((l) => /music \+10, engine stays 40/.test(l)), knob.report().join('\n'));
        assert(knob.report()[0].indexOf('KNOB direct, watching, vol 40, 0.0 dB, pin 40, music +10') === 0, knob.report()[0]);
        assert.strictEqual(knob.text(), 'Car volume 40, knob: music, direct');
        run(1000, (t) => { if (t % 100 === 0) car.turn(-1); });
        run(1500);
        assert.strictEqual(car.overlay, 40);
        assert.strictEqual(car.music, 30);
        // while the number is over the pin, the synth turns the engine down
        car.turn(3);
        let low = 0;
        run(POLL_MS * 2, () => { low = Math.min(low, knob.trim); });
        assert.strictEqual(low, -3);
        run(1000);
        assert.strictEqual(knob.trim, 0);
        // the car connects our source at the music's volume when that is higher: put back, and that is no turn
        car.music = 46;
        vals.enabled = false; run(1500); vals.enabled = true;
        car.overlay = 46;
        run(4000);
        assert.strictEqual(knob.mode, 'watching');
        assert.strictEqual(car.overlay, 40, 'back at the pin');
        assert.strictEqual(car.music, 46, 'nothing passed on to the music');
        // a phone call takes the top: its volume is not ours to touch, and nothing reaches the music
        car.calls.length = 0;
        car.call = 25;
        run(600);
        car.turn(4);
        run(2000);
        assert.strictEqual(car.call, 29, 'the call\'s volume is left alone');
        assert.strictEqual(car.music, 46);
        assert.strictEqual(car.calls.length, 0, car.calls.join(' | '));
        car.call = null;
        run(2000);
        assert.strictEqual(car.overlay, 40);
        assert.strictEqual(car.music, 46, 'the return from the call is no turn either');
        car.turn(2);
        run(1500);
        assert.strictEqual(car.music, 48);
        // a spinning number: no more than UP_MAX steps up within two seconds
        car.music = 20;
        run(1900, (t) => { if (t % 50 === 0) car.turn(2); });
        assert(car.music - 20 <= UP_MAX + 10, 'limited: ' + (car.music - 20));
    }
    // without music the knob has nothing to move: the engine still stays
    {
        const { knob, car, run, voltest } = make();
        run(3000);
        await voltest.run();
        run(3000);
        car.hasMusic = false;
        car.turn(5);
        run(1500);
        assert.strictEqual(car.overlay, 40);
        assert.strictEqual(car.music, 30);
        assert.strictEqual(knob.directOff, null);
    }
    // the car keeps our source at least at the music's volume: taken as it is, the synth makes up for it
    {
        const { knob, car, vals, run, voltest } = make();
        run(3000);
        await voltest.run();
        car.floor = true; car.music = 44;
        vals.enabled = false; run(1500); vals.enabled = true;
        car.overlay = 44;
        run(4000);
        assert.strictEqual(car.overlay, 44);
        assert.strictEqual(knob.pin, 44);
        assert.strictEqual(knob.trim, -4, 'the engine is as loud as at 40');
        assert.strictEqual(knob.directOff, null);
        assert(knob.report().some((l) => /the car keeps the engine at 44 \(asked 40\)/.test(l)));
    }
    // cars where the calls do not do it: the test says which, direct stays off, nothing is left changed
    {
        const { knob, car, vals, run, voltest } = make();
        car.deaf.setConnectionVolumeStep = true;
        run(3000);
        const res = await voltest.run();
        assert.strictEqual(res.ok, false);
        assert(res.lines.some((l) => /the steps did not reach it/.test(l)), res.lines.join('\n'));
        assert.strictEqual(vals.knob, 'engine');
        assert.strictEqual(car.music, 30); assert.strictEqual(car.overlay, 40);
        run(3000);
        car.turn(3); run(1000);
        assert.strictEqual(car.overlay, 43, 'the knob is the engine\'s as before');
        assert.strictEqual(knob.text(), 'Car volume 43, knob: engine');
    }
    {
        const { car, run, voltest } = make();
        car.stepsOwn = true;
        run(3000);
        const res = await voltest.run();
        assert.strictEqual(res.ok, false);
        assert(res.lines.some((l) => /it moved the engine, not the music/.test(l)), res.lines.join('\n'));
        assert.strictEqual(car.music, 30, res.lines.join('\n') + car.calls.join(' | ')); assert.strictEqual(car.overlay, 40);
    }
    {
        const { car, run, voltest } = make();
        car.fail = { setVolume: true };
        run(3000);
        const res = await voltest.run();
        assert.strictEqual(res.ok, false);
        assert(res.lines.some((l) => /setVolume .*E_NOT_POSSIBLE/.test(l)), res.lines.join('\n'));
        assert.strictEqual(car.music, 30); assert.strictEqual(car.overlay, 40);
    }
    // a car whose setVolume is always the music's: found out, the music put back, and no call ever asks for more
    // than there was
    {
        const { car, vals, run, voltest } = make();
        car.setsMusic = true;
        run(3000);
        const res = await voltest.run();
        assert.strictEqual(res.ok, false);
        assert(res.lines.some((l) => /it set the music instead: 30 -> 38, put back: 30/.test(l)), res.lines.join('\n'));
        assert.strictEqual(car.music, 30); assert.strictEqual(car.overlay, 40);
        assert.deepStrictEqual(car.calls, ['setVolume {"sinkID":1,"volume":38}', 'setVolume {"sinkID":1,"volume":30}']);
        assert.strictEqual(vals.knob, 'engine');
    }
    // not while nothing plays over the music, and not during a call
    {
        const { car, run, voltest } = make({ mix: false });
        run(3000);
        assert(/must play over the music/.test((await voltest.run()).lines[1]));
        const b = make();
        b.run(3000);
        b.car.call = 20;
        assert(/something else plays/.test((await b.voltest.run()).lines[1]));
        assert.strictEqual(car.calls.length + b.car.calls.length, 0);
    }
    // direct chosen without a passed test: as "engine", and the page says why
    {
        const { knob, car, run } = make({ knob: 'direct' });
        run(3000);
        car.turn(3); run(1000);
        assert.strictEqual(car.overlay, 43);
        assert.strictEqual(knob.text(), 'Car volume 43, knob: engine (volume test first)');
    }
    // the car stops taking the volume back: direct goes off, the music does not run away
    {
        const { knob, car, run, voltest } = make();
        run(3000);
        await voltest.run();
        run(3000);
        car.deaf.setVolume = true;
        car.turn(2);
        run(3000);
        assert(knob.directOff, 'off');
        assert.strictEqual(car.music, 32, 'the two steps went to the music once');
        car.turn(5);
        run(2000);
        assert.strictEqual(car.music, 32);
        assert(/direct failed/.test(knob.text()), knob.text());
    }
    console.log('knob tests ok');
})().catch((e) => { console.error(e); process.exit(1); });

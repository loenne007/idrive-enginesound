'use strict';
// node test/knob.test.js: the volume knob over the music (apps/lfb_enginesound/knob.js, core.js hold / trim) with a
// fake car: its main volume is the music's while only the music plays and our source's while the engine is connected.
const assert = require('assert');
const { Core } = require('../apps/lfb_enginesound/core');
const { Knob, POLL_MS, QUIET_MS, PAUSE_MS, CAP } = require('../apps/lfb_enginesound/knob');

function make(over) {
    const vals = Object.assign({ enabled: true, engine: 'v8', soundEngine: 'simulate', volume: 50, mix: true, knob: 'engine', rpmMap: 'locked', maxTorque: 400, turbo: true }, over || {});
    const sent = [];
    let t = 1e6;
    const now = () => t;
    // the car: two volumes, the knob moves the one of what plays on top; a connect is CS_CONNECTED 300 ms later
    const car = { music: 30, overlay: 40, conn: false, connAt: 0, readable: true,
        number() { return car.conn ? car.overlay : car.music; },
        turn(steps) { if (car.conn) car.overlay += steps; else car.music += steps; } };
    const route = { state: 'off', connState: null, route: null,
        connect(r) { route.route = r; route.state = 'connected'; route.connState = null; car.connAt = t; car.conn = true; return Promise.resolve(true); },
        disconnect() { route.route = null; route.state = 'off'; route.connState = null; car.conn = false; return Promise.resolve(); } };
    const log = { info() {}, warn() {} };
    const settings = { get: (k) => vals[k], set: (k, v) => { vals[k] = v; core.onSetting(k); } };
    const core = new Core({ settings, log, daemon: { send: (l) => sent.push(l), shm: null }, route, now });
    core._applySettings();       // (what core.start() sends first, without its timer)
    const knob = new Knob({ core, settings, log, now,
        link: () => (route.state !== 'connected' || !route.route || route.route.kind !== 'mix' ? 'off' : route.connState === 'CS_CONNECTED' ? 'connected' : 'linked') });
    /** ms of the app running: core.tick every 10 ms, the number read and knob.tick every POLL_MS; each(t) before every step. */
    const run = (ms, each) => {
        for (const end = t + ms; t < end;) {
            t += 10;
            if (each) each(t);
            if (route.state === 'connected' && !route.connState && t - car.connAt >= 300) route.connState = 'CS_CONNECTED';
            core.feed({ rpm: 1500, speed: 50, gear: 3, torque: 100 });
            core.tick();
            if (t % POLL_MS === 0) { if (knob.wanted() && car.readable) knob.feed(car.number()); knob.tick(); }
        }
    };
    return { core, knob, car, route, sent, vals, settings, run, now };
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
console.log('knob tests ok');

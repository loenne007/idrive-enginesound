'use strict';
// node test/core.test.js: the app's mapping and on / off logic (apps/lfb_enginesound/core.js) with a fake service.
const assert = require('assert');
const { Core, PRESETS } = require('../apps/lfb_enginesound/core');
const { DEVICE } = require('../apps/lfb_enginesound/route');

function make(over) {
    const vals = Object.assign({ enabled: true, engine: 'v8', volume: 60, mode: 'mix', sportOnly: false, rpmMap: 'scaled', realRedline: 4800, maxTorque: 400, turbo: true }, over || {});
    const sent = [], routed = [];
    let t = 1000;
    const core = new Core({
        settings: { get: (k) => vals[k] },
        log: { info() {}, warn() {} },
        daemon: { send: (l) => sent.push(l) },
        route: { connect: () => { routed.push(['connect']); return Promise.resolve(true); }, disconnect: () => { routed.push(['disconnect']); return Promise.resolve(); }, error: null },
        now: () => t,
    });
    return { core, sent, routed, vals, step: (ms) => { t += ms; } };
}

// rpm mapping: idle -> idle, max -> redline, linear between
{
    const { core } = make();
    const p = PRESETS.v8;
    assert.strictEqual(Math.round(core.virtualRpm(800)), p.idle);
    assert.strictEqual(Math.round(core.virtualRpm(4800)), p.red);
    const mid = core.virtualRpm(2800);
    assert(Math.abs(mid - (p.idle + (p.red - p.idle) / 2)) < 1, 'midpoint ' + mid);
    assert(core.virtualRpm(400) < p.idle && core.virtualRpm(400) > 0);
    assert(core.virtualRpm(9000) <= p.red * 1.1 + 1, 'clamped above');
}
{
    const { core } = make({ rpmMap: 'real' });
    assert.strictEqual(core.virtualRpm(2345), 2345);
}
// load: torque against the max, overrun negative
{
    const { core } = make();
    assert.strictEqual(core.load(200), 0.5);
    assert.strictEqual(core.load(800), 1);
    assert.strictEqual(core.load(-40), -0.5);
    assert.strictEqual(core.load(-400), -1);
    assert.strictEqual(core.load(null), 0.2);
}
// plays only with the engine running, sends P lines, routes once; mutes and releases the route when the engine stops
{
    const { core, sent, routed, step } = make();
    core.start();
    assert(sent.indexOf('M 1') >= 0 && sent.indexOf('E v8') >= 0 && sent.indexOf('V 0.60') >= 0);
    core.tick();
    assert.strictEqual(core.reason, 'no engine data');
    core.feed({ rpm: 1500, speed: 30, gear: 2, torque: 120 });
    core.tick();
    assert.strictEqual(core.reason, null);
    assert(sent.indexOf('M 0') >= 0, 'unmuted');
    assert(sent.indexOf('OUT ' + DEVICE) >= 0, 'device');
    assert.deepStrictEqual(routed[0], ['connect']);
    const p = sent.filter((l) => l[0] === 'P').pop().split(' ');
    assert.strictEqual(p[4], '2');
    assert(Math.abs(Number(p[2]) - 0.3) < 1e-6, 'load');
    // a shift
    core.feed({ gear: 3 });
    assert(sent.indexOf('S 2 3') >= 0, 'shift event');
    // engine off: muted, route released after RELEASE_MS
    core.feed({ rpm: 0 });
    core.tick();
    assert.strictEqual(core.reason, 'engine off');
    assert.strictEqual(sent[sent.length - 1], 'M 1');
    step(5000); core.tick();
    assert.deepStrictEqual(routed[routed.length - 1], ['disconnect']);
    assert(sent.indexOf('OUT none') >= 0);
}
// stale data stops it
{
    const { core, step } = make();
    core.feed({ rpm: 2000, speed: 10 });
    core.tick(); assert.strictEqual(core.reason, null);
    step(2000); core.tick(); assert.strictEqual(core.reason, 'no engine data');
}
// sport only
{
    const { core, routed } = make({ sportOnly: true });
    core.feed({ rpm: 2000, speed: 10, mode: 'other' });
    core.tick(); assert.strictEqual(core.reason, 'waiting for Sport mode');
    assert.strictEqual(routed.length, 0);
    core.feed({ mode: 'sport' });
    core.tick(); assert.strictEqual(core.reason, null);
    assert.deepStrictEqual(routed[0], ['connect']);
}
// the test tone: route + TONE, then released with the engine off
{
    const { core, sent, routed } = make();
    core.test();
    assert.deepStrictEqual(routed[0], ['connect']);
    assert(sent.indexOf('OUT ' + DEVICE) >= 0);
    // TONE goes once the route is connected (a promise later)
    setImmediate(() => { assert(sent.indexOf('TONE ' + DEVICE) >= 0, 'tone after connect'); console.log('core.test.js: ok'); process.exit(0); });
}
// idle learning moves towards the car's idle
{
    const { core } = make();
    for (let i = 0; i < 500; i++) core.feed({ rpm: 760, speed: 0, torque: 20 });
    assert(Math.abs(core.idle - 760) < 5, 'idle ' + core.idle);
}

'use strict';
// node test/core.test.js: the app's mapping, on / off logic, sound engine ids and audio routes (apps/lfb_enginesound/
// core.js, route.js) with a fake service and a fake AudioManager.
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Core, PRESETS, SOUND_ENGINES, soundEngineId } = require('../apps/lfb_enginesound/core');
const { Route, EXCLUSIVE, MIX_DEFAULT, deviceFor, mixCandidates, mixRoute, routeFor } = require('../apps/lfb_enginesound/route');
const DEVICE = EXCLUSIVE.device;

function make(over) {
    const vals = Object.assign({ enabled: true, engine: 'v8', soundEngine: 'simulate', volume: 50, sportOnly: false, rpmMap: 'scaled', realRedline: 4800, maxTorque: 400, turbo: true }, over || {});
    const sent = [], routed = [];
    let t = 1000;
    const daemon = { send: (l) => sent.push(l), shm: null };
    const core = new Core({
        settings: { get: (k) => vals[k], set: (k, v) => { vals[k] = v; core.onSetting(k); } },
        log: { info() {}, warn() {} },
        daemon,
        route: { connect: (r) => { routed.push(['connect', r.source]); return Promise.resolve(true); }, disconnect: () => { routed.push(['disconnect']); return Promise.resolve(); }, error: null },
        now: () => t,
    });
    return { core, sent, routed, vals, daemon, step: (ms) => { t += ms; } };
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
// the Enhance engine (Active Sound Design) gets the car's real rpm and shows its own preset names
{
    const { core } = make({ soundEngine: 'enhance', engine: 'v10' });
    assert.strictEqual(core.virtualRpm(2345), 2345);
    assert.strictEqual(core.presetInfo('v10').title, 'V6 TDI');
    const w = make({ soundEngine: 'waveguide', engine: 'v10' }).core;
    assert.strictEqual(w.presetInfo('v10').title, 'V10');
    assert(w.virtualRpm(2345) > 2345);
}
// the main page's five sounds, in the owner's order: three of Simulate, two of Enhance; choosing one sets the sound
// engine and its preset; a legacy combination is none of them
{
    const { core, vals, sent } = make({ engine: 'i6' });
    assert.deepStrictEqual(core.sounds().map((x) => x.title + ' / ' + x.kind), ['Inline 6 / Simulate', 'V8 / Simulate', 'V12 / Simulate', 'Inline 6 Diesel / Enhance', 'V8 TDI / Enhance']);
    assert.deepStrictEqual(core.sounds().filter((x) => x.selected).map((x) => x.id), ['i6']);
    core.setSound('v8d');
    assert.deepStrictEqual([vals.engine, vals.soundEngine], ['v8', 'enhance']);
    assert(sent.indexOf('E v8') >= 0 && sent.indexOf('E v8') < sent.indexOf('ENGINE enhance'), 'the preset first, then the sound engine: ' + sent);
    assert.deepStrictEqual(core.sounds().filter((x) => x.selected).map((x) => x.id), ['v8d']);
    assert.strictEqual(core.virtualRpm(2345), 2345);
    core.setSound('v12');
    assert.deepStrictEqual([vals.engine, vals.soundEngine], ['v12', 'simulate']);
    core.setSound('nope');
    assert.deepStrictEqual([vals.engine, vals.soundEngine], ['v12', 'simulate']);
    assert.strictEqual(make({ engine: 'v10' }).core.sounds().filter((x) => x.selected).length, 0);
    assert.strictEqual(make({ soundEngine: 'waveguide', engine: 'i6' }).core.sounds().filter((x) => x.selected).length, 0);
    assert.strictEqual(make().core.legacy(), false);
    assert.strictEqual(make({ legacyEngines: true }).core.legacy(), true);
}
// sound engines: Simulate (default) and Enhance, the legacy Waveguide and Replica behind the legacyEngines setting;
// ids saved by earlier versions map to the current ones (app.json "aliases" agrees)
{
    assert.deepStrictEqual(SOUND_ENGINES.map((x) => x.id), ['simulate', 'enhance', 'waveguide', 'replica']);
    assert(SOUND_ENGINES.every((x) => /^[A-Z][a-z]+$/.test(x.title)), 'one-word titles');
    const migrate = { a: 'waveguide', b: 'replica', c: 'simulate', d: 'enhance', pulse: 'simulate', bass: 'enhance',
        simulate: 'simulate', enhance: 'enhance', waveguide: 'waveguide', replica: 'replica',
        x: 'simulate', undefined: 'simulate', constructor: 'simulate' };
    Object.keys(migrate).forEach((k) => assert.strictEqual(soundEngineId(k === 'undefined' ? undefined : k), migrate[k], k));
    const spec = require('../apps/lfb_enginesound/app.json').settings.find((s) => s.key === 'soundEngine');
    assert.strictEqual(spec.default, 'simulate');
    assert.deepStrictEqual(spec.options.map((o) => o[0]), ['simulate', 'enhance', 'waveguide', 'replica']);
    assert.deepStrictEqual(spec.aliases, { a: 'waveguide', b: 'replica', c: 'simulate', d: 'enhance', pulse: 'simulate', bass: 'enhance' });
    const old = make({ soundEngine: 'c' });
    old.core.start();
    assert(old.sent.indexOf('ENGINE simulate') >= 0, 'old c plays simulate');
    assert.strictEqual(make({ soundEngine: 'd' }).core.virtualRpm(2345), 2345, 'old d is enhance: real rpm');
    // the legacy ones are offered only with the setting (or while one is chosen)
    assert.deepStrictEqual(make().core.soundEngines().map((x) => x.id), ['simulate', 'enhance']);
    assert.deepStrictEqual(make({ legacyEngines: true }).core.soundEngines().map((x) => x.id), ['simulate', 'enhance', 'waveguide', 'replica']);
    assert.deepStrictEqual(make({ soundEngine: 'replica' }).core.soundEngines().map((x) => x.id), ['simulate', 'enhance', 'replica']);
}
// volume: 10-100 % -> V 0.18-1.4, 50 % = 0.9 (about the 90 % of earlier versions)
{
    assert.strictEqual(make({ volume: 50 }).core.synthVolume().toFixed(2), '0.90');
    assert.strictEqual(make({ volume: 100 }).core.synthVolume().toFixed(2), '1.40');
    assert.strictEqual(make({ volume: 10 }).core.synthVolume().toFixed(2), '0.18');
    assert.strictEqual(make({ volume: undefined }).core.synthVolume().toFixed(2), '0.90');
    const v = require('../apps/lfb_enginesound/app.json').settings.find((s) => s.key === 'volume');
    assert.strictEqual(v.default, 50);
}
// rpmMap "locked" (the default): a fixed ratio per preset, the firing order on an even order of the car's
// four-cylinder (order 2), capped just over the redline
{
    const spec = require('../apps/lfb_enginesound/app.json').settings.find((x) => x.key === 'rpmMap');
    assert.strictEqual(spec.default, 'locked');
    const fire = { i6: 3, v8: 4, v10: 5, v12: 6 };
    Object.keys(fire).forEach((k) => {
        const { core } = make({ rpmMap: 'locked', engine: k });
        const order = fire[k] * core.virtualRpm(3000) / 3000;
        assert(Math.abs(order - Math.round(order)) < 1e-9 && Math.round(order) % 2 === 0, k + ' fires at order ' + order);
        assert(core.virtualRpm(4800) <= PRESETS[k].red * 1.03 + 1e-6 && core.virtualRpm(4800) > PRESETS[k].red * 0.85, k + ' top');
        assert(core.virtualRpm(9000) <= PRESETS[k].red * 1.03 + 1e-6);
    });
}
// between the car's samples the rpm sent moves on along the climb (track.js)
{
    const { core, sent, step } = make({ rpmMap: 'real' });
    core.feed({ rpm: 2000, speed: 50 }); step(60); core.feed({ rpm: 2200 }); step(60); core.feed({ rpm: 2400 });
    core.tick();                                  // starts playing
    step(30); core.tick();
    const p = Number(sent.filter((l) => l[0] === 'P').pop().split(' ')[1]);
    assert(p > 2450 && p < 2550, 'estimated ahead: ' + p);
    assert.strictEqual(core.d.rpm, 2400, 'the reading itself is kept');
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
    assert(sent.indexOf('ENGINE simulate') >= 0 && sent.indexOf('M 1') >= 0 && sent.indexOf('E v8') >= 0 && sent.indexOf('V 0.90') >= 0);
    core.tick();
    assert.strictEqual(core.reason, 'no engine data');
    core.feed({ rpm: 1500, speed: 30, gear: 2, torque: 120 });
    core.tick();
    assert.strictEqual(core.reason, null);
    assert(sent.indexOf('M 0') >= 0, 'unmuted');
    assert(sent.indexOf('OUT ' + DEVICE) >= 0, 'device');
    assert.deepStrictEqual(routed[0], ['connect', 287]);
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
    assert.deepStrictEqual(routed[0], ['connect', 287]);
}
// idle learning moves towards the car's idle
{
    const { core } = make();
    for (let i = 0; i < 500; i++) core.feed({ rpm: 760, speed: 0, torque: 20 });
    assert(Math.abs(core.idle - 760) < 5, 'idle ' + core.idle);
}

/* ---------------- audio routes ---------------- */
// the SmartX playback endpoints and the AudioManager's sources of a G20 head unit (its audio report, 2026-11)
const ENDPOINTS = ['AndroidAutoGuidance_CF_24_MONO', 'AndroidAutoMedia_CF_48_STEREO', 'CarLifeMedia_CF_48_STEREO', 'CarLifeNavigation_CF_24_MONO',
    'CarLifeSpeech_CF_24_MONO', 'CarPlayMain_CF_48_STEREO', 'CarPlayMain_Mono_CF_24_MONO', 'Media_Overlay_Converter_Source_CF_24_MONO',
    'Nuance_Navi_physical_Source_CF_24_MONO', 'TencentOverlay_CF_24_MONO', 'TencentSpeech_CF_24_MONO', 'TencentTTS_CF_24_MONO'];
const SOURCES = [{ id: 1, name: 'FmDABCombined' }, { id: 266, name: 'CarPlayMedia' }, { id: 271, name: 'AndroidAutoGuidance' }, { id: 275, name: 'Navigation' },
    { id: 279, name: 'TTS' }, { id: 287, name: 'CarLifeMedia' }, { id: 288, name: 'CarLifeNavigation' }, { id: 289, name: 'CarLifeSpeech' },
    { id: 294, name: 'TencentTTS' }, { id: 297, name: 'TencentOverlay' }, { id: 1046, name: 'OAPGenericMedia' }, { id: 1047, name: 'OAPGenericInterrupt' },
    { id: 1048, name: 'OAPGenericOverlay' }, { id: 1051, name: 'DynamicOverlay' }];
// the SmartX endpoint of a name: its prefix, case-insensitive, 48 kHz stereo preferred
{
    assert.strictEqual(deviceFor('TencentOverlay', ENDPOINTS), 'plug:smartx:TencentOverlay_CF_24_MONO');
    assert.strictEqual(deviceFor('carlifenavigation', ENDPOINTS), 'plug:smartx:CarLifeNavigation_CF_24_MONO');
    assert.strictEqual(deviceFor('CarPlayMain', ENDPOINTS), 'plug:smartx:CarPlayMain_CF_48_STEREO', '48 kHz stereo first');
    assert.strictEqual(deviceFor('OAPGenericOverlay', ENDPOINTS), null, 'the OAP sources have no endpoint of their own');
    assert.strictEqual(deviceFor('TencentOverlay', null), null);
}
// mix candidates: the default (OAPGenericOverlay through the overlay converter's endpoint), then the China-only prompt
// channels with an endpoint, to the front speakers and then to the main sink; never BMW's own navigation / speech,
// CarPlay or Android Auto
{
    const c = mixCandidates(SOURCES, ENDPOINTS);
    assert.deepStrictEqual(c.map((x) => x.source + '>' + x.sink), ['1048>1024', '297>1024', '288>1024', '294>1024', '289>1024', '1048>1', '297>1', '288>1', '294>1', '289>1']);
    assert.deepStrictEqual(c.slice(0, 3).map((x) => x.device), ['plug:smartx:Media_Overlay_Converter_Source_CF_24_MONO', 'plug:smartx:TencentOverlay_CF_24_MONO', 'plug:smartx:CarLifeNavigation_CF_24_MONO']);
    assert.deepStrictEqual(c.slice(0, 5).map((x) => x.markAvailable), [false, true, true, true, true], 'only the China-only ones are marked available');
    assert(c.every((x) => x.kind === 'mix'));
    // a source the car does not list, or one without an endpoint, is no candidate
    assert.deepStrictEqual(mixCandidates(SOURCES.filter((x) => x.id !== 297), ENDPOINTS).filter((x) => x.sink === 1024).map((x) => x.source), [1048, 288, 294, 289]);
    assert.deepStrictEqual(mixCandidates(SOURCES, ENDPOINTS.filter((x) => !/CarLife/.test(x))).filter((x) => x.sink === 1024).map((x) => x.source), [1048, 297, 294]);
    // no source list: the known ids; no lists at all: only the default, on its assumed device
    assert.deepStrictEqual(mixCandidates(null, ENDPOINTS).filter((x) => x.sink === 1024).map((x) => x.source), [1048, 297, 288, 294, 289]);
    assert.deepStrictEqual(mixCandidates(null, null).map((x) => [x.source, x.sink, x.device]), [[1048, 1024, MIX_DEFAULT.device], [1048, 1, MIX_DEFAULT.device]]);
    assert.strictEqual(MIX_DEFAULT.device, 'plug:smartx:Media_Overlay_Converter_Source_CF_24_MONO');
}
// mix or exclusive
{
    const tencent = { name: 'TencentOverlay', source: 297, sink: 1, device: 'plug:smartx:saved' };
    const ex = routeFor(false, tencent, ENDPOINTS);
    assert.deepStrictEqual([ex.source, ex.sink, ex.device, ex.markAvailable], [287, 1024, 'plug:smartx:CarLifeMedia_CF_48_STEREO', true]);
    const def = routeFor(true, null, ['Media_Overlay_Converter_Source_CF_24_MONO']);
    assert.deepStrictEqual([def.source, def.sink, def.device, def.markAvailable, def.note], [1048, 1024, 'plug:smartx:Media_Overlay_Converter_Source_CF_24_MONO', false, undefined]);
    assert.strictEqual(routeFor(true, null, null).device, MIX_DEFAULT.device, 'no SHM list: the assumed device');
    assert(/assumed/.test(routeFor(true, null, ['CarLifeMedia_CF_48_STEREO']).note), 'SHM known, no converter endpoint: said so');
    // a chosen route: its sink and source kept, the device from the list, marked available as its kind says
    const t = mixRoute(tencent, ENDPOINTS);
    assert.deepStrictEqual([t.source, t.sink, t.device, t.markAvailable, t.kind], [297, 1, 'plug:smartx:TencentOverlay_CF_24_MONO', true, 'mix']);
    assert.strictEqual(mixRoute(tencent, null).device, 'plug:smartx:saved', 'SHM unknown: the saved device');
    const main = mixRoute({ name: 'OAPGenericOverlay', source: 1048, sink: 1, device: 'x' }, ENDPOINTS);
    assert.deepStrictEqual([main.source, main.sink, main.markAvailable], [1048, 1, false], 'the default source to the main sink is a choice of its own');
    const gone = mixRoute(tencent, ['Media_Overlay_Converter_Source_CF_24_MONO']);
    assert.deepStrictEqual([gone.source, gone.sink], [1048, 1024], 'endpoint gone: default');
    assert(/default used/.test(gone.note));
    // a choice saved by an earlier version that is no mix source any more
    const old = mixRoute({ name: 'OAPGenericInterrupt', source: 1047, sink: 1024, device: 'plug:smartx:OAPGenericInterrupt_CF_48_STEREO' }, ENDPOINTS);
    assert.strictEqual(old.source, 1048);
    assert(/no mix source/.test(old.note));
}
// the app: exclusive by default; mix on -> the mix route (reroute while playing); the mix device from SHM
{
    const { core, sent, routed, vals, daemon, step } = make();
    core.feed({ rpm: 1500, speed: 30 });
    core.tick();
    assert.deepStrictEqual(routed, [['connect', 287]]);
    daemon.shm = { endpoints: ENDPOINTS };
    vals.mix = true; core.onSetting('mix');
    assert.deepStrictEqual(routed[1], ['disconnect']);
    core.feed({ rpm: 1500 }); step(20); core.tick();
    assert.deepStrictEqual(routed[2], ['connect', 1048]);
    assert.strictEqual(sent.filter((l) => /^OUT /.test(l)).pop(), 'OUT plug:smartx:Media_Overlay_Converter_Source_CF_24_MONO');
    // a new mix choice
    core.setMixChoice({ name: 'TencentOverlay', source: 297, sink: 1024, device: 'plug:smartx:x' });
    core.feed({ rpm: 1500 }); step(20); core.tick();
    assert.deepStrictEqual(routed.slice(3), [['disconnect'], ['connect', 297]]);
    assert.strictEqual(sent.filter((l) => /^OUT /.test(l)).pop(), 'OUT plug:smartx:TencentOverlay_CF_24_MONO');
}
// the test sound: through a given route, then that route goes and tick() connects the active one again
{
    const { core, sent, routed, step } = make();
    const cand = mixCandidates(SOURCES, ENDPOINTS)[1];
    core.test(cand);
    assert.deepStrictEqual(routed[0], ['connect', 297]);
    assert(sent.indexOf('OUT ' + cand.device) >= 0);
    core.feed({ rpm: 1500, speed: 30 }); core.tick();
    assert.strictEqual(routed.length, 1, 'the test owns the route');
    core.endTest();
    assert.deepStrictEqual(routed[1], ['disconnect']);
    step(20); core.feed({ rpm: 1500 }); core.tick();
    assert.deepStrictEqual(routed[2], ['connect', 287]);
}

/* ---------------- route.js against a fake AudioManager ---------------- */
function fakeAM(opts) {
    opts = opts || {};
    const calls = [];
    const conns = (opts.conns || []).slice();
    let next = 50;
    const signals = {
        proxy: async (iface) => ({ iface }),
        onAvailable: (p, fn) => fn(true),
        call: async (p, m, a) => {
            calls.push(m === 'hookSourceAvailablityStatusChange' ? 'avail ' + a.sourceID + ' ' + a.availability.availability
                : m === 'connect' ? 'connect ' + a.sourceID : m === 'disconnect' ? 'disconnect ' + a.mainConnectionID : m);
            if (m === 'connect') {
                if (opts.throwOn === 'connect') throw new Error('timeout');
                if (opts.refuse) return { amError: 'E_NOT_POSSIBLE' };
                const id = next++;
                conns.push({ mainConnectionID: id, sourceID: a.sourceID, sinkID: a.sinkID, connectionState: opts.state || 2 });
                return { amError: 'E_OK', mainConnectionID: id };
            }
            if (m === 'disconnect') { const i = conns.findIndex((c) => c.mainConnectionID === a.mainConnectionID); if (i >= 0) conns.splice(i, 1); return { amError: 'E_OK' }; }
            if (m === 'getListMainConnections') return { amError: 'E_OK', listConnections: conns.slice() };
            if (m === 'getListMainSources') { if (opts.noSources) throw new Error('Service unavailable'); return { amError: 'E_OK', listMainSources: SOURCES.map((s) => ({ sourceID: s.id, name: s.name })) }; }
            return { amError: 'E_OK' };
        },
    };
    return { signals, calls, conns };
}
const quiet = { info() {}, warn() {} };
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'es-route-'));

async function routeTests() {
    // exclusive: marked available, connected; released: disconnected, swept, unavailable (as Moonride)
    {
        const am = fakeAM(), r = new Route(am.signals, quiet, tmp());
        assert.strictEqual(await r.connect(routeFor(false)), true);
        assert.deepStrictEqual(am.calls, ['avail 287 A_AVAILABLE', 'connect 287']);
        await r.disconnect();
        assert.deepStrictEqual(am.calls.slice(2), ['disconnect 50', 'getListMainConnections', 'avail 287 A_UNAVAILABLE']);
        assert.strictEqual(am.conns.length, 0);
    }
    // mix: never marked available; only our own connection is disconnected
    {
        const other = { mainConnectionID: 7, sourceID: 1048, sinkID: 1024, connectionState: 2 };
        const am = fakeAM({ conns: [other] }), dir = tmp(), r = new Route(am.signals, quiet, dir);
        assert.strictEqual(await r.connect(routeFor(true, null, null)), true);
        assert(fs.existsSync(path.join(dir, 'route-state.json')), 'our connection noted');
        await r.disconnect();
        assert.deepStrictEqual(am.calls, ['connect 1048', 'disconnect 50']);
        assert.deepStrictEqual(am.conns, [other]);
        assert(!fs.existsSync(path.join(dir, 'route-state.json')));
    }
    // a China-only mix source: marked available while it plays (noted, so a start after a crash removes the mark too)
    {
        const am = fakeAM(), dir = tmp(), r = new Route(am.signals, quiet, dir);
        const t = mixRoute({ name: 'TencentOverlay', source: 297, sink: 1024 }, ENDPOINTS);
        assert.strictEqual(await r.connect(t), true);
        assert.deepStrictEqual(am.calls, ['avail 297 A_AVAILABLE', 'connect 297']);
        assert.deepStrictEqual(JSON.parse(fs.readFileSync(path.join(dir, 'route-state.json'), 'utf8')), { source: 297, sink: 1024, id: 50, marked: true });
        await r.disconnect();
        assert.deepStrictEqual(am.calls.slice(2), ['disconnect 50', 'avail 297 A_UNAVAILABLE']);
        assert.strictEqual(am.conns.length, 0);
        assert(!fs.existsSync(path.join(dir, 'route-state.json')));
        // the crash: the noted connection and its mark go at the next start
        fs.writeFileSync(path.join(dir, 'route-state.json'), JSON.stringify({ source: 297, sink: 1024, id: 9, marked: true }));
        const am2 = fakeAM({ conns: [{ mainConnectionID: 9, sourceID: 297, sinkID: 1024 }] }), r2 = new Route(am2.signals, quiet, dir);
        await r2.start();
        assert.strictEqual(am2.conns.length, 0);
        assert(am2.calls.indexOf('avail 297 A_UNAVAILABLE') >= 0, String(am2.calls));
    }
    // a failed connect leaves nothing behind: refused, or thrown after marking available
    {
        const am = fakeAM({ refuse: true }), r = new Route(am.signals, quiet, tmp());
        assert.strictEqual(await r.connect(routeFor(false)), false);
        assert.strictEqual(r.state, 'failed');
        assert.strictEqual(am.calls[am.calls.length - 1], 'avail 287 A_UNAVAILABLE');
        const am2 = fakeAM({ throwOn: 'connect' }), r2 = new Route(am2.signals, quiet, tmp());
        assert.strictEqual(await r2.connect(routeFor(false)), false);
        assert.strictEqual(am2.calls[am2.calls.length - 1], 'avail 287 A_UNAVAILABLE');
        const am3 = fakeAM({ refuse: true }), r3 = new Route(am3.signals, quiet, tmp());
        assert.strictEqual(await r3.connect(routeFor(true, null, null)), false);
        assert(am3.calls.every((c) => !/^avail/.test(c)), 'mix: no availability calls');
        assert(/refused/.test(r3.error));
    }
    // a mix connection that stays CS_CONNECTING is released and shown as failed; a connected one stays
    {
        const am = fakeAM({ state: 1 }), r = new Route(am.signals, quiet, tmp());
        r.checkMs = 5; r.settleMs = 20;
        assert.strictEqual(await r.connect(routeFor(true, null, null)), true);
        await new Promise((res) => setTimeout(res, 60));
        await r.q;
        assert.strictEqual(r.state, 'failed');
        assert(/CS_CONNECTING/.test(r.error), r.error);
        assert.strictEqual(am.conns.length, 0);
        const am2 = fakeAM(), r2 = new Route(am2.signals, quiet, tmp());
        r2.checkMs = 5; r2.settleMs = 20;
        await r2.connect(routeFor(true, null, null));
        await new Promise((res) => setTimeout(res, 60));
        await r2.q;
        assert.deepStrictEqual([r2.state, r2.connState, am2.conns.length], ['connected', 'CS_CONNECTED', 1]);
    }
    // switching routes: the old one goes first
    {
        const am = fakeAM(), r = new Route(am.signals, quiet, tmp());
        await r.connect(routeFor(false));
        await r.connect(routeFor(true, null, null));
        assert.deepStrictEqual(am.conns.map((c) => c.sourceID), [1048]);
        assert(am.calls.indexOf('avail 287 A_UNAVAILABLE') >= 0);
    }
    // start: every 287 leftover and our noted mix connection go; another 1048 connection stays
    {
        const dir = tmp();
        fs.writeFileSync(path.join(dir, 'route-state.json'), JSON.stringify({ source: 1048, sink: 1024, id: 9 }));
        const conns = [{ mainConnectionID: 3, sourceID: 287, sinkID: 1024 }, { mainConnectionID: 9, sourceID: 1048, sinkID: 1024 },
            { mainConnectionID: 7, sourceID: 1048, sinkID: 1024 }];
        const am = fakeAM({ conns }), r = new Route(am.signals, quiet, dir);
        await r.start();
        assert.deepStrictEqual(am.conns.map((c) => c.mainConnectionID), [7]);
        assert(am.calls.indexOf('avail 287 A_UNAVAILABLE') >= 0);
        assert(!fs.existsSync(path.join(dir, 'route-state.json')));
    }
    // the source list (Audio page); unavailable: it throws (the page shows it, the default is used)
    {
        const am = fakeAM(), r = new Route(am.signals, quiet, tmp());
        assert.deepStrictEqual((await r.sources()).map((s) => s.id), SOURCES.map((s) => s.id));
        await assert.rejects(new Route(fakeAM({ noSources: true }).signals, quiet, tmp()).sources());
    }
}

// the test tone on the active route: route + TONE once connected (a promise later)
{
    const { core, sent, routed } = make();
    core.test();
    assert.deepStrictEqual(routed[0], ['connect', 287]);
    assert(sent.indexOf('OUT ' + DEVICE) >= 0);
    setImmediate(() => {
        assert(sent.indexOf('TONE ' + DEVICE) >= 0, 'tone after connect');
        routeTests().then(() => { console.log('core.test.js: ok'); process.exit(0); }, (e) => { console.error(e); process.exit(1); });
    });
}

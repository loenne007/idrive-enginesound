'use strict';
/**
 * Engine Sound - a synthesized petrol engine (inline-6, V8, V10, V12) on the car's speakers, following the car's
 * real rpm, gear and load.
 *
 * The app runs from the platform's start (app.json "autostart"): background() reads the car, drives the synthesizer
 * service (tool lfb-enginesound, see core.js / daemon.js) and the car's audio routing (route.js), with the app closed.
 *
 * Pages:
 *   Engine   what plays and why (or why not), the car's rpm and the engine's, gear, load; the engine choice, volume,
 *            on / off, mix / exclusive
 *   Audio    the routing: the service, the SmartX endpoints, the AudioManager's sources; test a route with a tone
 *            and take it for Mix or Exclusive (saved in the app's data folder, routes.json)
 */
const fs = require('fs');
const path = require('path');
const mgu = require('./kit/mgu');
const G = require('./kit/gauges');
const { withAlpha } = require('./kit/canvas');
const Car = require('./kit/car');
const { Core, PRESETS, ORDER, DEFAULT_ROUTES } = require('./core');
const { Daemon } = require('./daemon');
const { Route } = require('./route');

const W = 1600, H = 520;
const TABS = ['Engine', 'Audio'];
const VOLS = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100];
const FES = 'de.bmw.infotainment.drivingexcitement.FES';
// FES torque: 12 bit, 0.5 Nm per bit, 0 Nm at raw 2047, 4095 = no value (kit/runtime/car.js)
const torqueOf = (raw) => (typeof raw === 'number' && raw < 4095 ? raw * 0.5 - 1023.5 : null);
// FES actualValueModeDrivingExperience -> sport / other (as ambient_config's modeOf)
const SPORT_NO = [1, 2, 3, 15, 16, 23, 28];
function modeOf(v) {
    const raw = v && typeof v === 'object' ? v.actualValueModeDrivingExperience : v;
    if (raw === undefined || raw === null || raw === '') return undefined;
    if (typeof raw === 'number' || /^\d+$/.test(String(raw))) return SPORT_NO.indexOf(Number(raw)) >= 0 ? 'sport' : 'other';
    return /SPORT|BOOST/i.test(String(raw)) ? 'sport' : 'other';
}

async function background(bg) {
    const { settings, signals, log, dataDir } = bg;
    const routesFile = path.join(dataDir || '.', 'routes.json');
    let saved = {};
    try { saved = JSON.parse(fs.readFileSync(routesFile, 'utf8')) || {}; } catch (e) { saved = {}; }
    const routes = () => ({ mix: saved.mix || DEFAULT_ROUTES.mix, exclusive: saved.exclusive || DEFAULT_ROUTES.exclusive });
    const saveRoute = (mode, r) => {
        saved[mode] = r;
        try { fs.writeFileSync(routesFile + '.tmp', JSON.stringify(saved)); fs.renameSync(routesFile + '.tmp', routesFile); } catch (e) { log.warn('routes: ' + e); }
    };

    const daemon = new Daemon(log);
    const route = new Route(signals, log);
    const core = new Core({ settings, log, daemon, route, routes });

    const fesSeen = [];
    const car = new Car(bg, ['rpm', 'speed', 'gear', 'condition'], {
        [FES]: {
            statusActualValueTorque2: (v) => core.feed({ torque: torqueOf(v && v.actualValueTorqueCrankshaft) }),
            statusFESVehicleDynamicSwitchHMI: (v) => {
                const raw = v && v.actualValueModeDrivingExperience;
                if (fesSeen.length < 10 && fesSeen.indexOf(String(raw)) < 0) { fesSeen.push(String(raw)); log.info('driving mode: ' + JSON.stringify(raw)); }
                const m = modeOf(v);
                if (m) core.feed({ mode: m });
            },
        },
    });
    car.setRedline(4800);
    car.onSample((d) => core.feed({ rpm: d.rpm, speed: d.speed, gear: d.gear, condition: d.condition, demo: d.demo,
        torque: d.demo && typeof d.torque === 'number' ? d.torque : undefined }));
    settings.onChange((key) => core.onSetting(key));
    core.start();
    log.info('engine sound: background running');
    return { core, car, daemon, route, routes, saveRoute };
}

/* ---------------- the Audio page's route candidates ---------------- */
// a SmartX endpoint whose name starts with an AudioManager source's name is that source's PCM (CarLifeMedia_CF_48_STEREO)
function candidates(lists, shm, routes) {
    const out = [];
    const add = (r) => { if (!out.some((x) => x.source === r.source && x.device === r.device)) out.push(r); };
    const r = routes();
    add(Object.assign({ tag: 'mix' }, r.mix)); add(Object.assign({ tag: 'exclusive' }, r.exclusive));
    const sources = (lists && lists.sources) || [];
    ((shm && shm.endpoints) || []).forEach((ep) => {
        const base = ep.split('_CF_')[0].toLowerCase();
        const s = sources.find((x) => x.name && x.name.toLowerCase() === base);
        if (s) add({ source: s.id, sink: 1024, device: 'plug:smartx:' + ep, name: s.name });
    });
    return out;
}

mgu.run({
    background,
    async start(ctx) {
        const { ui, settings, log } = ctx;
        const { core, daemon, route, routes, saveRoute } = ctx.background || await background(ctx);
        let page = 0, dirty = true, lists = null, listsBusy = false, testing = null, srcPage = 0, note = null;
        const redraw = () => { dirty = true; return render(); };
        const setNote = (s) => { note = { s, t: Date.now() }; dirty = true; };

        const step = (key, list, d) => {
            const v = settings.get(key), i = Math.max(0, list.indexOf(v));
            settings.set(key, list[Math.max(0, Math.min(list.length - 1, i + d))]);
            return redraw();
        };
        ui.onClick('BTN_TOGGLE', () => { settings.set('enabled', settings.get('enabled') === false); return redraw(); });
        ui.onClick('BTN_ENGINE', () => { const i = ORDER.indexOf(core.preset()); settings.set('engine', ORDER[(i + 1) % ORDER.length]); return redraw(); });

        function refreshLists() {
            if (listsBusy) return;
            listsBusy = true; daemon.send('SHM');
            route.lists().then((l) => { lists = l; }, (e) => { lists = { sources: [], sinks: [], connections: [], errors: [String(e)] }; })
                .then(() => { listsBusy = false; dirty = true; });
        }

        async function test(r) {
            if (testing) return;
            testing = r; setNote('Testing ' + r.name + ': listen for beeps (10 s)');
            core.routeWant = null;
            await route.connect(r);
            daemon.send('TONE ' + r.device);
            setTimeout(() => {
                testing = null; core.routeWant = null;
                if (!core.playing) route.disconnect(); else core._route(true);
                setNote('Test of ' + r.name + ' done. Heard it? Use it for Mix or Exclusive.');
                refreshLists();
            }, 11000);
        }

        /* ---------- Engine page ---------- */
        function drawEngine(cv, t) {
            const p = PRESETS[core.preset()], on = settings.get('enabled') !== false;
            const why = core.reason, d = core.d, o = core.out;
            // status
            G.panel(cv, t, 24, 44, 520, 468, 'Engine Sound');
            const col = !on ? t.dim : why ? t.warn : '#2fd27a';
            cv.roundRect(48, 104, 472, 64, 14, withAlpha(col, 0.18));
            cv.text(!on ? 'OFF' : why ? 'STANDBY' : 'PLAYING', 284, 148, 34, col, { align: 'center', font: 'bold', spacing: 2 });
            cv.text(on && why ? why : (settings.get('mode') === 'exclusive' ? 'Exclusive: the engine replaces the music' : 'Mix: over the music'), 284, 196, 20, t.dim, { align: 'center' });
            const rs = route.state === 'failed' ? 'Audio route failed: ' + (route.error || '') : route.state === 'connecting' ? 'Audio route connecting...' : route.state === 'connected' ? 'Audio route connected' : 'Audio route idle';
            cv.text(rs.slice(0, 48), 284, 226, 18, route.state === 'failed' ? t.warn : t.dim, { align: 'center' });
            if (!daemon.connected) cv.text('Engine Sound service not running', 284, 252, 18, t.warn, { align: 'center' });
            G.button(cv, t, 48, 284, 472, 56, on ? 'Switch off' : 'Switch on', () => { settings.set('enabled', !on); return redraw(); }, { primary: !on, key: 'onoff' });
            G.button(cv, t, 48, 354, 230, 52, 'Mix', () => { settings.set('mode', 'mix'); return redraw(); }, { primary: settings.get('mode') !== 'exclusive', key: 'mix' });
            G.button(cv, t, 290, 354, 230, 52, 'Exclusive', () => { settings.set('mode', 'exclusive'); return redraw(); }, { primary: settings.get('mode') === 'exclusive', key: 'excl' });
            const vol = Number(settings.get('volume')) || 60;
            G.button(cv, t, 48, 424, 80, 52, '-', () => step('volume', VOLS, -1), { key: 'vol-' });
            cv.text('VOLUME ' + vol + '%', 284, 458, 22, t.text, { align: 'center', font: 'bold', spacing: 1.5 });
            G.button(cv, t, 440, 424, 80, 52, '+', () => step('volume', VOLS, 1), { key: 'vol+' });

            // live
            G.panel(cv, t, 560, 44, 500, 468, 'Live');
            const rpm = typeof d.rpm === 'number' ? Math.round(d.rpm) : null;
            cv.text(rpm === null ? '-' : String(rpm), 700, 176, 72, t.text, { align: 'center', font: 'num' });
            cv.text('CAR RPM', 700, 206, 16, t.dim, { align: 'center', font: 'bold', spacing: 1.5 });
            cv.text(rpm === null ? '-' : String(Math.round(o.rpm)), 920, 176, 72, why ? t.dim : t.accent, { align: 'center', font: 'num' });
            cv.text(p.title.toUpperCase() + ' RPM', 920, 206, 16, t.dim, { align: 'center', font: 'bold', spacing: 1.5 });
            // rev bar of the virtual engine
            const fr = Math.max(0, Math.min(1, o.rpm / (p.red * 1.05)));
            cv.roundRect(590, 236, 440, 22, 11, withAlpha('#ffffff', 0.08));
            cv.roundRect(590, 236, Math.max(22, 440 * fr), 22, 11, fr > p.red / (p.red * 1.05) * 0.9 ? t.crit : t.accent);
            // load: pulling right, overrun left
            const ld = core.load(d.torque);
            cv.text('LOAD', 590, 300, 16, t.dim, { font: 'bold', spacing: 1.5 });
            cv.roundRect(660, 284, 370, 20, 10, withAlpha('#ffffff', 0.08));
            const mid = 660 + 370 * 0.25;
            if (ld >= 0) cv.roundRect(mid, 284, Math.max(10, 370 * 0.75 * ld), 20, 10, '#2fd27a');
            else cv.roundRect(mid - 370 * 0.25 * -ld, 284, Math.max(10, 370 * 0.25 * -ld), 20, 10, t.cold || '#4aa3ff');
            cv.text(typeof d.torque === 'number' ? Math.round(d.torque) + ' Nm' : 'no torque signal', 1030, 330, 18, t.dim, { align: 'right' });
            G.gear(cv, t, 700, 470, { gear: d.gear, size: 120 });
            cv.text('GEAR', 700, 494, 14, t.dim, { align: 'center', font: 'bold', spacing: 1.5 });
            cv.text(typeof d.speed === 'number' ? String(Math.round(d.speed)) : '-', 920, 450, 56, t.text, { align: 'center', font: 'num' });
            cv.text('KM/H', 920, 480, 14, t.dim, { align: 'center', font: 'bold', spacing: 1.5 });
            if (d.demo) cv.text('DEMO DATA', 1030, 494, 16, t.warn, { align: 'right', font: 'bold' });

            // engines
            G.panel(cv, t, 1076, 44, 500, 468, 'Engine');
            ORDER.forEach((k, i) => {
                const q = PRESETS[k], sel = k === core.preset(), y = 100 + i * 92;
                cv.roundRect(1100, y, 452, 80, 14, sel ? withAlpha(t.accent, 0.3) : withAlpha('#ffffff', 0.05));
                if (sel) cv.strokeRoundRect(1100, y, 452, 80, 14, 2, t.accent);
                cv.text(q.title, 1124, y + 36, 28, t.text, { font: 'bold' });
                cv.text(q.sub + ' - ' + q.red + ' rpm', 1124, y + 64, 17, t.dim);
                if (cv.own) cv.target(1100, y, 452, 80, () => { settings.set('engine', k); return redraw(); }, 'eng-' + k);
            });
            const tb = PRESETS[core.preset()].turbo;
            cv.text(tb ? 'Turbo sound: ' + (settings.get('turbo') !== false ? 'on' : 'off') + ' (Settings)' : 'Naturally aspirated', 1124, 488, 17, t.dim);
        }

        /* ---------- Audio page ---------- */
        function drawAudio(cv, t) {
            G.panel(cv, t, 24, 44, 600, 468, 'Routing');
            const st = daemon.status;
            const lines = [
                ['Service', daemon.connected ? 'running' + (st ? ' (' + st.synth + ')' : '') : 'not reachable'],
                ['Output', (st && st.device) || '-'],
                ['Route', route.state + (route.conn ? ' ' + route.conn.source + ' -> ' + route.conn.sink : '')],
                ['Mix', routes().mix.name + ' ' + routes().mix.source],
                ['Exclusive', routes().exclusive.name + ' ' + routes().exclusive.source],
            ];
            lines.forEach((l, i) => {
                cv.text(l[0], 48, 108 + i * 32, 19, t.dim);
                cv.text(String(l[1]).slice(0, 44), 190, 108 + i * 32, 19, t.text);
            });
            if (route.error) cv.text(String(route.error).slice(0, 56), 48, 276, 17, t.warn);
            const logs = (st && st.log) || [];
            logs.slice(-6).forEach((l, i) => cv.text(String(l).slice(0, 64), 48, 310 + i * 24, 15, t.dim));
            if (note && Date.now() - note.t < 30000) cv.text(note.s.slice(0, 60), 48, 470, 18, t.accent, { font: 'bold' });
            G.button(cv, t, 420, 58, 180, 40, 'Refresh', () => { refreshLists(); return redraw(); }, { key: 'refresh' });

            G.panel(cv, t, 640, 44, 936, 468, 'Routes to try');
            const cs = candidates(lists, daemon.shm, routes);
            if (!lists) cv.text(listsBusy ? 'Reading the AudioManager...' : 'Tap Refresh', 664, 110, 20, t.dim);
            cs.slice(0, 5).forEach((r, i) => {
                const y = 96 + i * 66;
                cv.text(r.name + ' (' + r.source + ')', 664, y + 22, 20, t.text, { font: 'bold' });
                cv.text(r.device.replace('plug:smartx:', '').slice(0, 40), 664, y + 46, 15, t.dim);
                G.button(cv, t, 1120, y + 4, 130, 44, testing === r ? '...' : 'Test', () => { test(r); return redraw(); }, { key: 'test' + i, busy: !!testing });
                G.button(cv, t, 1262, y + 4, 140, 44, 'Use mix', () => { saveRoute('mix', strip(r)); setNote(r.name + ' is used for Mix'); core.onSetting('mode'); return redraw(); }, { key: 'mix' + i, primary: r.tag === 'mix' });
                G.button(cv, t, 1414, y + 4, 140, 44, 'Use excl.', () => { saveRoute('exclusive', strip(r)); setNote(r.name + ' is used for Exclusive'); core.onSetting('mode'); return redraw(); }, { key: 'ex' + i, primary: r.tag === 'exclusive' });
            });
            // the AudioManager's sources, a page at a time
            const src = (lists && lists.sources) || [];
            if (src.length) {
                const per = 12, pages = Math.ceil(src.length / per);
                srcPage = Math.min(srcPage, pages - 1);
                cv.text('AudioManager sources ' + (srcPage + 1) + '/' + pages, 664, 448, 16, t.dim, { font: 'bold' });
                src.slice(srcPage * per, srcPage * per + per).forEach((s, i) => {
                    cv.text(s.id + ' ' + s.name.slice(0, 18), 664 + (i % 4) * 225, 472 + Math.floor(i / 4) * 18, 14, t.dim);
                });
                G.button(cv, t, 1430, 430, 120, 36, 'More', () => { srcPage = (srcPage + 1) % pages; return redraw(); }, { key: 'srcmore' });
            }
            if (lists && lists.errors && lists.errors.length) cv.text(lists.errors[0].slice(0, 70), 664, 430, 15, t.warn);
        }
        const strip = (r) => ({ source: r.source, sink: r.sink, device: r.device, name: r.name, markAvailable: !!r.markAvailable });

        async function render() {
            const t = ctx.theme(), cv = ctx.canvas(W, H).clear(t.bg);
            [drawEngine, drawAudio][page](cv, t);
            G.frame(cv, t, TABS, page, [core.d.demo && { text: 'DEMO' }]);
            dirty = false;
            await ui.image('MAIN_GFX', cv);
        }
        ctx.onTab((i) => { page = i; if (i === 1 && !lists) refreshLists(); return redraw(); });
        daemon.on('change', () => { dirty = true; });
        settings.onChange(() => { dirty = true; });
        let last = 0;
        ctx.loop('MAIN', 10, () => {
            const n = Date.now();
            // the live numbers: 10 times a second on the Engine page
            if (dirty || (page === 0 && n - last > 100) || (page === 1 && n - last > 1000)) { last = n; return render(); }
            return null;
        });
        render().catch((e) => log.warn('first render: ' + e));
    },
});


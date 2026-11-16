'use strict';
/**
 * Engine Sound - a synthesized engine sound (inline-6, V8, V10, V12, or bass for the diesel) on the car's speakers,
 * following the car's real rpm, gear and load.
 *
 * The app runs from the platform's start (app.json "autostart"): background() reads the car, drives the synthesizer
 * service (tool lfb-enginesound, see core.js / daemon.js) and the car's audio routing (route.js), with the app closed.
 *
 * Pages: Engine (what plays and why or why not, the car's rpm and the engine's, gear, load; the engine and the sound
 * engine, volume, on / off, a 10 s test sound), Sound (the sound sliders) and Audio (the audio route: exclusive, the
 * music pauses, or mix, over the music; the mix route candidates with a test each).
 */
const fs = require('fs');
const path = require('path');
const mgu = require('./kit/mgu');
const G = require('./kit/gauges');
const { withAlpha } = require('./kit/canvas');
const Car = require('./kit/car');
const { Core, PRESETS, ORDER, SOUND_ENGINES, SOUND_SETTINGS } = require('./core');
const { Daemon } = require('./daemon');
const { Route, MIX_DEFAULT, mixCandidates, mixRoute, sameRoute } = require('./route');

const W = 1600, H = 520;
const TABS = ['Engine', 'Sound', 'Audio'];
const MIX_ROWS = 5;           // mix route candidates shown on the Audio page
// the Sound page's sliders: track from SX0 to SX1, one row per setting
const SX0 = 520, SX1 = 1290, ROW0 = 112, ROWH = 76;
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

/* The mix route chosen on the Audio page, kept in the app's data folder (no file: the default). */
function mixStore(dataDir, log) {
    const file = path.join(dataDir || '.', 'mix-route.json');
    return {
        load() {
            try { const c = JSON.parse(fs.readFileSync(file, 'utf8')); return c && typeof c.source === 'number' && c.name ? c : null; } catch (e) { return null; }
        },
        save(c) {
            try { if (c) fs.writeFileSync(file, JSON.stringify({ name: c.name, source: c.source, sink: c.sink, device: c.device })); else if (fs.existsSync(file)) fs.unlinkSync(file); } catch (e) { log.warn('mix route save: ' + e); }
        },
    };
}

async function background(bg) {
    const { settings, signals, log } = bg;
    const daemon = new Daemon(log);
    const route = new Route(signals, log, bg.dataDir);
    const mix = mixStore(bg.dataDir, log);
    const core = new Core({ settings, log, daemon, route, mixChoice: mix.load() });
    route.start();      // a connection a crash or a power-off left goes first

    // the Audio page's lists: the AudioManager's sources (getListMainSources) and the service's SmartX endpoints (SHM)
    const audio = {
        sources: null, error: null, loading: false, at: 0,
        refresh() {
            if (audio.loading) return;
            audio.loading = true; audio.at = Date.now();
            daemon.send('SHM');
            route.sources().then((l) => { audio.sources = l; audio.error = null; log.info('audio: ' + l.length + ' sources'); },
                (e) => { audio.sources = null; audio.error = String(e && e.message || e); log.warn('audio: no source list: ' + audio.error); })
                .then(() => { audio.loading = false; daemon.emit('change'); });
        },
        candidates() { return mixCandidates(audio.sources, core.endpoints()); },
        use(c) {
            const d = !c || c.source === MIX_DEFAULT.source;
            log.info('audio: mix route ' + (d ? 'default' : c.name + ' ' + c.source + ' ' + c.device));
            mix.save(d ? null : c);
            core.setMixChoice(d ? null : c);
        },
    };

    const fesSeen = [];
    const car = new Car(bg, ['rpm', 'speed', 'gear'], {
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
    car.onSample((d) => core.feed({ rpm: d.rpm, speed: d.speed, gear: d.gear, demo: d.demo,
        torque: d.demo && typeof d.torque === 'number' ? d.torque : undefined }));
    settings.onChange((key) => core.onSetting(key));
    core.start();
    log.info('engine sound: background running');
    return { core, car, daemon, route, audio };
}

mgu.run({
    background,
    async start(ctx) {
        const { ui, settings, log } = ctx;
        const { core, daemon, route, audio } = ctx.background || await background(ctx);
        let dirty = true, page = 0;
        const redraw = () => { dirty = true; return render(); };

        const step = (key, list, d) => {
            const v = settings.get(key), i = Math.max(0, list.indexOf(v));
            settings.set(key, list[Math.max(0, Math.min(list.length - 1, i + d))]);
            return redraw();
        };
        ui.onClick('BTN_TOGGLE', () => { settings.set('enabled', settings.get('enabled') === false); return redraw(); });
        ui.onClick('BTN_TEST', () => { core.test(); return redraw(); });
        ui.onClick('BTN_ENGINE', () => { const i = ORDER.indexOf(core.preset()); settings.set('engine', ORDER[(i + 1) % ORDER.length]); return redraw(); });

        /* ---------- Engine page ---------- */
        function drawEngine(cv, t) {
            const p = core.presetInfo(core.preset()), on = settings.get('enabled') !== false;
            const red = p.red || Number(settings.get('realRedline')) || 4800;   // (D: the car's own range)
            const why = core.reason, d = core.d, o = core.out;
            // status
            G.panel(cv, t, 24, 44, 520, 468, 'Engine Sound');
            const col = !on ? t.dim : why ? t.warn : '#2fd27a';
            cv.roundRect(48, 104, 472, 64, 14, withAlpha(col, 0.18));
            cv.text(!on ? 'OFF' : why ? 'STANDBY' : 'PLAYING', 284, 148, 34, col, { align: 'center', font: 'bold', spacing: 2 });
            cv.text(on && why ? why : core.mixOn() ? 'Over the music (mix)' : 'On the speakers (the music pauses)', 284, 196, 20, t.dim, { align: 'center' });
            cv.text(routeText().slice(0, 52), 284, 226, 18, route.state === 'failed' ? t.warn : t.dim, { align: 'center' });
            const st = daemon.status, last = st && st.log && st.log.length ? st.log[st.log.length - 1] : '';
            if (!daemon.connected) cv.text('Engine Sound service not running (restart the head unit once)', 284, 252, 17, t.warn, { align: 'center' });
            else cv.text(String(last).slice(0, 52), 284, 252, 15, t.dim, { align: 'center' });
            G.button(cv, t, 48, 284, 472, 56, on ? 'Switch off' : 'Switch on', () => { settings.set('enabled', !on); return redraw(); }, { primary: !on, key: 'onoff' });
            const testing = Date.now() < core.testUntil;
            G.button(cv, t, 48, 354, 472, 52, testing ? 'Test sound: listen...' : 'Test sound (10 s beeps)', () => { core.test(); return redraw(); }, { key: 'test', busy: testing });
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
            const fr = Math.max(0, Math.min(1, o.rpm / (red * 1.05)));
            cv.roundRect(590, 236, 440, 22, 11, withAlpha('#ffffff', 0.08));
            cv.roundRect(590, 236, Math.max(22, 440 * fr), 22, 11, fr > 0.9 / 1.05 ? t.crit : t.accent);
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
                const q = core.presetInfo(k), sel = k === core.preset(), y = 96 + i * 76;
                cv.roundRect(1100, y, 452, 68, 14, sel ? withAlpha(t.accent, 0.3) : withAlpha('#ffffff', 0.05));
                if (sel) cv.strokeRoundRect(1100, y, 452, 68, 14, 2, t.accent);
                cv.text(q.title, 1124, y + 32, 26, t.text, { font: 'bold' });
                cv.text(q.red ? q.sub + ' - ' + q.red + ' rpm' : q.sub, 1124, y + 56, 16, t.dim);
                if (cv.own) cv.target(1100, y, 452, 68, () => { settings.set('engine', k); return redraw(); }, 'eng-' + k);
            });
            // the sound engine (es-synth-<id>): which synthesizer plays; greyed = not in the installed service
            const have = daemon.status && daemon.status.engines, se = core.soundEngine();
            cv.text('SOUND ENGINE', 1100, 422, 15, t.dim, { font: 'bold', spacing: 1.5 });
            const sw = (452 - 8 * (SOUND_ENGINES.length - 1)) / SOUND_ENGINES.length;
            SOUND_ENGINES.forEach((x, i) => {
                const there = !have || have.indexOf(x.id) >= 0;
                G.button(cv, t, 1100 + i * (sw + 8), 432, sw, 48, x.title, there ? () => { settings.set('soundEngine', x.id); return redraw(); } : null,
                    { primary: x.id === se, key: 'se-' + x.id, busy: !there });
            });
            const cur = SOUND_ENGINES.find((x) => x.id === se);
            cv.text(cur.sub + (PRESETS[core.preset()].turbo || cur.realRpm ? ' - turbo ' + (settings.get('turbo') !== false ? 'on' : 'off') : ''), 1100, 502, 15, t.dim);
        }

        /* ---------- Sound page: sliders for the X settings ---------- */
        const setPct = (key, v) => { settings.set(key, Math.max(0, Math.min(200, Math.round(v / 5) * 5))); return redraw(); };
        function drawSound(cv, t) {
            G.panel(cv, t, 24, 44, 1552, 468, 'Sound');
            G.button(cv, t, 1380, 58, 180, 40, 'Reset all', () => { SOUND_SETTINGS.forEach((x) => settings.set(x.key, 100)); return redraw(); }, { key: 'reset' });
            SOUND_SETTINGS.forEach((x, i) => {
                const y = ROW0 + i * ROWH, v = Math.round(core.soundSetting(x.key) * 100);
                cv.text(x.label, 48, y + 26, 24, t.text, { font: 'bold' });
                cv.text(x.sub, 48, y + 50, 16, t.dim);
                // track, the default (100 %) mark, the filled part and the knob
                const ty = y + 22, fx = SX0 + (SX1 - SX0) * v / 200;
                cv.roundRect(SX0, ty, SX1 - SX0, 14, 7, withAlpha('#ffffff', 0.1));
                cv.roundRect(SX0, ty, Math.max(14, fx - SX0), 14, 7, v === 0 ? t.dim : t.accent);
                cv.fillRect(SX0 + (SX1 - SX0) / 2 - 1, ty - 6, 2, 26, withAlpha('#ffffff', 0.35));
                cv.circle(fx, ty + 7, 15, '#ffffff');
                cv.text(v === 0 ? 'OFF' : v + '%', 1350, y + 34, 24, v === 100 ? t.dim : t.text, { align: 'center', font: 'bold' });
                G.button(cv, t, 1400, y + 6, 70, 48, '-', () => setPct(x.key, v - 10), { key: x.key + '-' });
                G.button(cv, t, 1484, y + 6, 70, 48, '+', () => setPct(x.key, v + 10), { key: x.key + '+' });
            });
        }
        // a tap on a slider's track sets it there
        if (ctx.onTap) {
            ctx.onTap((x, y) => {
                if (page !== 1 || x < SX0 - 20 || x > SX1 + 20) return null;
                const i = Math.floor((y - ROW0) / ROWH);
                if (i < 0 || i >= SOUND_SETTINGS.length) return null;
                return setPct(SOUND_SETTINGS[i].key, (x - SX0) / (SX1 - SX0) * 200);
            });
        }

        /* ---------- Audio page: exclusive or mix, the mix route candidates ---------- */
        /** The route's state in a line; full: with the AudioManager's connection state. */
        function routeText(full) {
            const r = route.route || route.failed;
            const n = r ? ' ' + r.name : '';
            return route.state === 'failed' ? 'Audio route' + n + ' failed: ' + (route.error || '')
                : route.state === 'connecting' ? 'Audio route' + n + ' connecting...'
                    : route.state === 'connected' ? 'Audio route' + n + ' connected' + (full && route.connState ? ' (' + route.connState + ')' : '')
                        : 'Audio route idle (connects when the sound plays)';
        }
        function drawAudio(cv, t) {
            const mixOn = core.mixOn(), r = core.activeRoute(), testing = Date.now() < core.testUntil;
            G.panel(cv, t, 24, 44, 600, 468, 'Audio route');
            G.button(cv, t, 48, 96, 268, 56, 'Exclusive', () => { settings.set('mix', false); return redraw(); }, { primary: !mixOn, key: 'mode-ex' });
            G.button(cv, t, 332, 96, 268, 56, 'Over the music', () => { settings.set('mix', true); return redraw(); }, { primary: mixOn, key: 'mode-mix' });
            cv.text(mixOn ? 'Mix: plays over the music, like a navigation prompt' : 'Exclusive: the music pauses while it plays', 48, 188, 17, t.dim);
            cv.text(r.name, 48, 232, 28, t.text, { font: 'bold' });
            cv.text('source ' + r.source + ' -> sink ' + r.sink, 600, 232, 17, t.dim, { align: 'right' });
            cv.text(r.device.replace(/^plug:smartx:/, 'SmartX ').slice(0, 56), 48, 262, 17, t.dim);
            cv.text(routeText(true).slice(0, 58), 48, 298, 17, route.state === 'failed' ? t.warn : route.state === 'connected' ? '#2fd27a' : t.dim);
            if (r.note) cv.text(r.note.slice(0, 58), 48, 326, 16, t.warn);
            G.button(cv, t, 48, 344, 552, 52, testing ? 'Test: listen...' : 'Test this route (10 s beeps)', testing ? null : () => { core.test(); return redraw(); },
                { key: 'test-route', busy: testing });
            const eps = core.endpoints();
            cv.text(audio.loading ? 'AudioManager: reading the sources...' : audio.sources ? 'AudioManager: ' + audio.sources.length + ' sources'
                : 'AudioManager list unavailable' + (audio.error ? ': ' + audio.error : '') + ' - default used', 48, 432, 16, audio.sources || audio.loading ? t.dim : t.warn);
            cv.text(eps ? 'SmartX: ' + eps.length + ' endpoints' : 'SmartX list unavailable (service) - default device assumed', 48, 458, 16, eps ? t.dim : t.warn);
            cv.text('Also in Settings: "Play over music (mix)"', 48, 490, 15, t.dim);

            G.panel(cv, t, 640, 44, 936, 468, 'Mix routes');
            G.button(cv, t, 1416, 58, 144, 40, audio.loading ? 'Reading...' : 'Refresh', audio.loading ? null : () => { audio.refresh(); return redraw(); }, { key: 'refresh', busy: audio.loading });
            const list = audio.candidates(), chosen = mixRoute(core.mixChoice, eps);
            list.slice(0, MIX_ROWS).forEach((c, i) => {
                const y = 108 + i * 74, sel = sameRoute(c, chosen), tested = testing && sameRoute(c, core.testRoute);
                cv.roundRect(664, y, 888, 66, 14, sel ? withAlpha(t.accent, 0.22) : withAlpha('#ffffff', 0.05));
                if (sel) cv.strokeRoundRect(664, y, 888, 66, 14, 2, t.accent);
                cv.text(c.name + (i === 0 ? ' (default)' : ''), 684, y + 30, 22, t.text, { font: 'bold' });
                cv.text(c.source + ' -> ' + c.sink + '   ' + c.device.replace(/^plug:smartx:/, ''), 684, y + 54, 15, t.dim);
                G.button(cv, t, 1220, y + 9, 140, 48, tested ? 'Listen...' : 'Test', testing ? null : () => { core.test(c); return redraw(); }, { key: 'mt-' + c.source, busy: tested });
                G.button(cv, t, 1372, y + 9, 168, 48, sel ? 'Chosen' : 'Use for mix', sel ? null : () => { audio.use(c); return redraw(); }, { key: 'mu-' + c.source, primary: sel });
            });
            if (list.length > MIX_ROWS) cv.text('+' + (list.length - MIX_ROWS) + ' more', 1552, 494, 15, t.dim, { align: 'right' });
            else if (list.length === 1 && !audio.loading) cv.text(audio.sources ? 'No other prompt-like source with a SmartX endpoint' : 'Only the default (no source list)', 664, 494, 15, t.dim);
        }

        async function render() {
            const t = ctx.theme(), cv = ctx.canvas(W, H).clear(t.bg);
            (page === 2 ? drawAudio : page === 1 ? drawSound : drawEngine)(cv, t);
            G.frame(cv, t, TABS, page, [core.d.demo && { text: 'DEMO' }]);
            dirty = false;
            await ui.image('MAIN_GFX', cv);
        }
        ctx.onTab((i) => { page = i; if (i === 2 && Date.now() - audio.at > 30000) audio.refresh(); return redraw(); });
        daemon.on('change', () => { dirty = true; });
        settings.onChange(() => { dirty = true; });
        let last = 0;
        ctx.loop('MAIN', 10, () => {
            const n = Date.now();
            // the live numbers: 10 times a second on the Engine page; the route's state twice a second on the Audio page
            if (dirty || (page === 0 && n - last > 100) || (page === 2 && n - last > 500)) { last = n; return render(); }
            return null;
        });
        render().catch((e) => log.warn('first render: ' + e));
    },
});


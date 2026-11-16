'use strict';
/**
 * Engine Sound - a synthesized petrol engine (inline-6, V8, V10, V12) on the car's speakers, following the car's
 * real rpm, gear and load.
 *
 * The app runs from the platform's start (app.json "autostart"): background() reads the car, drives the synthesizer
 * service (tool lfb-enginesound, see core.js / daemon.js) and the car's audio routing (route.js), with the app closed.
 *
 * The page: what plays and why (or why not), the car's rpm and the engine's, gear, load; the engine choice, volume,
 * on / off and a 10 s test sound. The sound goes to the main speakers as its own source (route.js): the music pauses
 * while it plays.
 */
const mgu = require('./kit/mgu');
const G = require('./kit/gauges');
const { withAlpha } = require('./kit/canvas');
const Car = require('./kit/car');
const { Core, PRESETS, ORDER, SOUND_ENGINES, SOUND_SETTINGS } = require('./core');
const { Daemon } = require('./daemon');
const { Route } = require('./route');

const W = 1600, H = 520;
const TABS = ['Engine', 'Sound'];
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

async function background(bg) {
    const { settings, signals, log } = bg;
    const daemon = new Daemon(log);
    const route = new Route(signals, log);
    const core = new Core({ settings, log, daemon, route });
    route.start();      // a connection a crash or a power-off left goes first

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
    return { core, car, daemon, route };
}

mgu.run({
    background,
    async start(ctx) {
        const { ui, settings, log } = ctx;
        const { core, daemon, route } = ctx.background || await background(ctx);
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
            const p = PRESETS[core.preset()], on = settings.get('enabled') !== false;
            const why = core.reason, d = core.d, o = core.out;
            // status
            G.panel(cv, t, 24, 44, 520, 468, 'Engine Sound');
            const col = !on ? t.dim : why ? t.warn : '#2fd27a';
            cv.roundRect(48, 104, 472, 64, 14, withAlpha(col, 0.18));
            cv.text(!on ? 'OFF' : why ? 'STANDBY' : 'PLAYING', 284, 148, 34, col, { align: 'center', font: 'bold', spacing: 2 });
            cv.text(on && why ? why : 'On the speakers (the music pauses)', 284, 196, 20, t.dim, { align: 'center' });
            const rs = route.state === 'failed' ? 'Audio route failed: ' + (route.error || '') : route.state === 'connecting' ? 'Audio route connecting...' : route.state === 'connected' ? 'Audio route connected' + (route.connState ? ' (' + route.connState + ')' : '') : 'Audio route idle';
            cv.text(rs.slice(0, 48), 284, 226, 18, route.state === 'failed' ? t.warn : t.dim, { align: 'center' });
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
                const q = PRESETS[k], sel = k === core.preset(), y = 96 + i * 76;
                cv.roundRect(1100, y, 452, 68, 14, sel ? withAlpha(t.accent, 0.3) : withAlpha('#ffffff', 0.05));
                if (sel) cv.strokeRoundRect(1100, y, 452, 68, 14, 2, t.accent);
                cv.text(q.title, 1124, y + 32, 26, t.text, { font: 'bold' });
                cv.text(q.sub + ' - ' + q.red + ' rpm', 1124, y + 56, 16, t.dim);
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
            cv.text(cur.sub + (PRESETS[core.preset()].turbo ? ' - turbo ' + (settings.get('turbo') !== false ? 'on' : 'off') : ''), 1100, 502, 15, t.dim);
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

        async function render() {
            const t = ctx.theme(), cv = ctx.canvas(W, H).clear(t.bg);
            (page === 1 ? drawSound : drawEngine)(cv, t);
            G.frame(cv, t, TABS, page, [core.d.demo && { text: 'DEMO' }]);
            dirty = false;
            await ui.image('MAIN_GFX', cv);
        }
        ctx.onTab((i) => { page = i; return redraw(); });
        daemon.on('change', () => { dirty = true; });
        settings.onChange(() => { dirty = true; });
        let last = 0;
        ctx.loop('MAIN', 10, () => {
            const n = Date.now();
            // the live numbers: 10 times a second on the Engine page
            if (dirty || (page === 0 && n - last > 100)) { last = n; return render(); }
            return null;
        });
        render().catch((e) => log.warn('first render: ' + e));
    },
});


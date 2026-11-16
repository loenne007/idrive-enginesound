'use strict';
/**
 * core.js - turns the car's data into the synthesizer's controls and decides when the sound plays.
 *
 *   rpm     the car's engine speed, mapped onto the chosen engine's rev range (setting rpmMap "scaled": the car's
 *           idle -> the engine's idle, the car's max rpm -> the engine's redline), or as it is ("real")
 *   load    the engine torque against the car's max torque: 0..1 pulling, negative on the overrun (engine braking)
 *   gear    from the gearbox; a change sends S <from> <to> (shift character in the synth)
 *
 * It plays while: switched on, the engine runs, and (setting sportOnly) the driving mode is Sport. Otherwise the
 * synth is muted (it fades) and, after a few seconds, the audio route is released so the car's own source returns.
 */
const PRESETS = {
    i6: { title: 'Inline-6', sub: '3.0 turbo, B58 / S58 style', idle: 700, red: 7000, turbo: true },
    v8: { title: 'V8', sub: 'cross-plane, M5 style', idle: 650, red: 7200, turbo: true },
    v10: { title: 'V10', sub: 'high-revving, naturally aspirated', idle: 900, red: 8250, turbo: false },
    v12: { title: 'V12', sub: 'smooth, naturally aspirated', idle: 700, red: 7500, turbo: false },
};
const ORDER = ['i6', 'v8', 'v10', 'v12'];

/* The default routes. mix: BMW's OAP overlay source, which the AudioManager mixes over the playing source (the way
 * a navigation prompt is) - to be confirmed on the car with the Audio page. exclusive: CarLifeMedia (China-only, free
 * elsewhere), the route Moonride uses on iDrive 7: it replaces the playing source while connected. */
const DEFAULT_ROUTES = {
    mix: { source: 1048, sink: 1024, device: 'plug:smartx:OAPGenericOverlay_CF_48_STEREO', name: 'OAPGenericOverlay' },
    exclusive: { source: 287, sink: 1024, device: 'plug:smartx:CarLifeMedia_CF_48_STEREO', name: 'CarLifeMedia', markAvailable: true },
};

const ENGINE_RUNS = 400;      // rpm: below = engine off
const RELEASE_MS = 4000;      // muted this long: the audio route goes back to the car
const SEND_MS = 20;           // P lines at 50 Hz
const STALE_MS = 1500;        // no rpm for this long: no P lines (the synth fades)
const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const num = (x) => typeof x === 'number' && isFinite(x);

class Core {
    /** opts = { settings, log, daemon, route, routes() -> { mix, exclusive }, now() } */
    constructor(opts) {
        this.settings = opts.settings; this.log = opts.log; this.daemon = opts.daemon; this.route = opts.route;
        this.routes = opts.routes || (() => DEFAULT_ROUTES);
        this.now = opts.now || Date.now;
        this.d = { rpm: null, rpmT: 0, speed: null, gear: undefined, torque: null, mode: null, condition: undefined, demo: false };
        this.idle = 800;          // the car's idle, learned at a standstill
        this.out = { rpm: 0, load: 0, throttle: 0, gear: 0 };
        this.playing = false; this.reason = 'starting'; this.mutedAt = this.now(); this.routeWant = null; this.lastGear = undefined;
        this.timer = null;
    }

    preset() { const e = this.settings.get('engine'); return PRESETS[e] ? e : 'i6'; }

    /* ---------- inputs ---------- */
    feed(d) {
        const t = this.now();
        if (num(d.rpm)) { this.d.rpm = d.rpm; this.d.rpmT = t; }
        if (num(d.speed)) this.d.speed = d.speed;
        if (d.torque !== undefined) this.d.torque = num(d.torque) ? d.torque : null;
        if (d.mode !== undefined) this.d.mode = d.mode;
        if (d.condition !== undefined) this.d.condition = d.condition;
        if (d.demo !== undefined) this.d.demo = d.demo;
        if (d.gear !== undefined && d.gear !== null) {
            const g = d.gear;
            if (this.lastGear !== undefined && g !== this.lastGear && g > 0 && this.lastGear > 0) this.daemon.send('S ' + this.lastGear + ' ' + g);
            this.lastGear = g; this.d.gear = g;
        }
        // the car's idle: rpm at a standstill, engine warm and settled (diesel 750-900)
        if (num(this.d.rpm) && this.d.speed !== null && this.d.speed < 1 && this.d.rpm > 550 && this.d.rpm < 1100 && (!num(this.d.torque) || this.d.torque < 60)) {
            this.idle += (this.d.rpm - this.idle) * 0.01;
        }
    }

    /* ---------- mapping ---------- */
    virtualRpm(real) {
        const p = PRESETS[this.preset()];
        if (this.settings.get('rpmMap') === 'real') return real;
        const top = Math.max(this.idle + 1000, Number(this.settings.get('realRedline')) || 4800);
        if (real <= this.idle) return real / this.idle * p.idle;
        const x = (real - this.idle) / (top - this.idle);
        return p.idle + clamp(x, 0, 1.1) * (p.red - p.idle);
    }

    load(tq) {
        if (!num(tq)) return 0.2;
        const max = Number(this.settings.get('maxTorque')) || 400;
        return tq >= 0 ? clamp(tq / max, 0, 1) : clamp(tq / 80, -1, 0);
    }

    /* ---------- when it plays ---------- */
    why() {
        const t = this.now();
        if (this.settings.get('enabled') === false) return 'off';
        if (!num(this.d.rpm) || t - this.d.rpmT > STALE_MS) return 'no engine data';
        if (this.d.rpm < ENGINE_RUNS) return 'engine off';
        if (this.settings.get('sportOnly') === true && this.d.mode !== 'sport') return 'waiting for Sport mode';
        return null;
    }

    start() {
        if (this.timer) return;
        this.daemon.send('M 1');
        this._applySettings();
        this.timer = setInterval(() => this.tick(), SEND_MS);
    }

    _applySettings() {
        const p = this.preset();
        this.daemon.send('E ' + p);
        this.daemon.send('T ' + (PRESETS[p].turbo && this.settings.get('turbo') !== false ? 1 : 0));
        this.daemon.send('V ' + (clamp(Number(this.settings.get('volume')) || 60, 0, 100) / 100).toFixed(2));
    }

    onSetting(key) {
        if (key === 'engine' || key === 'turbo' || key === 'volume') this._applySettings();
        if (key === 'mode' && this.playing) { this.routeWant = null; this._route(true); }
    }

    tick() {
        const why = this.why();
        const t = this.now();
        if (why !== this.reason) { this.log.info('sound: ' + (why || 'playing')); this.reason = why; }
        if (!why) {
            if (!this.playing) { this.playing = true; this._route(true); this.daemon.send('M 0'); }
            const rpm = this.virtualRpm(this.d.rpm), load = this.load(this.d.torque);
            // throttle: what the driver asks; the torque follows it closely in a turbo diesel
            const thr = clamp(load * 1.15, 0, 1);
            this.out = { rpm, load, throttle: thr, gear: this.d.gear === undefined ? 0 : this.d.gear };
            this.daemon.send('P ' + rpm.toFixed(1) + ' ' + load.toFixed(3) + ' ' + thr.toFixed(3) + ' ' + this.out.gear + ' ' + (num(this.d.speed) ? this.d.speed.toFixed(1) : '0'));
        } else {
            if (this.playing) { this.playing = false; this.mutedAt = t; this.daemon.send('M 1'); }
            if (this.routeWant && t - this.mutedAt > RELEASE_MS) this._route(false);
        }
    }

    _route(on) {
        if (!on) {
            this.routeWant = null;
            this.daemon.send('OUT none');
            if (this.route) this.route.disconnect();
            return;
        }
        const mode = this.settings.get('mode') === 'exclusive' ? 'exclusive' : 'mix';
        const r = this.routes()[mode] || DEFAULT_ROUTES[mode];
        if (this.routeWant && this.routeWant.device === r.device && this.routeWant.source === r.source) return;
        this.routeWant = r;
        this.daemon.send('OUT ' + r.device);
        if (this.route) this.route.connect(r).then((ok) => { if (!ok) this.log.warn('sound: route ' + r.name + ' failed: ' + this.route.error); });
    }
}

module.exports = { Core, PRESETS, ORDER, DEFAULT_ROUTES };

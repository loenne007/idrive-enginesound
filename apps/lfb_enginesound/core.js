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
 * The route is the CarLifeMedia source on the main speakers (route.js): it replaces the music while it plays.
 */
const PRESETS = {
    i6: { title: 'Inline-6', sub: '3.0 turbo, B58 / S58 style', idle: 700, red: 7000, turbo: true },
    v8: { title: 'V8', sub: 'cross-plane, M5 style', idle: 650, red: 7200, turbo: true },
    v10: { title: 'V10', sub: 'high-revving, naturally aspirated', idle: 900, red: 8250, turbo: false },
    v12: { title: 'V12', sub: 'smooth, naturally aspirated', idle: 700, red: 7500, turbo: false },
};
const ORDER = ['i6', 'v8', 'v10', 'v12'];
/* The sound engines (synth/<id>/ in the repository, es-synth-<id> in the service): each one plays all four presets;
 * the service runs the chosen one (ENGINE <id>). The first one is the default. */
const SOUND_ENGINES = [
    { id: 'pulse', title: 'Pulse', sub: 'light: pulses per cylinder, pipes, muffler' },
    { id: 'waveguide', title: 'Waveguide', sub: "Engine_Sim's pipes and resonators" },
    // Active Sound Design emulation: it adds rpm-locked low engine orders under the car's own diesel, so it gets the
    // REAL rpm, and its four presets are diesel characters (shown with these names)
    { id: 'bass', title: 'Bass', sub: 'Active Sound: bass under the real diesel', realRpm: true, presets: {
        i6: { title: 'Straight-6 diesel', sub: 'BMW B57, X7 40d style' },
        v8: { title: 'V8 TDI', sub: 'Audi SQ7 style' },
        v10: { title: 'V6 TDI', sub: 'Audi S6 / S7 style burble' },
        v12: { title: 'V6 TDI Sport', sub: 'stronger, dynamic calibration' } } },
];
const DEFAULT_SOUND_ENGINE = SOUND_ENGINES[0].id;
/* Sound engine ids saved by versions before 1.0 (setting soundEngine): a -> waveguide, b (removed) -> pulse,
 * c -> pulse, d -> bass. app.json's "aliases" maps them when the settings load; soundEngineId() does it again. */
const LEGACY_SOUND_ENGINES = { a: 'waveguide', b: 'pulse', c: 'pulse', d: 'bass' };
/** A stored soundEngine value as a current id (old ids mapped, unknown -> the default). */
function soundEngineId(v) {
    const id = Object.prototype.hasOwnProperty.call(LEGACY_SOUND_ENGINES, v) ? LEGACY_SOUND_ENGINES[v] : v;
    return SOUND_ENGINES.some((x) => x.id === id) ? id : DEFAULT_SOUND_ENGINE;
}
const soundEngineOf = (id) => SOUND_ENGINES.find((x) => x.id === soundEngineId(id));

const { DEVICE } = require('./route');

/* The sound settings (Sound page): app setting key -> the synth's X name (synth/PROTOCOL.md). Stored in percent,
 * 0..200, 100 = the engine's own default; sent as multipliers 0..2. */
const SOUND_SETTINGS = [
    { key: 'xOffgas', name: 'offgas', label: 'Off the gas', sub: 'idle, coasting, light load' },
    { key: 'xTurbo', name: 'turbo', label: 'Turbo', sub: 'whistle and spool' },
    { key: 'xBov', name: 'bov', label: 'Blow-off', sub: 'whoosh when lifting after boost' },
    { key: 'xPops', name: 'pops', label: 'Pops & bangs', sub: 'on the overrun' },
    { key: 'xBass', name: 'bass', label: 'Bass', sub: 'low end' },
];

const ENGINE_RUNS = 400;      // rpm: below = engine off
const RELEASE_MS = 4000;      // muted this long: the audio route goes back to the car
const SEND_MS = 20;           // P lines at 50 Hz
const STALE_MS = 1500;        // no rpm for this long: no P lines (the synth fades)
const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const num = (x) => typeof x === 'number' && isFinite(x);

class Core {
    /** opts = { settings, log, daemon, route, now() } */
    constructor(opts) {
        this.settings = opts.settings; this.log = opts.log; this.daemon = opts.daemon; this.route = opts.route;
        this.now = opts.now || Date.now;
        this.d = { rpm: null, rpmT: 0, speed: null, gear: undefined, torque: null, mode: null, condition: undefined, demo: false };
        this.idle = 800;          // the car's idle, learned at a standstill
        this.out = { rpm: 0, load: 0, throttle: 0, gear: 0 };
        this.playing = false; this.reason = 'starting'; this.mutedAt = this.now(); this.routed = false; this.lastGear = undefined;
        this.testUntil = 0; this.lastP = 0;
        this.timer = null;
    }

    preset() { const e = this.settings.get('engine'); return PRESETS[e] ? e : 'i6'; }

    /* ---------- inputs ---------- */
    feed(d) {
        const t = this.now();
        // a new rpm or torque goes to the synth at once (not only on the 50 Hz tick): a gear change sounds as quick
        // as the car sends it
        const fresh = num(d.rpm) || (d.torque !== undefined && num(d.torque));
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
        if (fresh && this.playing && t - this.lastP >= 8) this._sendP();
        // the car's idle: rpm at a standstill, engine warm and settled (diesel 750-900)
        if (num(this.d.rpm) && this.d.speed !== null && this.d.speed < 1 && this.d.rpm > 550 && this.d.rpm < 1100 && (!num(this.d.torque) || this.d.torque < 60)) {
            this.idle += (this.d.rpm - this.idle) * 0.01;
        }
    }

    /* ---------- mapping ---------- */
    virtualRpm(real) {
        const p = PRESETS[this.preset()];
        if (this.settings.get('rpmMap') === 'real' || soundEngineOf(this.soundEngine()).realRpm) return real;
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

    soundEngine() { return soundEngineId(this.settings.get('soundEngine')); }

    /** The preset's title / subtitle / redline as shown: the sound engine's own names where it has them (Bass). */
    presetInfo(k) {
        const p = PRESETS[k], own = soundEngineOf(this.soundEngine()).presets;
        return own && own[k] ? Object.assign({}, p, own[k], { red: null }) : p;
    }

    _applySettings() {
        const p = this.preset();
        this.daemon.send('ENGINE ' + this.soundEngine());
        this.daemon.send('E ' + p);
        // (the Bass engine's presets are all turbo diesels)
        const turbo = soundEngineOf(this.soundEngine()).realRpm || PRESETS[p].turbo;
        this.daemon.send('T ' + (turbo && this.settings.get('turbo') !== false ? 1 : 0));
        this.daemon.send('V ' + (clamp(Number(this.settings.get('volume')) || 60, 0, 100) / 100).toFixed(2));
        SOUND_SETTINGS.forEach((x) => this.daemon.send('X ' + x.name + ' ' + this.soundSetting(x.key).toFixed(2)));
    }

    /** A sound setting as the synth's multiplier, 0..2. */
    soundSetting(key) { const v = Number(this.settings.get(key)); return isFinite(v) && this.settings.get(key) !== undefined ? clamp(v / 100, 0, 2) : 1; }

    onSetting(key) {
        if (key === 'engine' || key === 'turbo' || key === 'volume' || key === 'soundEngine') this._applySettings();
        const x = SOUND_SETTINGS.find((s) => s.key === key);
        if (x) this.daemon.send('X ' + x.name + ' ' + this.soundSetting(key).toFixed(2));
    }

    tick() {
        const t = this.now();
        if (t < this.testUntil) return;                 // the test tone owns the route
        const why = this.why();
        if (why !== this.reason) { this.log.info('sound: ' + (why || 'playing')); this.reason = why; }
        if (!why) {
            if (!this.playing) { this.playing = true; this._route(true); this.daemon.send('M 0'); }
            if (t - this.lastP >= 15) this._sendP();
        } else {
            if (this.playing) { this.playing = false; this.mutedAt = t; this.daemon.send('M 1'); }
            if (this.routed && t - this.mutedAt > RELEASE_MS) this._route(false);
        }
    }

    _sendP() {
        const rpm = this.virtualRpm(this.d.rpm), load = this.load(this.d.torque);
        // throttle: what the driver asks; the torque follows it closely in a turbo diesel
        const thr = clamp(load * 1.15, 0, 1);
        this.out = { rpm, load, throttle: thr, gear: this.d.gear === undefined ? 0 : this.d.gear };
        this.lastP = this.now();
        this.daemon.send('P ' + rpm.toFixed(1) + ' ' + load.toFixed(3) + ' ' + thr.toFixed(3) + ' ' + this.out.gear + ' ' + (num(this.d.speed) ? this.d.speed.toFixed(1) : '0'));
    }

    _route(on) {
        if (on === this.routed) return;
        this.routed = on;
        if (on) {
            this.daemon.send('OUT ' + DEVICE);
            if (this.route) this.route.connect().then((ok) => { if (!ok) this.log.warn('sound: audio route failed: ' + this.route.error); });
        } else {
            this.daemon.send('OUT none');
            if (this.route) this.route.disconnect();
        }
    }

    /** Test: 10 s of beeps through the same route (also with the engine off). */
    test() {
        const t = this.now();
        if (t < this.testUntil) return;
        this.testUntil = t + 11000;
        this.log.info('sound: test tone');
        this.daemon.send('M 1');
        this.daemon.send('OUT ' + DEVICE);
        const go = () => this.daemon.send('TONE ' + DEVICE);
        if (this.route) { this.routed = true; this.route.connect().then(go); } else go();
        // afterwards: tick() decides again (playing or releasing the route)
        setTimeout(() => { this.playing = false; this.mutedAt = this.now() - RELEASE_MS; this.reason = 'after test'; }, 11000);
    }
}

module.exports = { Core, PRESETS, ORDER, SOUND_ENGINES, SOUND_SETTINGS, DEFAULT_SOUND_ENGINE, LEGACY_SOUND_ENGINES, soundEngineId, soundEngineOf };

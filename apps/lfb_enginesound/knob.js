'use strict';
/**
 * knob.js - the car's volume knob while the engine plays over the music (mix).
 *
 * What the car does (a G20, 2026-11): the AudioManager's main sink (1) lists the volume the knob moves. With nothing
 * but the music playing it is the music's (10 quiet, 53 loud). While our overlay source is connected the car hands the
 * knob to the overlay: the number is then the engine's volume in the car (22..55), and the music stays where it was.
 *
 * Setting knob = "music" (the default): the knob stays the music's, the engine's loudness is the app's.
 *   - the number changes while the engine plays (somebody turns the knob): the engine steps aside (core.hold: muted,
 *     its route released), so the car gives the knob back to the music; QUIET_MS after the last change it comes back
 *   - the steps the knob made before the engine was gone moved the engine's volume in the car: the synth's gain makes
 *     up for the difference to the reference (ref), DB_PER_STEP a step (an estimate), within TRIM_MIN..TRIM_MAX dB
 * Setting knob = "engine": as the car does it, the knob moves the engine while it plays; where it is left is the
 *   reference from then on.
 * For the speakers, in both: the engine's volume in the car counts for no more than CAP. Turned higher (55 is the
 * car's top, where the owner heard the bass strain), the synth turns down what is over, a dB a step, and the
 * reference is never above it.
 *
 * The number is only trusted once the connection is CS_CONNECTED and it stood still for SETTLE_MS (it switches from
 * the music's volume to the overlay's when the car connects). A change right after that, three times in a row, is not
 * a hand on the knob: the hand-over then rests for PAUSE_MS. Without a readable number nothing is held.
 */
const fs = require('fs');
const path = require('path');

const POLL_MS = 150;          // how often the number is read while the engine plays over the music
const STALE_MS = 3000;        // no number for this long: the knob is left to the car
const SETTLE_MS = 800;        // connected, read again and the number unchanged this long: it is the engine's volume
const QUIET_MS = 1500;        // the knob at rest this long: the engine comes back
const INSTANT_MS = 1500;      // a change this soon after the engine came back counts as "at once"
const PAUSE_MS = 300000;      // three of those in a row: no hand-over for 5 minutes
const DB_PER_STEP = 1;        // the car's volume steps in dB (an estimate)
const TRIM_MIN = -12, TRIM_MAX = 6;
const CAP = 48;               // the engine's volume in the car is never taken for more than this (speakers)
const EVENTS = 14;            // the last events kept for the report

const clamp = (x, a, b) => Math.max(a, Math.min(b, x));
const two = (n) => (n < 10 ? '0' : '') + n;

class Knob {
    /** opts = { core, settings, log, link(): the mix route is 'off', 'linked' (the connect is made) or 'connected'
     *  (the car lists it CS_CONNECTED), now(), dataDir } */
    constructor(opts) {
        this.core = opts.core; this.settings = opts.settings; this.log = opts.log;
        this.link = opts.link; this.now = opts.now || Date.now;
        this.file = opts.dataDir ? path.join(opts.dataDir, 'knob.json') : null;
        this.n = null; this.nAt = 0; this.changedAt = 0;      // the number, when it was read, when it last changed
        this.mode = 'off';            // off | blind (no number) | settling | watching | released (the engine stepped aside)
        this.base = null;             // the number while watching: another one is the knob
        this.wantedAt = 0; this.linkAt = 0; this.connAt = 0; this.watchAt = 0;
        this.music = null;            // the music's volume as last seen without the engine
        this.ref = null;              // the engine's volume in the car that the app's own volume is meant for
        this.trim = 0;                // dB the synth makes up for now
        this.handovers = 0; this.instant = 0; this.pausedUntil = 0;
        this.events = [];
        try { const s = JSON.parse(fs.readFileSync(this.file, 'utf8')); if (typeof s.ref === 'number' && isFinite(s.ref)) this.ref = s.ref; } catch (e) { /* none yet */ }
    }

    musicMode() { return this.settings.get('knob') !== 'engine'; }

    /** Should the number be read now (every POLL_MS)? While the engine plays over the music, or stepped aside. */
    wanted() { const c = this.core; return c.mixOn() && this.now() >= c.testUntil && !c.baseWhy(); }

    /** The main sink's volume as just read. */
    feed(n) {
        n = typeof n === 'string' && n.trim() !== '' ? Number(n) : n;
        if (typeof n !== 'number' || !isFinite(n)) return;
        const t = this.now();
        if (n !== this.n) this.changedAt = t;
        this.n = n; this.nAt = t;
    }

    _event(text) {
        const d = new Date(this.now());
        this.events.push(two(d.getHours()) + ':' + two(d.getMinutes()) + ':' + two(d.getSeconds()) + ' ' + text);
        if (this.events.length > EVENTS) this.events.shift();
        this.log.info('knob: ' + text);
    }
    _setRef(n) {
        n = Math.min(n, CAP);
        if (n === this.ref) return;
        this.ref = n;
        if (!this.file) return;
        try { fs.writeFileSync(this.file, JSON.stringify({ ref: n })); } catch (e) { this.log.warn('knob: ' + e); }
    }
    _trim(db) { this.trim = db; this.core.setTrim(db); }
    _off(mode) { if (this.core.held) this.core.hold(false); this.mode = mode; this.linkAt = this.connAt = 0; }
    /** The gain for the engine's volume n in the car: back to the reference (music mode), and never over the cap. */
    _trimFor(n, free) {
        const db = free ? 0 : clamp((this.ref - n) * DB_PER_STEP, TRIM_MIN, TRIM_MAX);
        this._trim(n > CAP ? Math.min(db, (CAP - n) * DB_PER_STEP) : db);
    }

    /** Every POLL_MS. */
    tick() {
        const t = this.now(), core = this.core;
        if (!this.wanted()) {
            // (the gain stays for the next time: the car keeps our source's volume; exclusive has none to make up for)
            this._off('off'); this.wantedAt = 0;
            if (!core.mixOn()) this._trim(0);
            return;
        }
        if (!this.wantedAt) this.wantedAt = t;
        if (this.n === null || t - this.nAt > STALE_MS) {
            if (t - this.wantedAt <= STALE_MS) { if (this.mode === 'off') this.mode = 'settling'; return; }     // (the reading has only just begun)
            if (this.mode !== 'blind') this._event('volume not readable: the knob is left to the car');
            this._off('blind');
            return;
        }
        if (this.mode === 'off' || this.mode === 'blind') this.mode = 'settling';
        const music = this.musicMode();
        if (this.mode === 'released') {
            this.music = this.n;
            if (music && t - this.changedAt < QUIET_MS) return;
            this._event('music ' + this.n + ': engine back');
            core.hold(false);
            this.mode = 'settling'; this.linkAt = this.connAt = 0;
            return;
        }
        const link = this.link();
        if (link === 'off') { this.mode = 'settling'; this.linkAt = this.connAt = 0; return; }
        if (!this.linkAt) this.linkAt = t;
        if (link !== 'connected') {
            // the engine is already heard: a number that left the music's since the connect is the engine's, so the
            // gain is right before the number is trusted for the knob
            if (music && this.ref !== null && this.music !== null && this.nAt > this.linkAt && this.n !== this.music) this._trimFor(this.n);
            this.mode = 'settling'; this.connAt = 0;
            return;
        }
        if (this.mode === 'settling') {
            if (!this.connAt) this.connAt = t;
            // (the number read again since the car connected, and at rest)
            if (this.nAt <= this.connAt || t - this.changedAt < SETTLE_MS) return;
            this.mode = 'watching'; this.base = this.n; this.watchAt = t;
            if (this.ref === null) this._setRef(this.n);
            this._event('engine on: ' + this.n + (this.music !== null ? ' (music ' + this.music + ')' : '') + ', ref ' + this.ref);
        }
        if (!music) {                                   // the knob is the engine's: where it is left is the reference
            this.base = this.n;
            this._setRef(this.n); this._trimFor(this.n, true);
            return;
        }
        if (this.n !== this.base) {
            const from = this.base;
            this.base = this.n;
            if (t >= this.pausedUntil) {
                this.instant = t - this.watchAt < INSTANT_MS ? this.instant + 1 : 0;
                if (this.instant >= 3) {
                    this.instant = 0; this.pausedUntil = t + PAUSE_MS;
                    this._event('moves by itself (' + from + ' -> ' + this.n + '): hand-over rests 5 min');
                } else {
                    this.handovers++;
                    this._event('knob ' + from + ' -> ' + this.n + ': engine aside');
                    this.mode = 'released'; this.changedAt = t;
                    core.hold(true);
                    return;
                }
            }
        }
        this._trimFor(this.n);
    }

    /** For the Audio page: one line. */
    text() {
        if (this.n === null) return null;
        const db = Math.round(this.trim);
        return 'Car volume ' + this.n + ', knob: ' + (this.musicMode() ? 'music' : 'engine') + (db ? ', engine ' + (db > 0 ? '+' : '') + db + ' dB' : '');
    }

    /** For the report: the state in a line, then the last events. */
    report() {
        return ['KNOB ' + (this.musicMode() ? 'music' : 'engine') + ', ' + this.mode + ', vol ' + (this.n === null ? '?' : this.n) + ', ref ' + (this.ref === null ? '?' : this.ref)
            + ', ' + this.trim.toFixed(1) + ' dB, ' + this.handovers + ' hand-overs' + (this.now() < this.pausedUntil ? ', resting' : '')]
            .concat(this.events.map((x) => '  ' + x));
    }
}

module.exports = { Knob, POLL_MS, QUIET_MS, SETTLE_MS, STALE_MS, PAUSE_MS, DB_PER_STEP, TRIM_MIN, TRIM_MAX, CAP };

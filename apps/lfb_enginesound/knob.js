'use strict';
/**
 * knob.js - the car's volume knob while the engine plays over the music (mix).
 *
 * What the car does with our overlay source (a G20, from its reports of 2026-11): it treats it as an announcement.
 *   - the music is lowered while the source is connected, and comes back to its own volume when it goes
 *   - the knob is the source's while it is connected: the AudioManager's main sink (1) then lists the source's volume,
 *     without it the music's; the music's volume cannot be moved while the source is connected
 *   - the source is never quieter than the music's volume: connected at 31 with the music at 39 it is 39 (max 55)
 *
 * Setting knob = "engine" (the default): as the car does it. The knob moves the engine while it plays; for the
 *   music's volume the engine sound is switched off for a moment.
 * Setting knob = "music": the number changes while the engine plays (somebody turns the knob): the engine steps aside
 *   (core.hold: muted, its route released), the car gives the knob back to the music, and QUIET_MS after the last
 *   change the engine comes back. The price is a pause of the engine and the music jumping to its own volume and
 *   back at every turn (the owner found that worse than the knob moving the engine).
 * For the speakers, in both: the source's volume counts for no more than CAP. Higher (55 is the car's top, where the
 * owner heard the bass strain), the synth turns down what is over, DB_PER_STEP (an estimate) a step.
 *
 * The number is only trusted once the connection is CS_CONNECTED and it stood still for SETTLE_MS (it switches from
 * the music's volume to the source's when the car connects). A change right after that, three times in a row, is not
 * a hand on the knob: the hand-over then rests for PAUSE_MS. Without a readable number nothing is held.
 */
const POLL_MS = 150;          // how often the number is read while the engine plays over the music
const STALE_MS = 3000;        // no number for this long: the knob is left to the car
const SETTLE_MS = 800;        // connected, read again and the number unchanged this long: it is the source's volume
const QUIET_MS = 1500;        // the knob at rest this long: the engine comes back (music mode)
const INSTANT_MS = 1500;      // a change this soon after the engine came back counts as "at once"
const PAUSE_MS = 300000;      // three of those in a row: no hand-over for 5 minutes
const DB_PER_STEP = 1;        // the car's volume steps in dB (an estimate)
const CAP = 48;               // the source's volume in the car is never taken for more than this (speakers)
const EVENTS = 14;            // the last events kept for the report

const two = (n) => (n < 10 ? '0' : '') + n;

class Knob {
    /** opts = { core, settings, log, link(): the mix route is 'off', 'linked' (the connect is made) or 'connected'
     *  (the car lists it CS_CONNECTED), now() } */
    constructor(opts) {
        this.core = opts.core; this.settings = opts.settings; this.log = opts.log;
        this.link = opts.link; this.now = opts.now || Date.now;
        this.n = null; this.nAt = 0; this.changedAt = 0;      // the number, when it was read, when it last changed
        this.mode = 'off';            // off | blind (no number) | settling | watching | released (the engine stepped aside)
        this.base = null;             // the number while watching: another one is the knob
        this.said = null;             // the source's volume the last event told
        this.wantedAt = 0; this.linkAt = 0; this.connAt = 0; this.watchAt = 0;
        this.music = null;            // the music's volume as last seen without the engine
        this.trim = 0;                // dB the synth turns down now (the cap)
        this.handovers = 0; this.instant = 0; this.pausedUntil = 0;
        this.events = [];
    }

    musicMode() { return this.settings.get('knob') === 'music'; }

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
    _trim(db) { this.trim = db; this.core.setTrim(db); }
    /** The gain for the source's volume n in the car: what is over the cap is turned down. */
    _cap(n) { this._trim(n > CAP ? (CAP - n) * DB_PER_STEP : 0); }
    _off(mode) { if (this.core.held) this.core.hold(false); this.mode = mode; this.linkAt = this.connAt = 0; }

    /** Every POLL_MS. */
    tick() {
        const t = this.now(), core = this.core;
        if (!this.wanted()) {
            // (the gain stays for the next time: the car keeps the source's volume; exclusive has none to turn down)
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
            // the engine is already heard: a number over the cap that came since the connect is the source's
            if (this.nAt > this.linkAt && this.n > CAP) this._cap(this.n);
            this.mode = 'settling'; this.connAt = 0;
            return;
        }
        if (this.mode === 'settling') {
            if (!this.connAt) this.connAt = t;
            // (the number read again since the car connected, and at rest)
            if (this.nAt <= this.connAt || t - this.changedAt < SETTLE_MS) return;
            this.mode = 'watching'; this.base = this.said = this.n; this.watchAt = t;
            this._event('engine on: ' + this.n + (this.music !== null ? ' (music ' + this.music + ')' : ''));
        }
        this._cap(this.n);
        if (this.n === this.base) {
            // the knob moved the engine and rests: said once
            if (this.n !== this.said && t - this.changedAt >= SETTLE_MS) { this._event('engine ' + this.said + ' -> ' + this.n); this.said = this.n; }
            return;
        }
        const from = this.base;
        this.base = this.n;
        if (!music || t < this.pausedUntil) return;       // the knob is the engine's
        this.instant = t - this.watchAt < INSTANT_MS ? this.instant + 1 : 0;
        if (this.instant >= 3) {
            this.instant = 0; this.pausedUntil = t + PAUSE_MS;
            this._event('moves by itself (' + from + ' -> ' + this.n + '): hand-over rests 5 min');
            return;
        }
        this.handovers++;
        this._event('knob ' + from + ' -> ' + this.n + ': engine aside');
        this.mode = 'released'; this.changedAt = t;
        core.hold(true);
    }

    /** For the Audio page: one line. */
    text() {
        if (this.n === null) return null;
        const db = Math.round(this.trim);
        return 'Car volume ' + this.n + ', knob: ' + (this.musicMode() ? 'music' : 'engine') + (db ? ', engine ' + db + ' dB' : '');
    }

    /** For the report: the state in a line, then the last events. */
    report() {
        return ['KNOB ' + (this.musicMode() ? 'music' : 'engine') + ', ' + this.mode + ', vol ' + (this.n === null ? '?' : this.n)
            + ', ' + this.trim.toFixed(1) + ' dB, ' + this.handovers + ' hand-overs' + (this.now() < this.pausedUntil ? ', resting' : '')]
            .concat(this.events.map((x) => '  ' + x));
    }
}

module.exports = { Knob, POLL_MS, QUIET_MS, SETTLE_MS, STALE_MS, PAUSE_MS, DB_PER_STEP, CAP };

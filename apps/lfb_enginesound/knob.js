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
 * Setting knob = "direct": the knob stays the music's without a pause. The car's AudioManager takes volumes from us
 *   (setVolume: the main sink's, which is our source's while it plays; setConnectionVolumeStep: one connection's,
 *   BMW's own addition). A turn of the knob moves our source's volume; the app puts it back where it was (the pin)
 *   and gives the steps to the music's connection. Only after the volume test (voltest.js) found both calls doing
 *   that in this car; nothing is passed on while anything but the music plays beside us (a call, a prompt), nor
 *   what the number does at a connect, and no more than UP_MAX steps up in UP_MS. A call that fails twice, or a
 *   volume the car does not take back: the knob is the engine's again (as "engine") until the app starts anew.
 * For the speakers, in all: the source's volume counts for no more than CAP. Higher (55 is the car's top, where the
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
const PIN_WAIT_MS = 3000;     // no number asked after a setVolume for this long: the call failed
const STEP_MAX = 10;          // steps passed on to the music at a time
const UP_MAX = 20, UP_MS = 2000;      // and no more than this many steps up within UP_MS
const TRIM_MIN = -15;         // dB the synth turns down at most while the car's number is over the pin

const two = (n) => (n < 10 ? '0' : '') + n;

class Knob {
    /** opts = { core, settings, log, link(): the mix route is 'off', 'linked' (the connect is made) or 'connected'
     *  (the car lists it CS_CONNECTED), now(), direct: { ready(): the pin (our source's volume to keep) once the
     *  volume test passed, else null; set(v): the main sink's volume, a promise of true; step(d): d steps to the
     *  music's connection, a promise of true, or null without music; clear(since): true when a list of the car's
     *  connections asked after `since` shows only the music beside us, false when anything else, null: not yet } } */
    constructor(opts) {
        this.core = opts.core; this.settings = opts.settings; this.log = opts.log;
        this.link = opts.link; this.now = opts.now || Date.now; this.direct = opts.direct || null;
        this.n = null; this.nAt = 0; this.changedAt = 0;      // the number, when it was read, when it last changed
        this.nAsked = 0;              // when the reading that brought it was asked for
        this.testing = false;         // the volume test owns the knob: the number is only read
        // direct: the pin of this connection, a number seen before the pin (nothing passed on), a setVolume on its way
        // (when, the number before it, was it the connection's first), what went to the music
        this.pin = null; this.pinned = false; this.pinAt = 0; this.was = null; this.firstPin = false; this.pinFails = 0;
        this.seenAt = 0; this.retryAt = 0; this.fwd = 0; this.turn = 0; this.turnAt = 0; this.ups = []; this.errors = 0; this.directOff = null;
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
    /** The pin when the knob is to stay the music's directly (the setting, the volume test passed, nothing failed). */
    directPin() {
        if (this.settings.get('knob') !== 'direct' || !this.direct || this.directOff) return null;
        const p = this.direct.ready();
        return typeof p === 'number' && isFinite(p) ? Math.min(p, CAP) : null;
    }
    /** A new start for direct (the setting was changed, the volume test passed again). */
    resetDirect() { this.directOff = null; this.errors = 0; this.pinFails = 0; this.pinned = false; this.pinAt = 0; }

    /** Should the number be read now (every POLL_MS)? While the engine plays over the music, or stepped aside. */
    wanted() { const c = this.core; return c.mixOn() && this.now() >= c.testUntil && !c.baseWhy(); }

    /** Is the number needed every POLL_MS (a turn of the knob is acted on, or the volume test runs)? */
    fast() { return this.testing || this.musicMode() || this.directPin() !== null; }

    /** The main sink's volume as just read; asked: when the reading was asked for (default: now). */
    feed(n, asked) {
        n = typeof n === 'string' && n.trim() !== '' ? Number(n) : n;
        if (typeof n !== 'number' || !isFinite(n)) return;
        const t = this.now();
        if (n !== this.n) this.changedAt = t;
        this.n = n; this.nAt = t; this.nAsked = typeof asked === 'number' ? asked : t;
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
            this.pin = null; this.pinned = false; this.pinAt = 0; this.pinFails = 0; this.seenAt = 0;
            this._event('engine on: ' + this.n + (this.music !== null ? ' (music ' + this.music + ')' : ''));
        }
        if (this.testing) { this._cap(this.n); this.base = this.said = this.n; return; }
        const pin = this.directPin();
        if (pin !== null) { this._direct(t, pin); return; }
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

    /* ---------- direct: the knob stays the music's, the engine's volume is put back ---------- */

    _fail(text) {
        if (++this.errors < 2 || this.directOff) return;
        this.directOff = text; this.pinAt = 0;
        this._event('direct off (' + text + '): the knob is the engine\'s');
    }
    _set(t) {
        this.was = this.n; this.pinAt = t;
        Promise.resolve(this.direct.set(this.pin)).then((ok) => { if (!ok) this._fail('setVolume refused'); }, (e) => this._fail('setVolume: ' + String(e && e.message || e).slice(0, 30)));
    }
    /** d steps of the knob to the music (limited); what went. */
    _forward(t, d) {
        d = Math.max(-STEP_MAX, Math.min(STEP_MAX, d));
        if (d > 0) {
            this.ups = this.ups.filter((x) => t - x.t < UP_MS);
            d = Math.min(d, UP_MAX - this.ups.reduce((a, x) => a + x.d, 0));
            if (d <= 0) return 0;
            this.ups.push({ t, d });
        }
        const p = this.direct.step(d);
        if (!p) return 0;         // no music plays: the knob has nothing to move
        Promise.resolve(p).then((ok) => { if (!ok) this._fail('music step refused'); }, (e) => this._fail('music step: ' + String(e && e.message || e).slice(0, 30)));
        this.fwd += d; this.turn += d; this.turnAt = t;
        return d;
    }
    _direct(t, P) {
        const n = this.n;
        this.base = this.said = n;
        // what the car's number is over the engine's level, the synth turns down until the number is back
        this._trim(n > P ? Math.max(TRIM_MIN, (P - n) * DB_PER_STEP) : 0);
        if (this.turn && t - this.turnAt >= SETTLE_MS) { this._event('music ' + (this.turn > 0 ? '+' : '') + this.turn + ', engine stays ' + this.pin); this.turn = 0; }
        if (this.pin === null) this.pin = P;
        if (this.pinAt) {
            // a setVolume is on its way: the first number asked for after it decides
            if (this.nAsked < this.pinAt) { if (t - this.pinAt > PIN_WAIT_MS) { this.pinAt = 0; this._fail('no volume after setVolume'); } return; }
            this.pinAt = 0;
            if (n !== this.pin && n === this.was) {
                // not taken. At a connect the car has its own mind (never under the music's volume): kept as it is
                if (this.firstPin) { this._event('the car keeps the engine at ' + n + ' (asked ' + this.pin + ')'); this.pin = n; return; }
                this.pinFails++; this.retryAt = t + POLL_MS;
            } else this.pinFails = 0;
        }
        if (n === this.pin) { this.pinned = true; this.seenAt = 0; this.pinFails = 0; return; }
        if (this.pinFails) {
            // the number as before the setVolume: not taken, or the knob went on by as much in that very moment.
            // Asked again, and nothing passed on; still so with the knob at rest: the car does not take it
            if (n === this.was) {
                if (this.pinFails >= 4 && t - this.changedAt >= 1000) { this.errors = 2; this.directOff = 'the car does not take the volume back'; this._event('direct off (' + this.directOff + ')'); return; }
                if (t >= this.retryAt) { this.firstPin = false; this._set(t); }
                return;
            }
            this.pinFails = 0;        // (it moves on: the knob)
        }
        // another number: the connect's own (nothing to pass on) or a hand on the knob. Only with nothing but the
        // music beside us
        if (!this.seenAt) this.seenAt = t;
        const clear = this.direct.clear(this.seenAt);
        if (clear === null) return;
        this.seenAt = 0;
        if (!clear) { this.pinned = false; return; }
        this.firstPin = !this.pinned;
        if (this.pinned) this._forward(t, n - this.pin);
        this.pinned = true;
        this._set(t);
    }

    /** For the Audio page: one line. */
    text() {
        if (this.n === null) return null;
        const db = Math.round(this.trim), set = this.settings.get('knob');
        const who = this.directPin() !== null ? 'music, direct' : set === 'direct' ? 'engine (' + (this.directOff ? 'direct failed' : 'volume test first') + ')' : this.musicMode() ? 'music' : 'engine';
        return 'Car volume ' + this.n + ', knob: ' + who + (db ? ', engine ' + db + ' dB' : '');
    }

    /** For the report: the state in a line, then the last events. */
    report() {
        const set = this.settings.get('knob'), direct = set === 'direct';
        return ['KNOB ' + (direct ? 'direct' : this.musicMode() ? 'music' : 'engine') + ', ' + this.mode + ', vol ' + (this.n === null ? '?' : this.n)
            + ', ' + this.trim.toFixed(1) + ' dB, ' + (direct ? (this.directPin() === null ? (this.directOff ? 'off: ' + this.directOff : 'not tested') : 'pin ' + (this.pin === null ? this.directPin() : this.pin)
                + ', music ' + (this.fwd >= 0 ? '+' : '') + this.fwd) : this.handovers + ' hand-overs') + (this.now() < this.pausedUntil ? ', resting' : '')]
            .concat(this.events.map((x) => '  ' + x));
    }
}

module.exports = { Knob, POLL_MS, QUIET_MS, SETTLE_MS, STALE_MS, PAUSE_MS, DB_PER_STEP, CAP, STEP_MAX, UP_MAX, UP_MS };

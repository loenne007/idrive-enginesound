'use strict';
/**
 * voltest.js - the volume test: does this car's AudioManager take volumes from the app?
 *
 * The car gives its volume knob to our source while the engine plays over the music (knob.js). Its AudioManager has
 * calls that could keep the knob the music's without a pause: setVolume (GENIVI: the main sink's volume, which is our
 * source's while it plays) and setConnectionVolumeStep (BMW's own: one connection's volume by steps; its arguments
 * are read from the car's interface definition, as their names are not published). What they do can only be found
 * out in the car, so the test tries them once, with the engine playing over the music, and only ever downwards
 * (2 steps quieter, then back to what was):
 *
 *   1. the engine steps aside: the music's volume E0 is read
 *   2. the engine comes back: our source's volume O0
 *   3. setVolume O0 - 2, read, setVolume O0, read: both taken = the engine's volume can be put back (the pin)
 *   4. getVolume (BMW's own, read only): its answer is noted
 *   5. setConnectionVolumeStep -2 on the music's connection; our own volume must not move
 *   6. the engine steps aside again: the music at E0 - 2 = the steps reach the music. It is put back to E0.
 * Without 3 the test ends there (nothing could be put back); had the call set the music's volume, that is put back.
 *
 * Passed (3 and 6): knob.js may run "direct". The lines of the last test are kept for the Audio report.
 */
const SETTLE_MS = 800;

/* ---------- a method's definition and its arguments ---------- */

/** The object that defines `name` somewhere in `o` (a key of that name, or an entry { name }). */
function findDef(o, name, depth) {
    depth = depth || 0;
    if (!o || typeof o !== 'object' || depth > 6) return null;
    if (Array.isArray(o)) {
        const hit = o.find((x) => x && typeof x === 'object' && x.name === name);
        if (hit) return hit;
        for (const x of o) { const r = findDef(x, name, depth + 1); if (r) return r; }
        return null;
    }
    if (o[name] && typeof o[name] === 'object') return o[name];
    for (const k of Object.keys(o)) { const r = findDef(o[k], name, depth + 1); if (r) return r; }
    return null;
}

const IN = /^(in|ins|inputs?|in_?args?|in_?arguments|in_?params?|in_?parameters|params?|parameters|args|arguments|request)$/i;
/** The names of a method definition's input arguments, in order; null when the definition does not show them. */
function inputNames(def) {
    if (!def || typeof def !== 'object') return null;
    const k = Object.keys(def).find((x) => IN.test(x));
    if (!k) return null;
    const v = def[k];
    if (Array.isArray(v)) {
        const n = v.map((e) => (typeof e === 'string' ? e : e && e.name)).filter((x) => typeof x === 'string' && x);
        return n.length && n.length === v.length ? n : null;
    }
    const n = v && typeof v === 'object' ? Object.keys(v) : [];
    return n.length ? n : null;
}

/* the names GENIVI's interface gives them (setConnectionVolumeStep and getVolume: a guess, only without a definition) */
const FALLBACK = { setVolume: ['sinkID', 'volume'], getVolume: ['sinkID'], setConnectionVolumeStep: ['mainConnectionID', 'volumeStep'] };

/** The argument struct for input names: each name gets the sink, the connection, the source or the value it means.
 *  { args } or { unknown: the name that means none of them }. */
function argsFor(names, ctx) {
    const args = {};
    for (const k of names || []) {
        const v = /step|percent/i.test(k) ? ctx.value : /conn/i.test(k) ? ctx.conn : /sink/i.test(k) ? ctx.sink
            : /source/i.test(k) ? ctx.source : /vol|value/i.test(k) ? ctx.value : undefined;
        if (v === undefined || v === null) return { unknown: k };
        args[k] = v;
    }
    return { args };
}

const short = (x, n) => (x === undefined ? '-' : JSON.stringify(x).replace(/"/g, '')).slice(0, n || 56);
const okReply = (r) => !!r && (r.amError === 'E_OK' || r.amError === 0);
const two = (n) => (n < 10 ? '0' : '') + n;

/** The definitions of the volume methods as report lines (so a photo shows what the car wants). */
function defLines(defs, width) {
    const L = [];
    Object.keys(defs).forEach((m) => {
        const d = defs[m], text = m + ' ' + (d ? short(d, 240) : 'no definition found');
        for (let i = 0; i < text.length && i < width * 4; i += width) L.push('  ' + text.slice(i, i + width));
    });
    return L;
}

class VolTest {
    /** o = { core, knob, link(), am(method, args): a promise of the reply, music(): { id, source } of the music's
     *  connection or null, clear(): only the music plays beside us, defs: { method: definition }, sink: the main
     *  sink's id, saved: the last result, done(result), sleep(ms), now(), log } */
    constructor(o) {
        this.o = o; this.running = false;
        this.result = o.saved || null;
        this.now = o.now || Date.now;
        this.sleep = o.sleep || ((ms) => new Promise((res) => setTimeout(res, ms)));
        this.names = {};
        Object.keys(FALLBACK).forEach((m) => { this.names[m] = inputNames((o.defs || {})[m]) || FALLBACK[m]; });
    }

    lines() { return this.result && this.result.lines ? this.result.lines : ['VOLUME TEST not run yet (Audio page, with the engine over music)']; }

    /** The number at rest with the route in state `link` (asked for after it got there); null after `timeout`. */
    async _rest(link, timeout) {
        const end = this.now() + timeout, k = this.o.knob;
        let since = 0;
        while (this.now() < end) {
            await this.sleep(100);
            if (this.o.link() !== link) { since = 0; continue; }
            if (!since) since = this.now();
            if (k.n !== null && k.nAsked > since && this.now() - k.changedAt >= SETTLE_MS && this.now() - k.nAt < 1500) return k.n;
        }
        return null;
    }
    /** The number as asked for at least `ms` from now. */
    async _read(ms) {
        const at = this.now() + (ms || 400), end = at + 2500, k = this.o.knob;
        while (this.now() < end) { await this.sleep(50); if (k.nAsked >= at) return k.n; }
        return null;
    }
    async _call(method, ctx) {
        const a = argsFor(this.names[method], ctx);
        if (!a.args) return { error: 'argument ' + a.unknown + ' not understood' };
        try { return { args: a.args, reply: await this.o.am(method, a.args) }; } catch (e) { return { args: a.args, error: String(e && e.message || e).slice(0, 44) }; }
    }

    run() {
        if (this.running) return Promise.resolve(this.result);
        this.running = true;
        const o = this.o, L = [], out = { ok: false, pin: null, names: this.names };
        return this._run(L, out).catch((e) => { L.push('  stopped: ' + String(e && e.message || e).slice(0, 50)); }).then(() => {
            o.knob.testing = false;
            if (o.core.held) o.core.hold(false);
            const d = new Date(this.now());
            out.lines = ['VOLUME TEST ' + two(d.getHours()) + ':' + two(d.getMinutes()) + (out.ok ? ' passed: the knob can stay the music\'s' : ' not passed')].concat(L);
            out.lines.forEach((x) => o.log.info('voltest: ' + x));
            this.result = out; this.running = false;
            o.done(out);
            return out;
        });
    }

    async _run(L, out) {
        const o = this.o, sink = o.sink;
        const say = (x) => { L.push('  ' + x); };
        if (o.knob.mode !== 'watching' || o.link() !== 'connected') return say('the engine must play over the music (now: ' + o.knob.mode + ')');
        let music = o.music();
        if (!music || !o.clear()) return say(music ? 'something else plays beside the music: later again' : 'no music plays: start the music, then again');
        o.knob.testing = true;

        // 1, 2: the music's volume without the engine, our source's with it
        o.core.hold(true);
        const E0 = await this._rest('off', 6000);
        o.core.hold(false);
        if (E0 === null) return say('the music\'s volume was not readable');
        const O0 = await this._rest('connected', 9000);
        if (O0 === null) return say('music ' + E0 + ', but the engine did not come back in time');
        say('music ' + E0 + ', engine ' + O0);
        if (O0 < 4) return say('the engine\'s volume is too low for the test: turn it up a little');

        // 3: the main sink's volume, down 2 and (only when our volume followed) back
        let r = await this._call('setVolume', { sink, value: O0 - 2 });
        const O1 = r.error ? null : await this._read();
        let O2 = null, pin = false;
        if (!r.error && O1 === O0 - 2) {
            const b = await this._call('setVolume', { sink, value: O0 });
            O2 = b.error ? null : await this._read();
            pin = O2 === O0;
        }
        say('setVolume ' + short(r.args, 30) + ': ' + (r.error || short(r.reply, 22) + ', car ' + O1 + (O2 === null ? '' : ', back ' + O2)) + (pin ? ' ok' : ''));
        if (!pin) {
            // (nothing could be put back, and the engine's volume could not be kept: no further.) Did it set the
            // music's volume instead? Then that is put back, with the music alone
            if (!r.error && O1 === O0) {
                o.core.hold(true);
                const E = await this._rest('off', 6000);
                if (E !== null && E !== E0) {
                    await this._call('setVolume', { sink, value: E0 });
                    say('it set the music instead: ' + E0 + ' -> ' + E + ', put back: ' + await this._read());
                }
                o.core.hold(false);
            }
            return say('setVolume does not set the engine\'s volume here');
        }

        // 4: BMW's getVolume (read only)
        r = await this._call('getVolume', { sink, conn: music.id, source: music.source });
        say('getVolume ' + short(r.args, 26) + ' -> ' + (r.error || short(r.reply, 30)));

        // 5: 2 steps down on the music's connection; our own volume stays
        music = o.music();
        if (!music) return say('the music stopped: again with music');
        r = await this._call('setConnectionVolumeStep', { sink, conn: music.id, source: music.source, value: -2 });
        say('setConnectionVolumeStep ' + short(r.args, 40));
        const O3 = await this._read();
        say('  -> ' + (r.error || short(r.reply, 24)) + ', engine ' + O3);
        let own = true;
        if (O3 !== null && O2 !== null && O3 !== O2) {
            own = false;
            say('  it moved the engine, not the music: put back');
            await this._call('setVolume', { sink, value: O0 });
        }

        // 6: the music's volume again
        o.core.hold(true);
        const E1 = await this._rest('off', 6000);
        let E2 = E1;
        if (E1 !== null && E1 !== E0) {
            // (the music is on top now: the main sink's volume is its)
            await this._call('setVolume', { sink, value: E0 });
            E2 = await this._read();
        }
        o.core.hold(false);
        const step = !r.error && own && E1 === E0 - 2;
        say('music after: ' + E1 + (E1 !== E0 ? ', put back: ' + E2 : '') + (step ? ' ok' : E1 === E0 ? ' (the steps did not reach it)' : ''));
        out.ok = pin && step;
        if (out.ok) out.pin = O0;
        return null;
    }
}

module.exports = { VolTest, findDef, inputNames, argsFor, defLines, FALLBACK };

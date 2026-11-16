'use strict';
/**
 * route.js - the car's audio routing (GENIVI AudioManager over SOME/IP).
 *
 * A route = { name, source, sink, device, markAvailable }: our PCM goes into the SmartX ALSA endpoint `device`, and
 * the AudioManager source `source` is connected to the sink (1024: the main speakers). Two kinds:
 *
 *   exclusive  CarLifeMedia (287) -> 1024, plug:smartx:CarLifeMedia_CF_48_STEREO, exactly as Moonride
 *              (JustAnotherPU/moonride) does it on iDrive 7. CarLifeMedia is China-only, so free elsewhere; it is
 *              A_UNAVAILABLE without a CarLife phone and a connect to it stays CS_CONNECTING for ever, so it is
 *              marked available on the routing side while we play and unavailable again after. It replaces the
 *              music (the music pauses).
 *   mix        an announcement-like source (default OAPGenericOverlay, 1048) -> 1024, so the engine plays over the
 *              music as a navigation prompt does. Its device is the SmartX endpoint named after the source (found in
 *              the service's /dev/shm list), else plug:smartx:OAPGenericOverlay_CF_48_STEREO. BMW's own OAP sources
 *              are never marked available.
 *
 *   start(leftover):  at the app's start, what a crash or a power-off left goes: every 287 connection (only ours can
 *                     exist), 287 marked unavailable, and the mix connection we had open (`leftover`, the state file)
 *   connect(route):   (mark available) + connect; a route that fails leaves nothing behind
 *   disconnect():     disconnect our connection (+ the 287 sweep and unavailable for the exclusive route)
 *   sources():        the AudioManager's main sources [{ id, name }] (getListMainSources), for the Audio page
 * One AudioManager call at a time, in order.
 */
const fs = require('fs');
const path = require('path');

const AM = 'org.genivi.am.commandinterface.CommandControl';
const AMR = 'org.genivi.am.routinginterface.RoutingControlObserver';
const SINK = 1024;   // the main speakers
const EXCLUSIVE = Object.freeze({ kind: 'exclusive', name: 'CarLifeMedia', source: 287, sink: SINK,
    device: 'plug:smartx:CarLifeMedia_CF_48_STEREO', markAvailable: true });
const MIX_DEFAULT = Object.freeze({ kind: 'mix', name: 'OAPGenericOverlay', source: 1048, sink: SINK,
    device: 'plug:smartx:OAPGenericOverlay_CF_48_STEREO', markAvailable: false });
// AudioManager sources that sound like a prompt over the music (mix candidates)
const MIX_WORDS = /overlay|interrupt|navi|tts|announcement|speech|prompt|chime/i;

/* ---------------- choosing a route (pure, tested in test/core.test.js) ---------------- */

/**
 * The SmartX endpoint (from the service's SHM list, e.g. "OAPGenericOverlay_CF_48_STEREO") whose name starts with the
 * AudioManager source's name, case-insensitive, as an ALSA device ("plug:smartx:<endpoint>"); null if none. Best:
 * "<name>_CF_48_STEREO", then "<name>_..." (48 kHz, stereo preferred), then any other that starts with the name.
 */
function deviceFor(name, endpoints) {
    if (!name || !Array.isArray(endpoints)) return null;
    const n = String(name).toLowerCase();
    const score = (e) => {
        const l = String(e).toLowerCase();
        if (l.indexOf(n) !== 0) return -1;
        if (l === n + '_cf_48_stereo') return 100;
        return (l[n.length] === '_' ? 50 : 0) + (/_48_/.test(l) ? 10 : 0) + (/stereo/.test(l) ? 5 : 0);
    };
    const best = endpoints.map((e) => [score(e), e]).filter((x) => x[0] >= 0).sort((a, b) => b[0] - a[0])[0];
    return best ? 'plug:smartx:' + best[1] : null;
}

/**
 * The mix candidates for the Audio page: the default first (always, with its endpoint if found), then every
 * AudioManager source whose name sounds like a prompt (MIX_WORDS) and that has a SmartX endpoint.
 * sources = [{ id, name }] or null (list unavailable); endpoints = [names] or null.
 */
function mixCandidates(sources, endpoints) {
    const out = [Object.assign({}, MIX_DEFAULT, { device: deviceFor(MIX_DEFAULT.name, endpoints) || MIX_DEFAULT.device })];
    (sources || []).forEach((s) => {
        if (!s || typeof s.id !== 'number' || !MIX_WORDS.test(s.name || '') || out.some((c) => c.source === s.id)) return;
        const device = deviceFor(s.name, endpoints);
        if (device) out.push({ kind: 'mix', name: String(s.name), source: s.id, sink: SINK, device, markAvailable: false });
    });
    return out;
}

/**
 * The mix route: the saved choice ({ name, source, sink, device } or null = the default). With the SmartX list known
 * the device is looked up there (a choice whose endpoint is gone falls back to the default); without it the saved
 * device is used. `note` says why when it is not what was asked for.
 */
function mixRoute(choice, endpoints) {
    const known = Array.isArray(endpoints);
    if (choice && typeof choice.source === 'number' && choice.source !== MIX_DEFAULT.source && choice.name) {
        const device = known ? deviceFor(choice.name, endpoints) : choice.device;
        if (device) return Object.assign({}, MIX_DEFAULT, { name: String(choice.name), source: choice.source, sink: choice.sink || SINK, device });
        const r = mixRoute(null, endpoints);
        r.note = 'no SmartX endpoint for ' + choice.name + ': default used';
        return r;
    }
    const device = known ? deviceFor(MIX_DEFAULT.name, endpoints) : null;
    return Object.assign({}, MIX_DEFAULT, { device: device || MIX_DEFAULT.device },
        known && !device ? { note: 'no SmartX endpoint ' + MIX_DEFAULT.name + ': assumed ' + MIX_DEFAULT.device } : {});
}

/** The route to play on: mix (setting "Play over music") or exclusive. */
function routeFor(mix, choice, endpoints) { return mix ? mixRoute(choice, endpoints) : Object.assign({}, EXCLUSIVE); }

const sameRoute = (a, b) => !!a && !!b && a.source === b.source && a.sink === b.sink;
const label = (r) => r.name + ' (' + r.source + ' -> ' + r.sink + ')';

/* ---------------- the AudioManager ---------------- */

const okReply = (r) => r && (r.amError === 'E_OK' || r.amError === 'E_ALREADY_EXISTS' || r.amError === 0);
const arrayIn = (r, key) => (r && Array.isArray(r[key]) ? r[key] : Array.isArray(r) ? r
    : (r && Object.keys(r).map((k) => r[k]).find(Array.isArray)) || []);
const listOf = (r) => (r && Array.isArray(r.listMainConnections) ? r.listMainConnections : arrayIn(r, 'listConnections'));
const STATES = { 0: 'CS_UNKNOWN', 1: 'CS_CONNECTING', 2: 'CS_CONNECTED', 3: 'CS_DISCONNECTING', 4: 'CS_DISCONNECTED', 5: 'CS_SUSPENDED' };
const stateName = (s) => STATES[s] || String(s);
const CHECK_MS = 1500;        // the connection's state is looked at this long after a connect
const MIX_SETTLE_MS = 6000;   // a mix connection not CS_CONNECTED by then is released (route failed)

class Route {
    /** dataDir: where our open mix connection is noted (route-state.json), so a start after a crash can remove it. */
    constructor(signals, log, dataDir) {
        this.signals = signals; this.log = log;
        this.file = dataDir ? path.join(dataDir, 'route-state.json') : null;
        this.p = null; this.r = null;
        this.route = null;           // the route connected (or connecting)
        this.id = null;              // its mainConnectionID (null: none known / not ours)
        this.marked = false;         // the route's source is marked available by us
        this.failed = null;          // the route that failed last (state failed)
        this.state = 'off';          // off | connecting | connected | failed
        this.error = null; this.connState = null;
        this.q = Promise.resolve();
        this.shape = false;
        this.gen = 0;                // bumped on every connect / disconnect (late checks of an old connect do nothing)
        this.checkMs = CHECK_MS; this.settleMs = MIX_SETTLE_MS;
    }

    _queue(fn) { const p = this.q.then(fn, fn); this.q = p.catch(() => {}); return p; }

    // a fresh proxy answers "Service unavailable" until the provider is found: wait for it (max 5 s)
    async _proxy(iface) {
        const p = await this.signals.proxy(iface, '1');
        await new Promise((res) => {
            const t = setTimeout(res, 5000);
            this.signals.onAvailable(p, (a) => { if (a) { clearTimeout(t); res(); } });
        });
        return p;
    }
    async _init() { if (!this.p) this.p = await this._proxy(AM); }
    _call(m, a, ms) { return this.signals.call(this.p, m, a || {}, ms || 5000); }

    /** Mark a source (un)available on the routing side; errors are logged, not thrown (as Moonride). */
    async _avail(source, on) {
        try {
            if (!this.r) this.r = await this._proxy(AMR);
            await this.signals.call(this.r, 'hookSourceAvailablityStatusChange',
                { sourceID: source, availability: { availability: on ? 'A_AVAILABLE' : 'A_UNAVAILABLE', availabilityReason: 0 } }, 5000);
            this.log.info('audio: source ' + source + (on ? ' available' : ' unavailable'));
        } catch (e) { this.log.warn('audio availability ' + source + ': ' + e); }
        this.marked = on;
    }

    async _connections() {
        const r = await this._call('getListMainConnections', {});
        if (!this.shape) { this.shape = true; this.log.info('audio: connections ' + JSON.stringify(r).slice(0, 800)); }
        return listOf(r);
    }

    async _disconnectId(id, why) {
        const r = await this._call('disconnect', { mainConnectionID: id });
        this.log.info('audio: ' + why + ' ' + id + ' disconnect ' + JSON.stringify(r));
    }

    /** Disconnect connections of `source` (-> `sink` if given; only `id` if given). */
    async _sweep(source, sink, id) {
        const list = (await this._connections()).filter((x) => x && x.sourceID === source && (sink === undefined || x.sinkID === sink)
            && (id === undefined || id === null || x.mainConnectionID === id));
        for (const c of list) await this._disconnectId(c.mainConnectionID, 'leftover');
    }

    /* the mix connection we hold, noted on disk until it is gone */
    _note(o) {
        if (!this.file) return;
        try { if (o) fs.writeFileSync(this.file, JSON.stringify(o)); else if (fs.existsSync(this.file)) fs.unlinkSync(this.file); } catch (e) { this.log.warn('audio: ' + e); }
    }
    _noted() { try { return this.file ? JSON.parse(fs.readFileSync(this.file, 'utf8')) : null; } catch (e) { return null; } }

    start() {
        return this._queue(async () => {
            try {
                await this._init();
                // the exclusive source: every connection is ours (CarLifeMedia is China-only)
                await this._sweep(EXCLUSIVE.source).catch((e) => this.log.warn('audio sweep: ' + e));
                await this._avail(EXCLUSIVE.source, false);
                // the mix source: only the connection we had open (a crash or a power-off while it played)
                const n = this._noted();
                if (n && typeof n.source === 'number') {
                    await this._sweep(n.source, n.sink, n.id).catch((e) => this.log.warn('audio sweep: ' + e));
                    this._note(null);
                }
            } catch (e) { this.log.warn('audio start: ' + e); }
        });
    }

    /** Connect a route (any other one goes first). Resolves true when connected (or connecting), false when it failed. */
    connect(route) {
        return this._queue(async () => {
            if (sameRoute(route, this.route) && (this.state === 'connected' || this.state === 'connecting')) return true;
            if (this.route) await this._release();
            const gen = ++this.gen;
            this.route = route; this.failed = null; this.state = 'connecting'; this.error = null; this.connState = null; this.id = null;
            try {
                await this._init();
                if (route.markAvailable) await this._avail(route.source, true);
                const r = await this._call('connect', { sourceID: route.source, sinkID: route.sink });
                this.log.info('audio: connect ' + label(route) + ' ' + JSON.stringify(r));
                if (!okReply(r)) throw new Error('connect refused (' + (r && r.amError) + ')');
                // E_ALREADY_EXISTS: a connection that is not ours; we play into it and leave it alone
                this.id = r.amError !== 'E_ALREADY_EXISTS' && r.mainConnectionID !== undefined && r.mainConnectionID !== null ? r.mainConnectionID : null;
                if (route.kind === 'mix' && r.amError !== 'E_ALREADY_EXISTS') this._note({ source: route.source, sink: route.sink, id: this.id });
                this.state = 'connected';
                this._check(gen, this.checkMs);
                return true;
            } catch (e) {
                this.error = String(e && e.message || e);
                this.log.warn('audio: ' + label(route) + ' failed: ' + this.error);
                await this._release();
                this.state = 'failed'; this.failed = route;
                return false;
            }
        });
    }

    /** After a connect: the connection's state as the AudioManager lists it. A mix route still not CS_CONNECTED after
     *  settleMs (6 s) failed: it is released (shown as failed), so no pending connection is left behind. */
    _check(gen, ms, since) {
        since = since || 0;
        setTimeout(() => this._queue(async () => {
            if (gen !== this.gen || this.state !== 'connected') return;
            const route = this.route;
            const c = (await this._connections()).find((x) => x && x.sourceID === route.source && x.sinkID === route.sink);
            this.connState = c ? stateName(c.connectionState) : 'no connection';
            this.log.info('audio: ' + label(route) + ' ' + ms + ' ms after connect: ' + this.connState);
            if (route.kind !== 'mix' || this.connState === 'CS_CONNECTED') return;
            if (ms < this.settleMs) return this._check(gen, this.settleMs, ms);
            this.error = 'stays ' + this.connState;
            this.log.warn('audio: mix route ' + label(route) + ' ' + this.error + ': released');
            await this._release();
            this.state = 'failed'; this.failed = route;
        }).catch((e) => this.log.warn('audio check: ' + e)), ms - since);
    }

    /** Remove what the current route left: our connection, the exclusive source's sweep, the availability mark. */
    async _release() {
        const route = this.route, id = this.id;
        this.gen++;
        this.route = null; this.id = null; this.connState = null;
        if (!route) return;
        try {
            await this._init();
            if (id !== null) await this._disconnectId(id, 'our');
            if (route.kind === 'exclusive') await this._sweep(route.source);
        } catch (e) { this.log.warn('audio disconnect: ' + e); }
        if (route.kind === 'exclusive' || this.marked) await this._avail(route.source, false);
        if (route.kind === 'mix') this._note(null);
    }

    disconnect() {
        return this._queue(async () => {
            if (this.state === 'off') return;
            await this._release();
            this.state = 'off'; this.error = null; this.failed = null;
        });
    }

    /** The AudioManager's main sources, [{ id, name }]; throws when the list is unavailable. */
    /** Everything the AudioManager lists, for the Audio page's report: { sources, sinks, connections, errors }. */
    lists() {
        return this._queue(async () => {
            await this._init();
            const out = { sources: [], sinks: [], connections: [], errors: [] };
            const get = async (method, key, listKey, map) => {
                try { out[key] = arrayIn(await this._call(method, {}, 3000), listKey).map(map); } catch (e) { out.errors.push(method + ': ' + (e && e.message || e)); }
            };
            await get('getListMainSources', 'sources', 'listMainSources', (x) => ({ id: x.sourceID, name: String(x.name || '') }));
            await get('getListMainSinks', 'sinks', 'listMainSinks', (x) => ({ id: x.sinkID, name: String(x.name || '') }));
            await get('getListMainConnections', 'connections', 'listConnections',
                (x) => ({ id: x.mainConnectionID, source: x.sourceID, sink: x.sinkID, state: stateName(x.connectionState) }));
            return out;
        });
    }

    sources() {
        return this._queue(async () => {
            await this._init();
            const r = await this._call('getListMainSources', {}, 3000);
            return arrayIn(r, 'listMainSources').map((s) => ({ id: s.sourceID, name: String(s.name || '') }))
                .filter((s) => typeof s.id === 'number');
        });
    }
}

module.exports = { Route, EXCLUSIVE, MIX_DEFAULT, SINK, deviceFor, mixCandidates, mixRoute, routeFor, sameRoute };

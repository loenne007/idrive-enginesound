'use strict';
/**
 * route.js - the car's audio routing (GENIVI AudioManager over SOME/IP), as Moonride (JustAnotherPU/moonride) and
 * RideTube do it: a source is connected to a sink with CommandControl.connect, and a SmartX ALSA endpoint named after
 * the source carries our PCM. A source the AudioManager thinks unavailable (CarLifeMedia without a CarLife phone) only
 * reaches CS_CONNECTED once it is marked available on the routing side (RoutingControlObserver).
 *
 *   const r = new Route(signals, log);
 *   await r.connect({ source: 1048, sink: 1024 });   // -> true / false
 *   await r.disconnect();
 *   await r.lists();                                 // { sources, sinks, connections } for the Audio page
 */
const AM = 'org.genivi.am.commandinterface.CommandControl';
const AMR = 'org.genivi.am.routinginterface.RoutingControlObserver';

const arrayIn = (r, key) => (r && Array.isArray(r[key]) ? r[key] : Array.isArray(r) ? r : (r && Object.keys(r).map((k) => r[k]).find(Array.isArray)) || []);
const okReply = (r) => r && (r.amError === 'E_OK' || r.amError === 0 || r.amError === 'E_ALREADY_EXISTS');
const stateName = (s) => ({ 0: 'CS_UNKNOWN', 1: 'CS_CONNECTING', 2: 'CS_CONNECTED', 3: 'CS_DISCONNECTING', 4: 'CS_DISCONNECTED', 5: 'CS_SUSPENDED' }[s] || s);

class Route {
    constructor(signals, log) {
        this.signals = signals; this.log = log;
        this.p = null; this.r = null;
        this.conn = null;            // { id, source, sink }
        this.state = 'off';          // off | connecting | connected | failed
        this.error = null;
        this.q = Promise.resolve();  // one AudioManager call chain at a time
        this.swept = {};
    }

    _queue(fn) { const p = this.q.then(fn, fn); this.q = p.catch(() => {}); return p; }

    async _proxy(iface) {
        const p = await this.signals.proxy(iface, '1');
        await new Promise((res) => {
            const t = setTimeout(res, 5000);
            this.signals.onAvailable(p, (a) => { if (a) { clearTimeout(t); res(); } });
        });
        return p;
    }

    async _init() {
        if (!this.p) this.p = await this._proxy(AM);
        if (!this.r) { try { this.r = await this._proxy(AMR); } catch (e) { this.log.warn('audio: no RoutingControlObserver: ' + e); } }
    }

    _call(method, params, ms) { return this.signals.call(this.p, method, params || {}, ms || 5000); }

    async _connections() {
        const r = await this._call('getListMainConnections', {});
        return arrayIn(r, 'listConnections').concat(r && Array.isArray(r.listMainConnections) ? r.listMainConnections : []);
    }

    async _avail(source, on) {
        if (!this.r) return;
        try {
            await this.signals.call(this.r, 'hookSourceAvailablityStatusChange',
                { sourceID: source, availability: { availability: on ? 'A_AVAILABLE' : 'A_UNAVAILABLE', availabilityReason: 0 } });
            this.log.info('audio: source ' + source + (on ? ' marked available' : ' marked unavailable'));
        } catch (e) { this.log.warn('audio availability ' + source + ': ' + e); }
    }

    /** Disconnect every connection of this source (left by a crash, a power-off or a timed-out connect). */
    async _sweep(source) {
        const list = await this._connections();
        for (const c of list.filter((x) => x && x.sourceID === source)) {
            const r = await this._call('disconnect', { mainConnectionID: c.mainConnectionID });
            this.log.info('audio: leftover ' + c.mainConnectionID + ' ' + JSON.stringify(r));
        }
    }

    async _stateOf(source) {
        const c = (await this._connections()).find((x) => x && x.sourceID === source);
        return c ? stateName(c.connectionState) : null;
    }

    /** route = { source, sink, markAvailable }: connect. Resolves true once connected (or connecting). */
    connect(route) {
        return this._queue(async () => {
            if (this.conn && this.conn.source === route.source && this.conn.sink === route.sink && this.state === 'connected') return true;
            this.state = 'connecting'; this.error = null;
            try {
                await this._init();
                if (this.conn) await this._disconnectNow();
                if (!this.swept[route.source]) { this.swept[route.source] = true; await this._sweep(route.source).catch((e) => this.log.warn('audio sweep: ' + e)); }
                if (route.markAvailable) await this._avail(route.source, true);
                let r = await this._call('connect', { sourceID: route.source, sinkID: route.sink });
                this.log.info('audio: connect ' + route.source + ' -> ' + route.sink + ' ' + JSON.stringify(r));
                if (!okReply(r)) { this.state = 'failed'; this.error = 'connect refused (' + (r && r.amError) + ')'; return false; }
                this.conn = { id: r.mainConnectionID, source: route.source, sink: route.sink, marked: !!route.markAvailable };
                // a source the AudioManager holds unavailable stays CS_CONNECTING: mark it available and connect again
                await new Promise((res) => setTimeout(res, 1500));
                let st = await this._stateOf(route.source).catch(() => null);
                if (st === 'CS_CONNECTING' && !route.markAvailable && this.r) {
                    this.log.info('audio: ' + route.source + ' stays connecting: marking it available');
                    await this._disconnectNow();
                    await this._avail(route.source, true);
                    r = await this._call('connect', { sourceID: route.source, sinkID: route.sink });
                    this.log.info('audio: connect again ' + JSON.stringify(r));
                    if (!okReply(r)) { this.state = 'failed'; this.error = 'connect refused (' + (r && r.amError) + ')'; return false; }
                    this.conn = { id: r.mainConnectionID, source: route.source, sink: route.sink, marked: true };
                    await new Promise((res) => setTimeout(res, 1500));
                    st = await this._stateOf(route.source).catch(() => null);
                }
                this.log.info('audio: ' + route.source + ' ' + (st || 'no connection listed'));
                this.state = st === 'CS_CONNECTING' ? 'connecting' : 'connected';
                return true;
            } catch (e) {
                this.state = 'failed'; this.error = String(e && e.message || e);
                this.log.warn('audio connect: ' + this.error);
                return false;
            }
        });
    }

    async _disconnectNow() {
        const c = this.conn;
        this.conn = null;
        if (!c) return;
        if (c.id !== undefined && c.id !== null) {
            const r = await this._call('disconnect', { mainConnectionID: c.id }).catch((e) => String(e));
            this.log.info('audio: disconnect ' + c.id + ' ' + JSON.stringify(r));
        } else await this._sweep(c.source).catch(() => {});
        if (c.marked) await this._avail(c.source, false);
    }

    disconnect() {
        return this._queue(async () => {
            try { await this._init(); await this._disconnectNow(); } catch (e) { this.log.warn('audio disconnect: ' + e); }
            this.state = 'off';
        });
    }

    /** Sources, sinks and connections, for the Audio page. */
    lists() {
        return this._queue(async () => {
            await this._init();
            const out = { sources: [], sinks: [], connections: [], errors: [] };
            const get = async (m, key, map) => {
                try { const r = await this._call(m, {}, 3000); out[key] = arrayIn(r, { sources: 'listMainSources', sinks: 'listMainSinks', connections: 'listConnections' }[key]).map(map); } catch (e) { out.errors.push(m + ': ' + e); }
            };
            await get('getListMainSources', 'sources', (s) => ({ id: s.sourceID, name: String(s.name || '') }));
            await get('getListMainSinks', 'sinks', (s) => ({ id: s.sinkID, name: String(s.name || '') }));
            await get('getListMainConnections', 'connections', (c) => ({ id: c.mainConnectionID, source: c.sourceID, sink: c.sinkID, state: stateName(c.connectionState) }));
            return out;
        });
    }
}

module.exports = { Route, stateName };

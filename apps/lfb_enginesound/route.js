'use strict';
/**
 * route.js - the car's audio routing (GENIVI AudioManager over SOME/IP), exactly as Moonride
 * (JustAnotherPU/moonride, apps/jap_moonride/main.js) does it on iDrive 7:
 *
 *   our PCM goes into the SmartX ALSA endpoint CarLifeMedia_CF_48_STEREO; the AudioManager source CarLifeMedia (287,
 *   China only, so free elsewhere) is connected to the main speakers (sink 1024). 287 is A_UNAVAILABLE without a
 *   CarLife phone, and a connect to it stays CS_CONNECTING for ever, so it is marked available on the routing side
 *   while we play and unavailable again after.
 *
 *   start():      at the app's start, what a crash or a power-off left (a 287 connection, the source marked
 *                 available) goes
 *   connect():    available + connect 287 -> 1024
 *   disconnect(): disconnect + unavailable
 * One call at a time, in order.
 */
const AM = 'org.genivi.am.commandinterface.CommandControl';
const AMR = 'org.genivi.am.routinginterface.RoutingControlObserver';
const SOURCE = 287, SINK = 1024;
const DEVICE = 'plug:smartx:CarLifeMedia_CF_48_STEREO';

const okReply = (r) => r && (r.amError === 'E_OK' || r.amError === 'E_ALREADY_EXISTS' || r.amError === 0);
const listOf = (r) => (r && Array.isArray(r.listConnections) ? r.listConnections : r && Array.isArray(r.listMainConnections) ? r.listMainConnections
    : Array.isArray(r) ? r : (r && Object.keys(r).map((k) => r[k]).find(Array.isArray)) || []);

class Route {
    constructor(signals, log) {
        this.signals = signals; this.log = log;
        this.p = null; this.r = null; this.id = null;
        this.state = 'off';          // off | connecting | connected | failed
        this.error = null; this.connState = null;
        this.q = Promise.resolve();
        this.shape = false;
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
    async _init() {
        if (!this.p) this.p = await this._proxy(AM);
        if (!this.r) this.r = await this._proxy(AMR);
    }
    _call(m, a, ms) { return this.signals.call(this.p, m, a || {}, ms || 5000); }

    _avail(on) {
        return this.signals.call(this.r, 'hookSourceAvailablityStatusChange',
            { sourceID: SOURCE, availability: { availability: on ? 'A_AVAILABLE' : 'A_UNAVAILABLE', availabilityReason: 0 } })
            .then(() => this.log.info('audio: source ' + SOURCE + (on ? ' available' : ' unavailable')), (e) => this.log.warn('audio availability: ' + e));
    }

    async _connections() {
        const r = await this._call('getListMainConnections', {});
        if (!this.shape) { this.shape = true; this.log.info('audio: connections ' + JSON.stringify(r).slice(0, 800)); }
        return listOf(r);
    }

    /** Disconnect every connection of our source (left by a crash, a power-off or a timed-out connect). */
    async _sweep() {
        for (const c of (await this._connections()).filter((x) => x && x.sourceID === SOURCE)) {
            const r = await this._call('disconnect', { mainConnectionID: c.mainConnectionID });
            this.log.info('audio: leftover ' + c.mainConnectionID + ' disconnect ' + JSON.stringify(r));
        }
    }

    start() {
        return this._queue(async () => {
            try {
                await this._init();
                await this._sweep().catch((e) => this.log.warn('audio sweep: ' + e));
                await this._avail(false);
            } catch (e) { this.log.warn('audio start: ' + e); }
        });
    }

    connect() {
        return this._queue(async () => {
            if (this.state === 'connected' || this.state === 'connecting') return true;
            this.state = 'connecting'; this.error = null; this.connState = null;
            try {
                await this._init();
                await this._avail(true);
                const r = await this._call('connect', { sourceID: SOURCE, sinkID: SINK });
                this.log.info('audio: connect ' + JSON.stringify(r));
                if (!okReply(r)) {
                    this.state = 'failed'; this.error = 'connect refused (' + (r && r.amError) + ')';
                    await this._avail(false);
                    return false;
                }
                this.id = r.mainConnectionID !== undefined && r.mainConnectionID !== null ? r.mainConnectionID : null;
                this.state = 'connected';
                setTimeout(() => this._queue(async () => {
                    const c = (await this._connections()).find((x) => x && x.sourceID === SOURCE);
                    this.connState = c ? String(c.connectionState) : 'no connection';
                    this.log.info('audio: 1.5 s after connect: ' + this.connState);
                }).catch(() => {}), 1500);
                return true;
            } catch (e) {
                this.state = 'failed'; this.error = String(e && e.message || e);
                this.log.warn('audio connect: ' + this.error);
                return false;
            }
        });
    }

    disconnect() {
        return this._queue(async () => {
            if (this.state === 'off') return;
            try {
                await this._init();
                if (this.id !== null) {
                    const r = await this._call('disconnect', { mainConnectionID: this.id });
                    this.log.info('audio: disconnect ' + this.id + ' ' + JSON.stringify(r));
                    this.id = null;
                }
                await this._sweep().catch((e) => this.log.warn('audio sweep: ' + e));
                await this._avail(false);
            } catch (e) { this.log.warn('audio disconnect: ' + e); }
            this.state = 'off'; this.connState = null;
        });
    }
}

module.exports = { Route, SOURCE, SINK, DEVICE };

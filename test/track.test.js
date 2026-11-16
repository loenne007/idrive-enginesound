'use strict';
// node test/track.test.js: the rpm estimate between the car's samples (apps/lfb_enginesound/track.js) against a
// hold of the last sample, on a rev (800 -> 4000 rpm in 0.7 s and back), samples every 50-100 ms with jitter.
const assert = require('assert');
const { Track } = require('../apps/lfb_enginesound/track');

let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
// the true engine speed: idle, a rev up, a hold, the fall back, idle (an engine's inertia rounds the corners)
const ease = (x) => (x <= 0 ? 0 : x >= 1 ? 1 : 0.5 - 0.5 * Math.cos(Math.PI * x));
const truth = (t) => (t < 500 ? 800 : t < 1200 ? 800 + 3200 * ease((t - 500) / 700) : t < 1700 ? 4000 : t < 2900 ? 4000 - 3200 * ease((t - 1700) / 1200) : 800);

function run(interval) {
    const tr = new Track();
    let next = 0, held = null, sumT = 0, sumH = 0, maxT = 0, maxH = 0, n = 0, maxStep = 0, last = null, lagT = 0, lagH = 0, m = 0;
    for (let t = 0; t <= 3400; t += 10) {
        if (t >= next) { const v = Math.round(truth(t) * 4) / 4; tr.feed(v, t); held = v; next = t + interval * (0.8 + 0.4 * rnd()); }
        const e = tr.at(t), tru = truth(t);
        const eT = Math.abs(e - tru), eH = Math.abs(held - tru);
        sumT += eT * eT; sumH += eH * eH; maxT = Math.max(maxT, eT); maxH = Math.max(maxH, eH); n++;
        if (last !== null) maxStep = Math.max(maxStep, Math.abs(e - last));
        last = e;
        if (t > 600 && t < 1150) { lagT += tru - e; lagH += tru - held; m++; }   // on the climb: how far behind
    }
    return { rmsT: Math.sqrt(sumT / n), rmsH: Math.sqrt(sumH / n), maxT, maxH, maxStep, lagT: lagT / m, lagH: lagH / m, stats: tr.stats() };
}

[60, 100].forEach((interval) => {
    const r = run(interval);
    console.log(`samples every ~${interval} ms: error rms ${r.rmsT.toFixed(0)} rpm (hold ${r.rmsH.toFixed(0)}), max ${r.maxT.toFixed(0)} (hold ${r.maxH.toFixed(0)}), ` +
        `behind on the climb ${r.lagT.toFixed(0)} rpm (hold ${r.lagH.toFixed(0)}), largest 10 ms step ${r.maxStep.toFixed(0)} rpm, measured ${r.stats.perSecond.toFixed(1)}/s`);
    assert(r.rmsT < r.rmsH * 0.5, 'half the error of a hold');
    assert(r.maxT < r.maxH * 0.6, 'no overshoot worse than the hold\'s lag');
    assert(Math.abs(r.lagT) < Math.abs(r.lagH) * 0.25, 'hardly behind on a climb');
    assert(Math.abs(r.stats.interval - interval) < interval * 0.2, 'the sample interval measured');
});
// steady: the reading itself; a glitch or a step (gear change) is not extrapolated; nothing before the first reading
{
    const tr = new Track();
    assert.strictEqual(tr.at(0), null);
    tr.feed(2000, 0); tr.feed(2000.25, 60); tr.feed(2000, 120);
    assert(Math.abs(tr.at(150) - 2000) < 1);
    tr.feed(3500, 180);                       // 25000 rpm/s: a step
    assert.strictEqual(tr.at(230), 3500);
    // after a climb that stops (no new sample: the car only sends changes), it eases back to the reading
    const c = new Track();
    [0, 60, 120, 180].forEach((t, i) => c.feed(2000 + i * 200, t));
    assert(c.at(240) > 2700, 'moves on');
    assert.strictEqual(c.at(180 + 72 + 200 + 1), 2600, 'back at the reading');
    // repeats of the same value (another signal's tick) change nothing
    const before = c.at(200); c.feed(2600, 190); assert.strictEqual(c.at(200), before);
}
console.log('track.test.js: ok');

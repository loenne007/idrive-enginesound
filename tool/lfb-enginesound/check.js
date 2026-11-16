'use strict';
// Store health check after an install: the synth binary runs on this unit (its --info answers).
const { execFileSync } = require('child_process');
const path = require('path');
try {
    const out = execFileSync(path.join(__dirname, 'es-synth'), ['--info'], { timeout: 10000 }).toString();
    JSON.parse(out);
    console.log('es-synth ok');
} catch (e) {
    console.error('es-synth --info failed: ' + (e && e.message || e));
    process.exit(1);
}

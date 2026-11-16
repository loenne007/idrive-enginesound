// node replay.mjs out.wav seconds volume dump1.txt [out2.wav seconds volume dump2.txt ...]
// Renders C-port param dumps through the real Engine_Sim graph in headless Chrome (DevTools protocol, no deps).
import { spawn } from 'node:child_process';
import { writeFileSync, readFileSync, mkdtempSync, rmSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
// ENGINE_SIM: a checkout of github.com/YigitSalihEmecen/Engine_Sim; replay.html is copied into its tools/
const ROOT = process.env.ENGINE_SIM || join(HERE, 'es');
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const PORT = 8600 + Math.floor(Math.random() * 300);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const jobs = [];
const a = process.argv.slice(2);
for (let i = 0; i + 3 < a.length + 0 && i < a.length; i += 4) jobs.push({ out: a[i], sec: +a[i + 1], vol: +a[i + 2], dump: a[i + 3] });

copyFileSync(join(HERE, 'replay.html'), join(ROOT, 'tools/replay.html'));
const server = spawn(process.execPath, [join(ROOT, 'tools/serve.mjs'), String(PORT)], { stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 'replay-chrome-'));
const chrome = spawn(CHROME, ['--headless=new', '--no-sandbox', `--remote-debugging-port=${PORT + 1}`,
  `--user-data-dir=${profile}`, 'about:blank'], { stdio: 'ignore' });
try {
  let tabs;
  for (let i = 0; i < 80 && !tabs; i++) {
    try { tabs = await (await fetch(`http://127.0.0.1:${PORT + 1}/json`)).json(); } catch { await sleep(200); }
  }
  const ws = new WebSocket(tabs.find((t) => t.type === 'page').webSocketDebuggerUrl);
  await new Promise((r) => { ws.onopen = r; });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
    if (d.method === 'Runtime.consoleAPICalled') console.log('[page]', d.params.args.map((x) => x.value).join(' '));
    if (d.method === 'Runtime.exceptionThrown') console.log('[page error]', JSON.stringify(d.params.exceptionDetails).slice(0, 400));
  };
  const call = (method, params = {}) => new Promise((r) => {
    const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params }));
  });
  const evaluate = async (expression) => {
    const r = await call('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, timeout: 900000 });
    if (r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails).slice(0, 800));
    return r.result?.result?.value;
  };
  await call('Runtime.enable');
  await call('Page.navigate', { url: `http://localhost:${PORT}/tools/replay.html` });
  for (let i = 0; i < 100 && !(await evaluate('window.ready === true')); i++) await sleep(200);
  for (const j of jobs) {
    const t0 = Date.now();
    const text = readFileSync(j.dump, 'utf8');
    const r = await evaluate(`window.replay(${JSON.stringify(text)}, ${j.sec}, ${j.vol})`);
    writeFileSync(j.out, Buffer.from(r.wav, 'base64'));
    console.log(j.out, ((Date.now() - t0) / 1000).toFixed(1) + 's');
  }
  ws.close();
} finally {
  server.kill();
  await new Promise((r) => { chrome.once('exit', r); chrome.kill(); });
  rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
}

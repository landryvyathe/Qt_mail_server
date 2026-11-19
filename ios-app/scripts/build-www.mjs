// Copies the webmail into www/ for the iPhone app and points it at the live server.
import { cpSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const SERVER = 'https://mail.bykiva.com';
const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const src = join(root, '..', 'webmail');
const out = join(root, 'www');

rmSync(out, { recursive: true, force: true });
cpSync(src, out, { recursive: true });

writeFileSync(join(out, 'config.js'), `globalThis.BYKIVA_API_BASE = ${JSON.stringify(SERVER)};\n`);

const indexPath = join(out, 'index.html');
const html = readFileSync(indexPath, 'utf8')
  .replace("connect-src 'self'", `connect-src 'self' ${SERVER}`)
  .replace('<script type="module"', '<script src="config.js"></script>\n  <script type="module"');
if (!html.includes('config.js') || !html.includes(`connect-src 'self' ${SERVER}`))
  throw new Error('index.html layout changed: could not inject the iPhone app config');
writeFileSync(indexPath, html);

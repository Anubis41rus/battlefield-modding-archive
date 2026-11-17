// build-vuic-ru.js — патч ui.vuic мода fun-bots: русский язык меню F12 (01.11.2026).
// Запуск: node build-vuic-ru.js <ui.vuic мода> <ru_RU.js> <выход .evrp>
//
// ui.vuic (VU): "CIUV" | u32 2 | u32 n | n × {size, stored, aux, key, offset} | данные zstd.
// key = XXH32(путь без "/" в нижнем регистре, 0x333D06C1) (evbots-dll Vuic.cpp), aux = XXH32(распакованное, 0x333D06C1)
// (проверено 01.11 на всех 45 записях ui.vuic fun-bots). Языковые модули мода грузятся только из редактора путей, поэтому
// перевод подключается обычным <script> в index.html до модуля страницы: он задаёт window.Language.ru_RU.
//
// Выход .evrp (его применяет EV Launcher, ModPatch.cs, без zstd у себя):
//   "EVRP" | u32 1 | u32 auxИсходногоIndex | u32 число записей | записи {u32 key, u32 aux, u32 size, u32 stored, байты}
// Первая запись заменяет index.html, остальные добавляются.
const fs = require('fs'), zlib = require('zlib');
const SEED = 0x333D06C1;
const P1 = 2654435761, P2 = 2246822519, P3 = 3266489917, P4 = 668265263, P5 = 374761393;
const mul = (a, b) => Math.imul(a, b) >>> 0;
const rotl = (x, r) => ((x << r) | (x >>> (32 - r))) >>> 0;
function xxh32(buf, seed) {
  let i = 0, h; const len = buf.length, u32 = (o) => buf.readUInt32LE(o);
  if (len >= 16) {
    let v1 = (seed + P1 + P2) >>> 0, v2 = (seed + P2) >>> 0, v3 = seed >>> 0, v4 = (seed - P1) >>> 0;
    do {
      v1 = mul(rotl((v1 + mul(u32(i), P2)) >>> 0, 13), P1); i += 4;
      v2 = mul(rotl((v2 + mul(u32(i), P2)) >>> 0, 13), P1); i += 4;
      v3 = mul(rotl((v3 + mul(u32(i), P2)) >>> 0, 13), P1); i += 4;
      v4 = mul(rotl((v4 + mul(u32(i), P2)) >>> 0, 13), P1); i += 4;
    } while (i <= len - 16);
    h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) >>> 0;
  } else h = (seed + P5) >>> 0;
  h = (h + len) >>> 0;
  while (i + 4 <= len) { h = mul(rotl((h + mul(u32(i), P3)) >>> 0, 17), P4); i += 4; }
  while (i < len) { h = mul(rotl((h + mul(buf[i], P5)) >>> 0, 11), P1); i++; }
  h ^= h >>> 15; h = mul(h, P2); h ^= h >>> 13; h = mul(h, P3); h ^= h >>> 16;
  return h >>> 0;
}
const key = (p) => xxh32(Buffer.from(p.toLowerCase(), 'utf8'), SEED);

const [, , vuicPath, ruPath, outPath] = process.argv;
const d = fs.readFileSync(vuicPath);
if (d.toString('latin1', 0, 4) !== 'CIUV' || d.readUInt32LE(4) !== 2) throw new Error('не CIUV v2');
const n = d.readUInt32LE(8);
const SCRIPT = 'assets/ru_RU-evbots.js';
let index = null;
for (let k = 0; k < n; k++) {
  const o = 12 + k * 20;
  const e = { size: d.readUInt32LE(o), stored: d.readUInt32LE(o + 4), aux: d.readUInt32LE(o + 8), key: d.readUInt32LE(o + 12), off: d.readUInt32LE(o + 16) };
  if (e.key === key('index.html')) index = e;
  if (e.key === key(SCRIPT)) throw new Error('ui.vuic уже с русским — нужен исходный');
}
if (!index) throw new Error('нет index.html');
const html = zlib.zstdDecompressSync(d.subarray(index.off, index.off + index.stored)).toString('utf8');
const at = html.indexOf('<script type="module"');
if (at < 0) throw new Error('в index.html нет <script type="module">');
const patched = Buffer.from(html.slice(0, at) + '<script src="/' + SCRIPT + '"></script>\n  ' + html.slice(at), 'utf8');
const ru = fs.readFileSync(ruPath);
const entries = [[key('index.html'), patched], [key(SCRIPT), ru]].map(([k, raw]) => {
  const c = zlib.zstdCompressSync(raw);
  if (!zlib.zstdDecompressSync(c).equals(raw)) throw new Error('zstd: круг не сошёлся');
  const h = Buffer.alloc(16);
  h.writeUInt32LE(k, 0); h.writeUInt32LE(xxh32(raw, SEED), 4); h.writeUInt32LE(raw.length, 8); h.writeUInt32LE(c.length, 12);
  return Buffer.concat([h, c]);
});
const head = Buffer.alloc(16);
head.write('EVRP', 0, 'latin1'); head.writeUInt32LE(1, 4); head.writeUInt32LE(index.aux, 8); head.writeUInt32LE(entries.length, 12);
fs.writeFileSync(outPath, Buffer.concat([head, ...entries]));
console.log(`ok: index.html aux ${index.aux.toString(16)} -> ${xxh32(patched, SEED).toString(16)}, ${SCRIPT} key ${key(SCRIPT).toString(16)}, ${outPath}`);

// make_icon.js — 生成 RLPlayer 的程序图标（PNG 多尺寸 + .ico）
// 纯 Node 实现，不依赖任何图形库：自己画像素、自己压 PNG、自己封 ICO。
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const OUT_DIR = process.argv[2] || path.join(__dirname, '..', 'res');
fs.mkdirSync(OUT_DIR, { recursive: true });

// ---- 画一张 size x size 的 RGBA ----
function draw(size) {
  const S = size;
  const px = Buffer.alloc(S * S * 4, 0);
  const cx = (S - 1) / 2, cy = (S - 1) / 2;
  const R = S * 0.22;               // 圆角半径
  const pad = S * 0.045;
  const half = S / 2 - pad;

  // 圆角矩形的有向距离（<0 在内部）
  function sdRoundRect(x, y) {
    const qx = Math.abs(x - cx) - (half - R);
    const qy = Math.abs(y - cy) - (half - R);
    const ax = Math.max(qx, 0), ay = Math.max(qy, 0);
    return Math.sqrt(ax * ax + ay * ay) + Math.min(Math.max(qx, qy), 0) - R;
  }
  const smooth = (e0, e1, x) => {
    const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0)));
    return t * t * (3 - 2 * t);
  };

  for (let y = 0; y < S; ++y) {
    for (let x = 0; x < S; ++x) {
      const i = (y * S + x) * 4;
      const d = sdRoundRect(x + 0.5, y + 0.5);
      const cover = 1 - smooth(-0.9, 0.9, d);          // 抗锯齿覆盖
      if (cover <= 0) continue;

      // 对角渐变：左上 #3b82f6 -> 右下 #8b5cf6
      const t = Math.min(1, Math.max(0, ((x + y) / (2 * S)) * 1.15));
      let r = 0x3b + (0x8b - 0x3b) * t;
      let g = 0x82 + (0x5c - 0x82) * t;
      let b = 0xf6 + (0xf6 - 0xf6) * t;
      // 左上角加一点高光
      const hl = Math.max(0, 1 - Math.hypot(x - S * 0.22, y - S * 0.18) / (S * 0.55));
      r += hl * 26; g += hl * 26; b += hl * 26;

      // 白色声波：一条水平中心线 + 上下起伏的包络
      const nx = (x - cx) / (S * 0.5);                 // -1..1
      const env = Math.exp(-(nx * nx) / 0.16);          // 中间的钟形包络
      const wy = cy - Math.sin(nx * Math.PI * 3.2) * env * S * 0.26;
      const thick = Math.max(1.2, S * 0.045);
      const dy = Math.abs(y + 0.5 - wy);
      const wave = 1 - smooth(thick * 0.5 - 0.7, thick * 0.5 + 0.7, dy);
      // 只在包络明显的地方画波（两端收细）
      const wv = wave * Math.min(1, env * 2.2);

      const a = cover;
      const mix = Math.min(1, wv);
      r = r * (1 - mix) + 255 * mix;
      g = g * (1 - mix) + 255 * mix;
      b = b * (1 - mix) + 255 * mix;

      px[i] = Math.round(Math.min(255, r));
      px[i + 1] = Math.round(Math.min(255, g));
      px[i + 2] = Math.round(Math.min(255, b));
      px[i + 3] = Math.round(255 * a);
    }
  }
  return px;
}

// ---- PNG 编码 ----
function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = c ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const t = Buffer.from(type, 'ascii');
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])), 0);
  return Buffer.concat([len, t, data, crc]);
}
function png(size) {
  const px = draw(size);
  const raw = Buffer.alloc((size * 4 + 1) * size);
  for (let y = 0; y < size; ++y) {
    raw[y * (size * 4 + 1)] = 0;
    px.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

const sizes = [16, 24, 32, 48, 64, 128, 256];
const pngs = sizes.map(s => ({ size: s, data: png(s) }));
fs.writeFileSync(path.join(OUT_DIR, 'rlplayer-256.png'), pngs[pngs.length - 1].data);
fs.writeFileSync(path.join(OUT_DIR, 'rlplayer-64.png'), pngs[4].data);

// ---- ICO 封装（PNG 负载，Vista+ 支持）----
const n = pngs.length;
const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(n, 4);
const dir = Buffer.alloc(16 * n);
let offset = 6 + 16 * n;
pngs.forEach((p, i) => {
  const o = i * 16;
  dir[o] = p.size >= 256 ? 0 : p.size;
  dir[o + 1] = p.size >= 256 ? 0 : p.size;
  dir[o + 2] = 0; dir[o + 3] = 0;
  dir.writeUInt16LE(1, o + 4);
  dir.writeUInt16LE(32, o + 6);
  dir.writeUInt32LE(p.data.length, o + 8);
  dir.writeUInt32LE(offset, o + 12);
  offset += p.data.length;
});
const ico = Buffer.concat([header, dir, ...pngs.map(p => p.data)]);
fs.writeFileSync(path.join(OUT_DIR, 'rlplayer.ico'), ico);
console.log('写出: ' + path.join(OUT_DIR, 'rlplayer.ico') + ' (' + ico.length + ' 字节, ' + n + ' 个尺寸)');

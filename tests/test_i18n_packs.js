// test_i18n_packs.js — 语言包完整性（2026-11-15 用户报「外语界面里还有中文」）
//
// 判据（全部是"语言包本身"就能证伪的事实，不需要起界面）：
//   ① 六个语种的键集合必须与 en 完全一致（少一条 = 那个语种会在界面上露中文）；
//   ② 除 en 外不得出现「en 里没有的单汉字」键 —— 那是早先按 DOM 扫描时把
//      专辑/艺术家首字占位（乐/亲/刘…）当成词条翻掉的垃圾，会把用户数据译坏；
//   ③ 用户报过的每一条文案都必须在六个包里都有词条（回归钉子）；
//   ④ A–D 四组旧默认名（@group: 前缀）与更新失败原因（UPD_ERR）都在。
// 为什么用 Node 而不是 C++：语言包是运行期 JSON，判据就是 JSON 本身。
const fs = require('node:fs');
const path = require('node:path');
const root = process.env.RLPLAYER_TEST_SOURCE_ROOT || path.join(__dirname, '..');
const dir = path.join(root, 'data', 'i18n');
const LANGS = ['en', 'ja', 'fr', 'ru', 'es', 'ar'];
const packs = {};
for (const l of LANGS) packs[l] = JSON.parse(fs.readFileSync(path.join(dir, l + '.json'), 'utf8'));

let checks = 0, failures = 0;
function check(ok, label) { ++checks; if (!ok) ++failures; console.log((ok ? 'PASS ' : 'FAIL ') + label); }

const enKeys = Object.keys(packs.en);
check(enKeys.length > 2000, 'en pack has ' + enKeys.length + ' entries');

// ① 键集合一致
for (const l of LANGS) {
  if (l === 'en') continue;
  const missing = enKeys.filter(k => !Object.prototype.hasOwnProperty.call(packs[l], k));
  check(missing.length === 0, l + ' covers every en key (missing=' + missing.length + (missing.length ? ' e.g. ' + JSON.stringify(missing.slice(0, 3)) : '') + ')');
}

// ② 非 en 包里不得有"en 没有的单汉字"键
const singleCJK = /^[\u3400-\u9fff\u3005\u3007]$/;
for (const l of LANGS) {
  if (l === 'en') continue;
  const junk = Object.keys(packs[l]).filter(k => singleCJK.test(k) && !Object.prototype.hasOwnProperty.call(packs.en, k));
  check(junk.length === 0, l + ' has no album-placeholder single-char keys (junk=' + junk.length + (junk.length ? ' e.g. ' + junk.slice(0, 5).join('') : '') + ')');
}

// ③ 用户报过的文案（2026-11-15）必须有词条
const REPORTED = [
  '暂无歌单，点 + 新建',
  '新建歌单',
  '音量旋钮',
  '静音',
  '音量（dB）；正增益可能削波',
  '输入 -120 至 +20 dB，Enter 确认，Esc 取消。',
  '📁 管理音乐目录',
  '🔄 重新扫描',
  '库里还没有音乐',
  '没有匹配的曲目',
  '播放台',
  '清除片段',
  '拖动选取片段 · 选区内拖动整体平移',
  '拖动空白处选取新片段；在选区内拖动可整段平移；拖两端微调。松手自动保存。',
  'SDM 1x 滤波',
  'SDM Nx 滤波',
  '本机 ASIO 驱动 {0} 个 · Native DSD 需后端 ASIO、源为 DSD、音量 0 dB、无 EQ / 卷积；支持设备可用的同族整数倍率转换，同位率未转换时可 bit-perfect，转换后非 bit-perfect；驱动拒绝时回落 DoP',
  '拒绝版本回滚',
  '检查失败：{0}'
];
for (const k of REPORTED) {
  const miss = LANGS.filter(l => !Object.prototype.hasOwnProperty.call(packs[l], k));
  check(miss.length === 0, 'reported string has an entry in all 6 packs: ' + JSON.stringify(k.slice(0, 28)) + (miss.length ? ' missing in ' + miss.join(',') : ''));
}

// ④ A–D 旧默认名 + 更新失败原因
for (const k of ['@group:流行', '@group:女声', '@group:男声', '@group:古典']) {
  const miss = LANGS.filter(l => !Object.prototype.hasOwnProperty.call(packs[l], k));
  check(miss.length === 0, 'legacy group name localized: ' + k + (miss.length ? ' missing in ' + miss.join(',') : ''));
}
const UPD_CODES = ['拒绝版本回滚', '网络不可用', '更新服务器鉴权失败', '更新清单已过期', '更新清单签名校验失败', '更新服务器返回 HTTP {0}'];
for (const k of UPD_CODES) {
  const miss = LANGS.filter(l => !Object.prototype.hasOwnProperty.call(packs[l], k));
  check(miss.length === 0, 'update failure reason localized: ' + JSON.stringify(k) + (miss.length ? ' missing in ' + miss.join(',') : ''));
}

// ⑤ 规则键（~ 开头）必须是合法正则，否则整条规则会被静默丢掉
for (const l of LANGS) {
  const bad = Object.keys(packs[l]).filter(k => k.charAt(0) === '~').filter(k => { try { new RegExp(k.slice(1)); return false; } catch (e) { return true; } });
  check(bad.length === 0, l + ' regex rules compile (' + bad.length + ' bad)');
}

// ⑥ 译文不得为空（空串在 i18n 里是"整段删掉"的语义，只允许极少数显式删除规则）
for (const l of LANGS) {
  const empty = Object.keys(packs[l]).filter(k => k.charAt(0) !== '~' && packs[l][k] === '');
  check(empty.length <= 2, l + ' has at most 2 intentional empty translations (' + empty.length + ')');
}

console.log(JSON.stringify({ checks, failures }));
process.exitCode = failures ? 1 : 0;

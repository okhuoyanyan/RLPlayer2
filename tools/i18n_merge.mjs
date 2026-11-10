// 把一批 {中文: 译文} 合并进 data/i18n/<lang>.json（保留原键，新增追加，按键排序）
import fs from 'node:fs';
const [, , lang, addFile] = process.argv;
const target = 'D:/RTA-hqplayer/data/i18n/' + lang + '.json';
const cur = JSON.parse(fs.readFileSync(target, 'utf8'));
const add = JSON.parse(fs.readFileSync(addFile, 'utf8'));
let added = 0, changed = 0, same = 0;
for (const [k, v] of Object.entries(add)) {
  if (!(k in cur)) { cur[k] = v; added++; }
  else if (cur[k] !== v) { cur[k] = v; changed++; }
  else same++;
}
const sorted = {};
for (const k of Object.keys(cur).sort()) sorted[k] = cur[k];
fs.writeFileSync(target, JSON.stringify(sorted, null, 2) + '\n', 'utf8');
console.log(lang + ': added=' + added + ' changed=' + changed + ' same=' + same + ' total=' + Object.keys(sorted).length);

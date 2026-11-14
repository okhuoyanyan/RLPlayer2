
import fs from "node:fs";
const lang = process.argv[2];
const dir = "D:\\RTA-hqplayer\\data\\i18n";
const en = JSON.parse(fs.readFileSync(dir + "\\en.json", "utf8"));
const pack = JSON.parse(fs.readFileSync(dir + "\\" + lang + ".json", "utf8"));
const zhKeys = new Set(Object.keys(en).filter(k => !k.startsWith("~")));
const text = fs.readFileSync("D:\\RTA-hqplayer\\build-takeover\\qa_text.log", "utf8");
const last = "=== step " + text.split("=== step ").pop();
const lines = last.split(/\r?\n/).map(s => s.trim()).filter(Boolean);
const misses = new Map();
for (const l of lines) {
  if (zhKeys.has(l) && !(l in pack)) misses.set(l, (misses.get(l) || 0) + 1);
}
const out = [...misses.keys()];
console.log(JSON.stringify({ lang, lines: lines.length, missing: out.length, sample: out.slice(0, 40) }, null, 1));

const fs = require('fs');
const path = require('path');
const png = fs.readFileSync(process.argv[2]);
const bytes = Array.from(png);
let out = '// app_icon.h — 内嵌的 64x64 图标（由 tools/make_icon.js + tools/embed_icon.js 生成）\n';
out += '// 用途：网页 /favicon.png，以及不依赖外部文件\n';
out += '#pragma once\n#include <cstddef>\n\nnamespace rta::net {\n\n';
out += 'inline const unsigned char kAppIconPng[] = {\n';
for (let i = 0; i < bytes.length; i += 16) {
  out += '    ' + bytes.slice(i, i + 16).map(b => b + ',').join('') + '\n';
}
out += '};\n\n';
out += 'inline const size_t kAppIconPngSize = sizeof(kAppIconPng);\n\n';
out += '} // namespace rta::net\n';
fs.writeFileSync(process.argv[3], out);
console.log('写出 ' + process.argv[3] + ' (' + png.length + ' 字节 PNG)');

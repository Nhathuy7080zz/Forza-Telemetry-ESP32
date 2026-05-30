const fs = require('fs');
const html = fs.readFileSync('../index.html', 'utf8');
const escaped = html.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\r?\n/g, '\\n" \n"');
const content = `#pragma once\nconst char index_html[] PROGMEM = "${escaped}";\n`;
fs.writeFileSync('index_html.h', content);
console.log('Created index_html.h');
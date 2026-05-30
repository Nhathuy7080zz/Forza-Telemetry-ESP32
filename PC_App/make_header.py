import sys

with open('../index.html', 'r', encoding='utf-8') as f:
    html = f.read()

escaped = html.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n"\n"')

with open('index_html.h', 'w', encoding='utf-8') as f:
    f.write('#pragma once\nconst char index_html[] PROGMEM = "')
    f.write(escaped)
    f.write('";\n')

print("Created index_html.h")
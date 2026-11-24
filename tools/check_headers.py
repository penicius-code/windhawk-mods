"""Check every Windhawk mod file in this repository for a complete header and consistent naming.

For each <folder>/<id>/<id>.wh.cpp under mods/ and modified/:
  - the header block and the readme block exist
  - @id equals the folder name and the file name
  - @name, @description, @version, @author and @include are present
  - mods/: @license is present, and @author is the repository owner
  - modified/: @author names the original author and the change ("... by <owner>"), the version carries a suffix
  - a README.md is next to the file, and the top-level README.md links the folder
Exit code 1 if anything is wrong. Usage: python tools/check_headers.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OWNER = 'penicius-code'
problems = []
count = 0


def header_fields(text):
    m = re.search(r'// ==WindhawkMod==\r?\n(.*?)// ==/WindhawkMod==', text, re.S)
    if not m:
        return None
    fields = {}
    for line in m.group(1).splitlines():
        f = re.match(r'//\s*@([\w:-]+)\s+(.*\S)\s*$', line)
        if f:
            fields.setdefault(f.group(1), []).append(f.group(2))
    return fields


top_readme = io.open(os.path.join(ROOT, 'README.md'), encoding='utf-8').read()
for group in ('mods', 'modified'):
    base = os.path.join(ROOT, group)
    for name in sorted(os.listdir(base)):
        folder = os.path.join(base, name)
        if not os.path.isdir(folder):
            continue
        count += 1
        where = f'{group}/{name}'
        path = os.path.join(folder, name + '.wh.cpp')
        if not os.path.isfile(path):
            problems.append(f'{where}: {name}.wh.cpp is missing')
            continue
        text = io.open(path, encoding='utf-8').read()
        fields = header_fields(text)
        if fields is None:
            problems.append(f'{where}: no ==WindhawkMod== header block')
            continue
        if '// ==WindhawkModReadme==' not in text:
            problems.append(f'{where}: no readme block')
        for key in ('id', 'name', 'description', 'version', 'author', 'include'):
            if key not in fields:
                problems.append(f'{where}: @{key} is missing')
        if fields.get('id', [''])[0] != name:
            problems.append(f'{where}: @id is {fields.get("id")}, expected {name}')
        author = fields.get('author', [''])[0]
        version = fields.get('version', [''])[0]
        if group == 'mods':
            if 'license' not in fields:
                problems.append(f'{where}: @license is missing')
            if author != OWNER:
                problems.append(f'{where}: @author is "{author}", expected {OWNER}')
        else:
            if not re.search(r'\S \(.* by ' + re.escape(OWNER) + r'\)$', author):
                problems.append(f'{where}: @author must keep the original author and name the change: "{author}"')
            if '-' not in version:
                problems.append(f'{where}: version "{version}" has no suffix that marks it as a modified copy')
        if not os.path.isfile(os.path.join(folder, 'README.md')):
            problems.append(f'{where}: README.md is missing')
        if f']({group}/{name}/)' not in top_readme:
            problems.append(f'{where}: not linked in the top-level README.md')

for p in problems:
    print('PROBLEM', p)
print(f'{count} mod folders checked, {len(problems)} problem(s)')
sys.exit(1 if problems else 0)

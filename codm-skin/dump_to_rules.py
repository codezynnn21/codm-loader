#!/usr/bin/env python3
"""Convert an Il2CppDumper dump.cs into codm-skin recon-style JSON.

Usage:
    python dump_to_rules.py dump.cs -f "skin|camo|weapon" -o codm_dump.json
    python dump_to_rules.py dump.cs -f "skin" -o codm_dump.json -r config.json
"""

import argparse
import json
import re
import sys

IMAGE_RE = re.compile(r'^//\s*Image\s+\d+:\s*(\S+)')
NS_RE = re.compile(r'^//\s*Namespace:\s*(.*)$')
RVA_RE = re.compile(r'^//\s*RVA:\s*(0x[0-9A-Fa-f]+)')
CLASS_RE = re.compile(r'\b(?:class|struct|interface|enum)\s+([A-Za-z_][\w`]*)')
FIELD_RE = re.compile(
    r'^\s*(?:public|private|protected|internal)\s+(?:static\s+|readonly\s+)*'
    r'([^;]+?)\s+([A-Za-z_]\w*)\s*;\s*//\s*0x([0-9A-Fa-f]+)'
)
SIG_RE = re.compile(
    r'^\s*(?:public|private|protected|internal)\s+(.+?)\s+'
    r'([A-Za-z_]\w*)\s*\(([^)]*)\)'
)
MODIFIERS = {
    'public', 'private', 'protected', 'internal', 'static', 'virtual',
    'override', 'abstract', 'sealed', 'extern', 'unsafe', 'async', 'new',
    'readonly', 'const', 'volatile',
}
INT_TYPES = {'int', 'uint', 'long', 'ulong', 'short', 'ushort', 'byte', 'sbyte'}


def parse(path):
    classes = {}
    order = []
    ns = ''
    image = ''
    pending_rva = None
    current = None

    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\n')

            m = IMAGE_RE.match(line)
            if m:
                image = m.group(1)
                continue

            m = NS_RE.match(line)
            if m:
                ns = m.group(1).strip()
                continue

            m = RVA_RE.match(line)
            if m:
                pending_rva = m.group(1)
                continue

            head = line.split('{')[0]
            if '{' in line and '(' not in head:
                m = CLASS_RE.search(line)
                if m:
                    name = m.group(1)
                    full = ns + '.' + name if ns else name
                    current = classes.get(full)
                    if current is None:
                        current = {'image': image, 'klass': full, 'methods': [], 'fields': []}
                        classes[full] = current
                        order.append(full)
                    pending_rva = None
                    continue

            if current is None:
                continue

            m = FIELD_RE.match(line)
            if m:
                current['fields'].append({
                    'name': m.group(2),
                    'type': m.group(1).strip(),
                    'offset': int(m.group(3), 16),
                })
                continue

            if ';' in line.split('(')[0]:
                continue
            m = SIG_RE.match(line)
            if m:
                pre, name, params = m.group(1), m.group(2), m.group(3)
                tokens = pre.split()
                while tokens and tokens[0] in MODIFIERS:
                    tokens.pop(0)
                returns = ' '.join(tokens) if tokens else ''
                count = len([p for p in params.split(',') if p.strip()])
                current['methods'].append({
                    'name': name,
                    'params': count,
                    'returns': returns,
                    'rva': pending_rva,
                })
                pending_rva = None

    return [classes[k] for k in order]


def main():
    ap = argparse.ArgumentParser(description='dump.cs -> codm-skin json')
    ap.add_argument('dump')
    ap.add_argument('-f', '--filter', default='skin|camo|weapon|loadout|appearance')
    ap.add_argument('-o', '--out', default='codm_dump.json')
    ap.add_argument('-r', '--rules', help='write a patch-mode config.json template here')
    ap.add_argument('--limit', type=int, default=400)
    args = ap.parse_args()

    try:
        classes = parse(args.dump)
    except OSError as e:
        print('cannot read dump: %s' % e, file=sys.stderr)
        return 1

    rex = re.compile(args.filter, re.I)
    matched = [c for c in classes if rex.search(c['klass'])][: args.limit]

    report = {'source': args.dump, 'matched': len(matched), 'classes': matched}
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2)
    print('%d classes parsed, %d matched -> %s' % (len(classes), len(matched), args.out))

    if args.rules:
        rules = []
        for c in matched:
            for m in c['methods']:
                if m['params'] == 0 and m['returns'] in INT_TYPES:
                    rules.append({
                        'class': c['klass'],
                        'method': m['name'],
                        'params': 0,
                        'value': 0,
                        'rva': m['rva'],
                    })
        with open(args.rules, 'w', encoding='utf-8') as f:
            json.dump({'mode': 'patch', 'rules': rules}, f, indent=2)
        print('%d rule templates -> %s (value 0 = replace with a real skin id)' % (len(rules), args.rules))

    return 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python
import ast
import sys

FNV1_32_INIT = 0x811c9dc5
FNV_32_PRIME = 0x01000193

def fnv1a_32(data, hval=FNV1_32_INIT):
    for byte in data:
        hval ^= byte
        hval = (hval * FNV_32_PRIME) & 0xffffffff
    return hval

def read_po(infile):
    current = {}
    active = None

    for raw_line in infile.read().decode('utf-8').splitlines() + ['']:
        line = raw_line.strip()
        if not line:
            if current:
                yield current
                current = {}
            active = None
            continue
        if line.startswith('#'):
            continue

        for key in ('msgctxt', 'msgid', 'msgstr'):
            prefix = key + ' '
            if line.startswith(prefix):
                active = key
                current[key] = ast.literal_eval(line[len(prefix):])
                break
        else:
            if active and line.startswith('"'):
                current[active] += ast.literal_eval(line)

def po2ymo(infile, outfile, encoding='utf-16le'):

    units = {}
    for unit in read_po(infile):
        source = unit.get('msgid', '')
        target = unit.get('msgstr', '')
        if not source or not target:
            continue
        context = unit.get('msgctxt')
        if context:
            source = context + '\004' + source
        hash = fnv1a_32(source.encode(encoding))
        units[hash] = target.encode(encoding) + bytes(2)

    byteorder='little'
    outfile.write(len(units).to_bytes(2, byteorder)) # len

    offset = 2 + len(units) * (4 + 2)
    for hash, data in units.items():
        outfile.write(hash.to_bytes(4, byteorder))
        outfile.write(offset.to_bytes(2, byteorder))
        offset += len(data)

    for data in units.values():
        outfile.write(data)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("usage: po2ymo.py <infile> <outfile>")
        sys.exit()
    infile = open(sys.argv[1], 'rb')
    outfile = open(sys.argv[2], 'wb')
    po2ymo(infile, outfile)

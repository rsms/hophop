#!/usr/bin/env python3
# Converts lib/c_prelude.h into a C header that declares kCPrelude as a string constant.
# Usage: python3 gen_c_prelude.py <input.h> > output.h
import sys

with open(sys.argv[1], 'r') as f:
    lines = f.readlines()

print('/* auto-generated from lib/c_prelude.h — do not edit */')
print('static const char kCPrelude[] = {')
for line in lines:
    s = line.rstrip('\n')
    s = s.replace('\\', '\\\\').replace('"', '\\"')
    print(f'"{s}\\n"')
print('"\\0"};')

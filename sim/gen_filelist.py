#!/usr/bin/env python3
"""Generate sim/filelist.f with Windows-native absolute paths.

Verilator under MSYS2 Perl does not accept /e/05_... POSIX drive paths
in -f filelists (treats them as module names rather than file paths).
Python3 on Windows returns E:/... native paths that Verilator handles
correctly. Called from sim/verilator/Makefile's $(FILELIST_F) recipe.

Usage: python3 sim/gen_filelist.py <output_filelist> <incdir1> [incdir2 ...] -- <src1> [src2 ...]
"""
import os, sys

args = sys.argv[1:]
out = args[0]
rest = args[1:]
sep = rest.index('--')
incdirs = rest[:sep]
srcs = rest[sep+1:]

lines = []
for d in incdirs:
    lines.append('+incdir+' + os.path.abspath(d).replace('\\', '/'))
for f in srcs:
    lines.append(os.path.abspath(f).replace('\\', '/'))

with open(out, 'w') as fh:
    fh.write('\n'.join(lines) + '\n')

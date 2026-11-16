#!/usr/bin/env python3
import pathlib,re,subprocess,sys
binary=sys.argv[1] if len(sys.argv)>1 else '../tool/lfb-enginesound/es-synth'
# Apple's objdump is LLVM and supports ELF dynamic symbol tables.
text=subprocess.check_output(['objdump','-T',binary],text=True)
pathlib.Path('build/glibc-symbols.txt').write_text(text)
versions=sorted(set(re.findall(r'GLIBC_(\d+(?:\.\d+)+)',text)),key=lambda x:tuple(map(int,x.split('.'))))
assert versions,'no GLIBC version evidence'
assert all(tuple(map(int,x.split('.'))) <= (2,27) for x in versions),versions
print('GLIBC versions: '+', '.join('GLIBC_'+v for v in versions)+' (all <= 2.27)')
# The head unit has no /lib64: its loader is /lib/ld-linux-x86-64.so.2 (link with -Wl,--dynamic-linker=..., as
# mgu-render, Moonride and mgu-python do). A binary asking for /lib64/... fails to start there with ENOENT.
out=subprocess.check_output(['file',binary],text=True)
m=re.search(r'interpreter (\S+?),',out)
interp=m.group(1) if m else None
assert interp=='/lib/ld-linux-x86-64.so.2','loader '+repr(interp)+': the head unit only has /lib/ld-linux-x86-64.so.2'
print('loader: '+interp)

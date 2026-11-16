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

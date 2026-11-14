"""Copy FEXCore's OnCodeBufferAllocated and InvalidateCodeBuffersCodeRange out of Core.cpp, unchanged."""
import re
import sys

src = open(sys.argv[1]).read()
parts = []
for name in ('OnCodeBufferAllocated', 'InvalidateCodeBuffersCodeRange'):
    m = re.search(r'^void ContextImpl::' + name + r'\(.*?\n\}\n', src, re.S | re.M)
    if not m:
        sys.exit('extract.py: ContextImpl::%s not found in %s' % (name, sys.argv[1]))
    parts.append(m.group(0))
open(sys.argv[2], 'w').write('\n'.join(parts))

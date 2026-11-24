#!/usr/bin/env python3
# The decoder core of JS8Call-improved's JS8.cpp without its Qt worker: the
# text up to "// Worker", then the encoder from "// Public Interface -
# Encoding" on, then WIFILT's decoder_wrapper.inc (the C API) — the same cut
# as WIFILT's cmake/extract_reference_core.cmake. JS8.cpp is pinned by its
# sha256: a new upstream is reviewed, then the pin here changes.
import hashlib
import sys

original, wrapper, output, pin = sys.argv[1:5]
data = open(original, 'rb').read()
got = hashlib.sha256(data).hexdigest()
if got != pin:
    sys.exit(f'JS8.cpp changed: expected {pin}, got {got} — review upstream DSP, then update the pin')
text = data.decode('utf-8')
core_end = text.find('// Worker')
enc_start = text.find('// Public Interface - Encoding')
if core_end < 0 or enc_start < 0:
    sys.exit('JS8.cpp: section markers not found')
combined = text[:core_end] + '\n' + text[enc_start:] + '\n' + open(wrapper, encoding='utf-8').read() + '\n'
try:
    if open(output, encoding='utf-8').read() == combined:
        sys.exit(0)
except OSError:
    pass
open(output, 'w', encoding='utf-8').write(combined)

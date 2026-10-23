"""Compile the context memcpy contract against the actual recompiled BIOS body.

The reference is extracted unchanged from the checked-in generated BIOS, with
only memory/timing hooks redirected by the test harness. No copied reference
implementation can drift from the emitter output. Usage: --cc gcc (or clang).
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--cc', default='cc')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'generated/OpenBIOS_full.c').read_text()
start = source.index('void OpenBIOS_func_1FC085D8(CPUState* cpu) {')
end = source.index('\n}\n', start) + 3
with tempfile.TemporaryDirectory(prefix='dctx-bios-') as directory:
    tmp = Path(directory)
    (tmp / 'dctx_bios_reference.inc').write_text(source[start:end])
    binary = tmp / 'test_dctx_bios.exe'
    subprocess.run([a.cc, '-std=c11', '-O2', '-pthread', '-I' + str(root / 'runtime/include'),
                    '-I' + str(tmp), str(root / 'runtime/tests/test_dctx_bios.c'),
                    str(root / 'runtime/src/psx_dctx_bios.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(root / 'bios/openbios.bin')], check=True)

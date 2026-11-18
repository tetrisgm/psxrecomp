#!/usr/bin/env python3
"""Compare native CRC instructions and portable slicing with IEEE zlib CRC."""
import argparse
import ctypes
from pathlib import Path
import random
import subprocess
import tempfile
import zlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    rng = random.Random(4132)
    with tempfile.TemporaryDirectory() as directory:
        functions = []
        for name, flags in [('native', []), ('portable', ['-U__ARM_FEATURE_CRC32'])]:
            library = Path(directory) / (name + '.so')
            subprocess.run([args.compiler, '-O3', '-shared', '-fPIC', *flags,
                            '-I' + str(root / 'include'), str(root / 'src/crc32.c'),
                            '-o', str(library)], check=True)
            fn = ctypes.CDLL(str(library)).crc32_update
            fn.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
            fn.restype = ctypes.c_uint32
            functions.append(fn)
        data = rng.randbytes(2 * 1024 * 1024 + 32)
        buffer = ctypes.create_string_buffer(data)
        cases = [(offset, length) for offset in range(16) for length in range(256)]
        cases += [(rng.randrange(16), rng.randrange(256, len(data) - 32))
                  for _ in range(128)]
        for offset, length in cases:
            initial = rng.getrandbits(32)
            expected = zlib.crc32(data[offset:offset + length], initial ^ 0xffffffff) ^ 0xffffffff
            address = ctypes.addressof(buffer) + offset
            split = length // 3
            for fn in functions:
                assert fn(initial, address, length) == expected, (offset, length)
                partial = fn(initial, address, split)
                assert fn(partial, address + split, length - split) == expected
        print(f'{len(cases)} unaligned, arbitrary-seed and incremental cases passed')


if __name__ == '__main__':
    main()

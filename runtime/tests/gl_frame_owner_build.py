"""Shared C++ linkage for the native raster and complete-frame GL owner."""
from pathlib import Path


def cxx_compiler(cc, explicit=None):
    if explicit:
        return explicit
    compiler = Path(cc)
    names = {"gcc": "g++", "gcc.exe": "g++.exe", "cc": "c++",
             "cc.exe": "c++.exe", "clang": "clang++", "clang.exe": "clang++.exe"}
    sibling = compiler.with_name(names.get(compiler.name, "c++"))
    return str(sibling) if sibling.is_file() else "c++"


def build_frame_owner(run, cxx, source, output, flags):
    objects = []
    flags = [flag for flag in flags if not str(flag).startswith("-std=")]
    for name in ("frame_owner", "frame_packet", "native_raster",
                 "packet_interpolation", "packet_interpolation_stream"):
        obj = output / (name + ".o")
        result = run([cxx, "-std=c++17", *flags, "-c", source / (name + ".cpp"), "-o", obj])
        if result is not None and result.returncode:
            print(result.stderr[-3000:])
            return None
        objects.append(obj)
    return objects

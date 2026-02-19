#!/usr/bin/env python3
import os, sys, stat

def pad4(n): return (4 - (n % 4)) % 4
def hex8(x): return f"{x:08x}".encode()

def write_hdr(out, name, mode, filesize):
    # newc header (110 bytes ASCII)
    out.write(b"070701")          # c_magic
    out.write(hex8(0))            # c_ino
    out.write(hex8(mode))         # c_mode
    out.write(hex8(0))            # c_uid
    out.write(hex8(0))            # c_gid
    out.write(hex8(1))            # c_nlink
    out.write(hex8(0))            # c_mtime
    out.write(hex8(filesize))     # c_filesize
    out.write(hex8(0))            # c_devmajor
    out.write(hex8(0))            # c_devminor
    out.write(hex8(0))            # c_rdevmajor
    out.write(hex8(0))            # c_rdevminor
    out.write(hex8(len(name) + 1))# c_namesize
    out.write(hex8(0))            # c_check

def write_entry(out, name, mode, data=b""):
    write_hdr(out, name, mode, len(data))
    out.write(name.encode() + b"\x00")
    out.write(b"\x00" * pad4(110 + len(name) + 1))
    out.write(data)
    out.write(b"\x00" * pad4(len(data)))

def main():
    if len(sys.argv) != 3:
        print("usage: mkcpio.py <root_dir> <out.cpio>", file=sys.stderr)
        sys.exit(1)

    root = sys.argv[1]
    outp = sys.argv[2]

    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        filenames.sort()
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace("\\", "/")
            files.append((full, rel))

    with open(outp, "wb") as out:
        # include directories too (helps later when you extract/mount)
        # write directories first
        for dirpath, dirnames, filenames in os.walk(root):
            rel_dir = os.path.relpath(dirpath, root).replace("\\", "/")
            if rel_dir == ".":
                continue
            write_entry(out, rel_dir, stat.S_IFDIR | 0o755, b"")

        # write files
        for full, rel in files:
            with open(full, "rb") as f:
                data = f.read()
            write_entry(out, rel, stat.S_IFREG | 0o644, data)

        # trailer
        write_entry(out, "TRAILER!!!", stat.S_IFREG | 0o644, b"")

    print(f"OK: wrote {outp} with {len(files)} file(s)")

if __name__ == "__main__":
    main()

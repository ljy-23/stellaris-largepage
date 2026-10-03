#!/usr/bin/env python3
"""Stellaris 4.5.x 大页补丁安装器 / 卸载器（自包含，不依赖其它脚本）

用法：
    python patch_stellaris.py status
    python patch_stellaris.py install [payload.dll]
    python patch_stellaris.py restore

原理：
    在 stellaris.exe 里新增一个 PE 节 (.pdxlp)，把入口函数的第一条
    `call rel32` 重定向到该节；节里的存根用 LoadLibraryA 加载我们的
    分配器 DLL，然后用一条**直接相对跳转 (E9)** 回到原来的目标。
    必须用 E9 而不是 `mov rax / jmp rax`：游戏开了 CFG
    (Control Flow Guard)，间接跳转会触发 CFG 违规。

    只改两个地方：入口 call 的 4 字节位移 + 新增一个节；其余原样保留。
    原始 exe 备份在 backup/stellaris.exe.orig，restore 会逐字节还原。
"""

import hashlib
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = r"E:\SteamLibrary\steamapps\common\Stellaris"
EXE = os.path.join(GAME, "stellaris.exe")
BAK = os.path.join(HERE, "backup", "stellaris.exe.orig")
DEFAULT_DLL = os.path.join(HERE, "lpshim.dll")
SEC_NAME = b".pdxlp\0\0"
IAT_LOADLIBRARYA = 0x2216390          # stellaris.exe 4.5.1 (358e) 的 IAT 槽


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def sha(path, n=16):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:n]


def parse(d):
    e = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e:e + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    fh = e + 4
    nsec = struct.unpack_from("<H", d, fh + 2)[0]
    optsz = struct.unpack_from("<H", d, fh + 16)[0]
    opt = fh + 20
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        raise SystemExit("not PE32+")
    sof = opt + optsz
    secs = []
    for i in range(nsec):
        s = sof + i * 40
        secs.append(dict(name=d[s:s + 8].rstrip(b"\0").decode("latin1"),
                         vsz=struct.unpack_from("<I", d, s + 8)[0],
                         va=struct.unpack_from("<I", d, s + 12)[0],
                         rawsz=struct.unpack_from("<I", d, s + 16)[0],
                         praw=struct.unpack_from("<I", d, s + 20)[0], hdr=s))
    return dict(fh=fh, nsec=nsec, opt=opt, sof=sof, secs=secs,
                entry=struct.unpack_from("<I", d, opt + 16)[0],
                imgbase=struct.unpack_from("<Q", d, opt + 24)[0],
                secalign=struct.unpack_from("<I", d, opt + 32)[0],
                filealign=struct.unpack_from("<I", d, opt + 36)[0])


def rva_to_off(secs, rva):
    for s in secs:
        if s["va"] <= rva < s["va"] + max(s["vsz"], s["rawsz"]):
            return s["praw"] + (rva - s["va"])
    return None


def build_stub(stub_va, cont_va, dll_path):
    """sub rsp,28h ; lea rcx,[path] ; call [LoadLibraryA] ; add rsp,28h ; jmp cont"""
    path_off = 26
    st = bytearray()
    st += bytes([0x48, 0x83, 0xEC, 0x28])
    st += bytes([0x48, 0x8D, 0x0D]) + struct.pack("<i", (stub_va + path_off) - (stub_va + 11))
    st += bytes([0xFF, 0x15]) + struct.pack("<i", (0x140000000 + IAT_LOADLIBRARYA) - (stub_va + 17))
    st += bytes([0x48, 0x83, 0xC4, 0x28])
    st += bytes([0xE9]) + struct.pack("<i", cont_va - (stub_va + 26))
    assert len(st) == path_off, len(st)
    st += dll_path.encode("ascii") + b"\0"
    return bytes(st)


def cmd_status():
    d = open(EXE, "rb").read()
    print(f"game exe : {EXE}")
    print(f"  sha256 : {sha(EXE)}")
    print(f"  size   : {len(d)}")
    print(f"backup   : {BAK}  {'EXISTS ' + sha(BAK) if os.path.exists(BAK) else 'MISSING'}")
    p = parse(d)
    names = [s["name"] for s in p["secs"]]
    patched = ".pdxlp" in names
    print(f"patched  : {'YES' if patched else 'no'}   sections={names}")
    print(f"entry    : RVA 0x{p['entry']:X}  imgbase 0x{p['imgbase']:X}")
    if patched:
        o = rva_to_off(p["secs"], p["entry"]) + 4
        tgt = p["entry"] + 9 + struct.unpack_from("<i", d, o + 1)[0]
        sec = [s for s in p["secs"] if s["name"] == ".pdxlp"][0]
        print(f"  entry call -> RVA 0x{tgt:X} (stub in .pdxlp @0x{sec['va']:X})")


def cmd_install(dll):
    if not os.path.exists(BAK):
        os.makedirs(os.path.dirname(BAK), exist_ok=True)
        shutil.copy2(EXE, BAK)
        print(f"backup created -> {BAK} ({sha(BAK)})")
    d = bytearray(open(EXE, "rb").read())
    p = parse(d)
    if ".pdxlp" in [s["name"] for s in p["secs"]]:
        print("already patched - run 'restore' first if you want to repatch")
        return
    call_off = rva_to_off(p["secs"], p["entry"]) + 4
    if d[call_off] != 0xE8:
        raise SystemExit(f"entry+4 is 0x{d[call_off]:02X}, not a call - unsupported build")
    cont_va = p["imgbase"] + p["entry"] + 9 + struct.unpack_from("<i", d, call_off + 1)[0]

    raw_end = max(s["praw"] + s["rawsz"] for s in p["secs"])
    new_raw = align(raw_end, p["filealign"])
    new_va = align(max(s["va"] + max(s["vsz"], s["rawsz"]) for s in p["secs"]), p["secalign"])
    stub_va = p["imgbase"] + new_va
    blob = build_stub(stub_va, cont_va, dll)
    print(f"payload  : {dll}")
    print(f"new sect : VA 0x{new_va:X} raw 0x{new_raw:X} stub 0x{stub_va:X} cont 0x{cont_va:X}")

    h = p["sof"] + p["nsec"] * 40
    hdr = bytearray(40)
    hdr[0:8] = SEC_NAME
    struct.pack_into("<I", hdr, 8, len(blob))
    struct.pack_into("<I", hdr, 12, new_va)
    struct.pack_into("<I", hdr, 16, len(blob))
    struct.pack_into("<I", hdr, 20, new_raw)
    struct.pack_into("<I", hdr, 36, 0x60000020)          # CODE | EXECUTE | READ
    d[h:h + 40] = hdr
    struct.pack_into("<H", d, p["fh"] + 2, p["nsec"] + 1)
    struct.pack_into("<I", d, p["opt"] + 56, align(new_va + len(blob), p["secalign"]))
    struct.pack_into("<i", d, call_off + 1, stub_va - (p["imgbase"] + p["entry"] + 9))
    if len(d) < new_raw:
        d += b"\0" * (new_raw - len(d))
    d[new_raw:new_raw + len(blob)] = blob
    open(EXE, "wb").write(bytes(d))
    print(f"installed. new sha256 = {sha(EXE)}")


def cmd_restore():
    if not os.path.exists(BAK):
        raise SystemExit(f"no backup at {BAK} - cannot restore")
    shutil.copy2(BAK, EXE)
    print(f"restored from backup. sha256 = {sha(EXE)}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "status":
        cmd_status()
    elif cmd == "install":
        cmd_install(sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DLL)
    elif cmd == "restore":
        cmd_restore()
    else:
        raise SystemExit(__doc__)

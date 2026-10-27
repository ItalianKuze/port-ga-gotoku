#!/usr/bin/env python3
"""PortGaGotoku save slots: import from and export to the PS4 / shadPS4 layout.

PS4 (Apollo Save Tool, "Copy/Export decrypted save") and shadPS4 keep a slot as one folder:
    <slot>/data.saveps4, printclupak0..23, ...   files the game wrote
    <slot>/sce_sys/param.sfo                    title, subtitle, detail, dir name, user param
    <slot>/sce_sys/icon0.png                    (optional)
shadPS4 puts the slots in <shadPS4 user>/savedata/<user id>/<title>/<slot>/ (the folder passed with
--shadps4 is <shadPS4 user>/savedata).
bbport keeps a slot as two sibling folders (src/runtime_savedata.c):
    <data>/user/savedata/<user>/<title>/<slot>/          the game's files
    <data>/user/savedata/<user>/<title>/<slot>.sce_sys/  param.bin (Param, 1328 bytes), icon0.png
For Hokuto: <title> = CUSA12776 (save title id, not CUSA12781), <user> = 1, game slots 01, 02, ...
and 99 (system data). Without --title, <title> is the game's INSTALL_DIR_SAVEDATA (else TITLE_ID) from
the param.sfo of --game, or of the game folder in <data>/launcher.json; CUSA12776 when there is none.

    pgg_saves.py import <slot folder> --data <PortGaGotoku-data> [--slot NN] [--apply]
    pgg_saves.py export <slot> --data <PortGaGotoku-data> --to <folder> [--shadps4] [--apply]

Without --apply nothing is written: the script prints what it would do. Import refuses to run while
the game is running, copies an existing slot to <data>/save-backups/<slot>-import-<date-time>/ (same
layout as the runtime backups), copies the files, writes param.bin (dated like the source's param.sfo)
and verifies every copied file by SHA-256. Export writes param.sfo as shadPS4 does and refuses a target
slot folder that already exists. Neither deletes files.

As a module: plan_import()/apply_import(), plan_export()/apply_export(), read_sfo()/write_sfo(),
read_param()/pack_param().
"""
import argparse, datetime, hashlib, json, os, shutil, struct, subprocess, sys
from dataclasses import dataclass, field

PARAM = struct.Struct("<128s128s1024sIiq32s")  # Param in runtime_savedata.c
DEFAULT_TITLE = "CUSA12776"  # Hokuto CUSA12781's save title id
DEFAULT_BLOCKS = 320  # SAVEDATA_BLOCKS in shadPS4's param.sfo: the blocks Hokuto mounts its slots with
assert PARAM.size == 1328

SFO_BINARY, SFO_TEXT, SFO_INT = 0x0004, 0x0204, 0x0404


class SaveError(Exception):
    pass


def read_sfo(path):
    """param.sfo as {key: value}: str for text, int for integers, bytes for binary values."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 20:
        raise SaveError(f"{path}: not a param.sfo")
    magic, _version, keys, data, count = struct.unpack_from("<4sIIII", raw)
    if magic != b"\0PSF":
        raise SaveError(f"{path}: not a param.sfo")
    out = {}
    for i in range(count):
        key_off, fmt, length, _max, data_off = struct.unpack_from("<HHIII", raw, 20 + 16 * i)
        key = raw[keys + key_off:raw.index(b"\0", keys + key_off)].decode()
        value = raw[data + data_off:data + data_off + length]
        if fmt == SFO_INT:
            out[key] = struct.unpack("<I", value[:4])[0]
        elif fmt == SFO_TEXT:
            out[key] = value.split(b"\0")[0].decode("utf-8", "replace")
        else:
            out[key] = bytes(value)
    return out


def write_sfo(entries):
    """param.sfo from [(key, fmt, value, max length)], laid out as shadPS4 writes it: keys unpadded,
    each value at a file offset aligned to 4 with its full max length (zero-filled)."""
    keys = b"".join(k.encode() + b"\0" for k, *_ in entries)
    key_table = 20 + 16 * len(entries)
    data_table = key_table + len(keys)
    table, values = bytearray(), bytearray()
    key_off, pos = 0, data_table
    for key, fmt, value, size in entries:
        pos += -pos % 4
        if fmt == SFO_INT:
            raw = struct.pack("<I", value)
        elif fmt == SFO_TEXT:
            raw = value.encode("utf-8")[:size - 1] + b"\0"
        else:
            raw = bytes(value)[:size]
        table += struct.pack("<HHIII", key_off, fmt, len(raw), size, pos - data_table)
        values += bytes(pos - data_table - len(values)) + raw.ljust(size, b"\0")
        key_off += len(key) + 1
        pos += size
    return struct.pack("<4sIIII", b"\0PSF", 0x101, key_table, data_table, len(entries)) + table + keys + values


def slot_sfo(param, slot, title, blocks=DEFAULT_BLOCKS):
    """A slot's param.sfo from its Param (read_param()), with shadPS4's keys in shadPS4's order."""
    return write_sfo([
        ("ACCOUNT_ID", SFO_BINARY, bytes(8), 8),
        ("MAINTITLE", SFO_TEXT, param["title"], 128),
        ("SUBTITLE", SFO_TEXT, param["subtitle"], 128),
        ("DETAIL", SFO_TEXT, param["detail"], 1024),
        ("SAVEDATA_DIRECTORY", SFO_TEXT, slot, 32),
        ("SAVEDATA_LIST_PARAM", SFO_INT, param["user_param"], 4),
        ("TITLE_ID", SFO_TEXT, title, 12),
        ("SAVEDATA_BLOCKS", SFO_BINARY, struct.pack("<Q", blocks), 8),
    ])


def pack_param(title="", subtitle="", detail="", user_param=0, mtime=0):
    enc = lambda s, n: s.encode("utf-8")[:n - 1]
    return PARAM.pack(enc(title, 128), enc(subtitle, 128), enc(detail, 1024), user_param, 0, int(mtime), b"")


def read_param(path):
    with open(path, "rb") as f:
        raw = f.read(PARAM.size)
    if len(raw) != PARAM.size:
        raise SaveError(f"{path}: not a param.bin ({len(raw)} bytes)")
    title, subtitle, detail, user_param, _pad, mtime, _ = PARAM.unpack(raw)
    text = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
    return {"title": text(title), "subtitle": text(subtitle), "detail": text(detail),
            "user_param": user_param, "mtime": mtime}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def save_title(game, data):
    """The title id the game saves under (as src/probe.c): INSTALL_DIR_SAVEDATA, else TITLE_ID, of
    the game's param.sfo; the game folder is `game`, else the launcher's (<data>/launcher.json)."""
    if not game:
        try:
            with open(os.path.join(data, "launcher.json"), encoding="utf-8") as f:
                game = json.load(f).get("game_dir", "")
        except (OSError, ValueError):
            game = ""
    sfo_path = os.path.join(game, "sce_sys", "param.sfo") if game else ""
    sfo = read_sfo(sfo_path) if sfo_path and os.path.isfile(sfo_path) else {}
    return sfo.get("INSTALL_DIR_SAVEDATA") or sfo.get("TITLE_ID") or DEFAULT_TITLE


def game_running():
    if os.name != "nt":
        return False
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq bb-probe.exe"], capture_output=True, text=True).stdout
    return "bb-probe.exe" in out


def valid_slot(slot):
    return bool(slot) and len(slot) < 32 and not any(c in slot for c in "/\\:") and not slot.startswith(".")


def slot_dirs(data, user, title, slot):
    """bbport's folders of a slot: (game files, .sce_sys)."""
    base = os.path.join(data, "user", "savedata", user, title)
    return os.path.join(base, slot), os.path.join(base, slot + ".sce_sys")


def slot_files(folder):
    return sorted(n for n in os.listdir(folder) if n != "sce_sys" and os.path.isfile(os.path.join(folder, n)))


@dataclass
class ImportPlan:
    source: str
    slot: str
    title: str
    sfo: dict
    files: list
    host: str
    meta: str
    backup: str = ""  # set when the slot exists
    warnings: list = field(default_factory=list)


def plan_import(source, data, slot=None, title=None, game=None, user="1"):
    """What importing the PS4 / shadPS4 slot folder `source` into <data> would do."""
    title = title or save_title(game, data)
    sfo_path = os.path.join(source, "sce_sys", "param.sfo")
    sfo = read_sfo(sfo_path) if os.path.isfile(sfo_path) else {}
    slot = slot or sfo.get("SAVEDATA_DIRECTORY")
    if not valid_slot(slot):
        raise SaveError("slot name missing or invalid: pass --slot")
    files = slot_files(source)
    if not files:
        raise SaveError(f"{source}: no save files")
    plan = ImportPlan(source, slot, title, sfo, files, *slot_dirs(data, user, title, slot))
    if not sfo:
        plan.warnings.append("param.sfo missing: the slot gets an empty title")
    if sfo.get("TITLE_ID") and sfo["TITLE_ID"] != title:
        plan.warnings.append(f"param.sfo TITLE_ID {sfo['TITLE_ID']} differs from {title}")
    if os.path.isdir(plan.host) or os.path.isdir(plan.meta):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        plan.backup = os.path.join(data, "save-backups", f"{slot}-import-{stamp}")
    return plan


def apply_import(plan):
    """Back up the existing slot, copy the files, write param.bin, verify. Returns the files in the
    slot that the source does not have (left as they were)."""
    if game_running():
        raise SaveError("the game is running (bb-probe.exe): close it first")
    if plan.backup:
        for src in (plan.host, plan.meta):
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(plan.backup, os.path.basename(src)))
    os.makedirs(plan.host, exist_ok=True)
    os.makedirs(plan.meta, exist_ok=True)
    for name in plan.files:
        shutil.copy2(os.path.join(plan.source, name), os.path.join(plan.host, name))
    icon = os.path.join(plan.source, "sce_sys", "icon0.png")
    if os.path.isfile(icon):
        shutil.copy2(icon, os.path.join(plan.meta, "icon0.png"))
    # The runtime dates a slot by param.bin's mtime: keep the source's date.
    sfo_path = os.path.join(plan.source, "sce_sys", "param.sfo")
    mtime = os.path.getmtime(sfo_path) if plan.sfo else datetime.datetime.now().timestamp()
    param_path = os.path.join(plan.meta, "param.bin")
    with open(param_path, "wb") as f:
        f.write(pack_param(plan.sfo.get("MAINTITLE", ""), plan.sfo.get("SUBTITLE", ""), plan.sfo.get("DETAIL", ""),
                           plan.sfo.get("SAVEDATA_LIST_PARAM", 0), mtime))
    os.utime(param_path, (mtime, mtime))

    bad = [n for n in plan.files if sha(os.path.join(plan.source, n)) != sha(os.path.join(plan.host, n))]
    if bad:
        raise SaveError(f"VERIFY FAILED: {bad}; restore from {plan.backup or '(no backup: new slot)'}")
    return sorted(set(os.listdir(plan.host)) - set(plan.files))


@dataclass
class ExportPlan:
    slot: str
    title: str
    param: dict
    files: list
    host: str
    meta: str
    target: str
    blocks: int = DEFAULT_BLOCKS


def plan_export(data, slot, dest, shadps4=False, title=None, game=None, user="1", blocks=DEFAULT_BLOCKS):
    """What exporting bbport's slot `slot` would do: to <dest>/<slot>/ (PS4, Apollo's decrypted
    layout), or with shadps4 to <dest>/<user>/<title>/<slot>/ (dest = shadPS4's user/savedata)."""
    title = title or save_title(game, data)
    if not valid_slot(slot):
        raise SaveError(f"invalid slot name: {slot!r}")
    host, meta = slot_dirs(data, user, title, slot)
    if not os.path.isdir(host) or not os.path.isfile(os.path.join(meta, "param.bin")):
        raise SaveError(f"slot {slot} not found in {os.path.dirname(host)}")
    files = slot_files(host)
    if not files:
        raise SaveError(f"{host}: no save files")
    target = os.path.join(dest, user, title, slot) if shadps4 else os.path.join(dest, slot)
    if os.path.exists(target):
        raise SaveError(f"{target} already exists: choose another folder or move it away first")
    return ExportPlan(slot, title, read_param(os.path.join(meta, "param.bin")), files, host, meta, target, blocks)


def apply_export(plan):
    if os.path.exists(plan.target):
        raise SaveError(f"{plan.target} already exists")
    sys_dir = os.path.join(plan.target, "sce_sys")
    os.makedirs(sys_dir)
    for name in plan.files:
        shutil.copy2(os.path.join(plan.host, name), os.path.join(plan.target, name))
    icon = os.path.join(plan.meta, "icon0.png")
    if os.path.isfile(icon):
        shutil.copy2(icon, os.path.join(sys_dir, "icon0.png"))
    sfo_path = os.path.join(sys_dir, "param.sfo")
    with open(sfo_path, "wb") as f:
        f.write(slot_sfo(plan.param, plan.slot, plan.title, plan.blocks))
    mtime = os.path.getmtime(os.path.join(plan.meta, "param.bin"))
    os.utime(sfo_path, (mtime, mtime))
    bad = [n for n in plan.files if sha(os.path.join(plan.host, n)) != sha(os.path.join(plan.target, n))]
    if bad:
        raise SaveError(f"VERIFY FAILED: {bad} in {plan.target}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    imp = sub.add_parser("import", help="a PS4 / shadPS4 slot folder into PortGaGotoku")
    imp.add_argument("source", help="slot folder (contains sce_sys/param.sfo)")
    imp.add_argument("--slot", help="target slot (default: SAVEDATA_DIRECTORY from param.sfo)")
    exp = sub.add_parser("export", help="a PortGaGotoku slot to the PS4 / shadPS4 layout")
    exp.add_argument("slot", help="slot to export (01, 02, ..., 99)")
    exp.add_argument("--to", required=True, help="destination folder (with --shadps4: shadPS4's user/savedata)")
    exp.add_argument("--shadps4", action="store_true", help="write to <to>/<user>/<title>/<slot>")
    exp.add_argument("--blocks", type=int, default=DEFAULT_BLOCKS, help="SAVEDATA_BLOCKS in param.sfo")
    for p in (imp, exp):
        p.add_argument("--data", required=True, help="PortGaGotoku-data folder")
        p.add_argument("--title", help="save title id (default: from the game's param.sfo, else CUSA12776)")
        p.add_argument("--game", help="game folder for --title (default: the launcher's, from <data>/launcher.json)")
        p.add_argument("--user", default="1")
        p.add_argument("--apply", action="store_true", help="write (default: only show the plan)")
    a = ap.parse_args(argv)
    try:
        if a.command == "import":
            plan = plan_import(a.source, a.data, a.slot, a.title, a.game, a.user)
            print(f"source: {plan.source} ({len(plan.files)} files, param.sfo {'found' if plan.sfo else 'MISSING'})")
            print(f"title: {plan.sfo.get('MAINTITLE', '')!r} / {plan.sfo.get('SUBTITLE', '')!r}")
            print(f"target: {plan.host} (+ {os.path.basename(plan.meta)})")
            print(f"existing slot: {'yes, backup to ' + plan.backup if plan.backup else 'no'}")
            for w in plan.warnings:
                print(f"WARNING: {w}")
            if not a.apply:
                print("dry run: nothing written (add --apply)")
                return 0
            extra = apply_import(plan)
            if plan.backup:
                print(f"backup written: {plan.backup}")
            print(f"imported and verified {len(plan.files)} files into slot {plan.slot}")
            if extra:
                print(f"note: files in the slot not in the export (left as they were): {extra}")
        else:
            plan = plan_export(a.data, a.slot, a.to, a.shadps4, a.title, a.game, a.user, a.blocks)
            print(f"source: {plan.host} ({len(plan.files)} files)")
            print(f"title: {plan.param['title']!r} / {plan.param['subtitle']!r}")
            print(f"target: {plan.target} ({'shadPS4' if a.shadps4 else 'PS4'} layout, sce_sys/param.sfo)")
            if not a.apply:
                print("dry run: nothing written (add --apply)")
                return 0
            apply_export(plan)
            print(f"exported and verified {len(plan.files)} files to {plan.target}")
    except SaveError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

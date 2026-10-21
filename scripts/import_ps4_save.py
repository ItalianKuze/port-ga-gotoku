#!/usr/bin/env python3
"""Import one PS4 save exported decrypted by Apollo Save Tool into PortGaGotoku.

Apollo ("Copy/Export decrypted save") gives a folder with the game's files plus sce_sys/:
    <export>/data.saveps4, printclupak0..23, ...   files the game wrote
    <export>/sce_sys/param.sfo                    title, subtitle, detail, dir name, user param
    <export>/sce_sys/icon0.png                    (optional)
bbport keeps a slot as two sibling folders (src/runtime_savedata.c):
    <data>/user/savedata/<user>/<title>/<slot>/          the game's files
    <data>/user/savedata/<user>/<title>/<slot>.sce_sys/  param.bin (Param, 1328 bytes), icon0.png
For Hokuto: <title> = CUSA12776 (save title id, not CUSA12781), <user> = 1, game slots 01, 02, ...
and 99 (system data). Without --title, <title> is the game's INSTALL_DIR_SAVEDATA (else TITLE_ID) from
the param.sfo of --game, or of the game folder in <data>/launcher.json; CUSA12776 when there is none.

Without --apply nothing is written: the script prints what it would do. With --apply it refuses to run
while the game is running, copies an existing slot to <data>/save-backups/<slot>-import-<date-time>/
(same layout as the runtime backups), copies the files, writes param.bin and verifies every copied
file by SHA-256. It never deletes the old slot's files outside the slot being replaced.
"""
import argparse, datetime, hashlib, json, os, shutil, struct, subprocess, sys

PARAM = struct.Struct("<128s128s1024sIiq32s")  # Param in runtime_savedata.c
DEFAULT_TITLE = "CUSA12776"  # Hokuto CUSA12781's save title id
assert PARAM.size == 1328


def read_sfo(path):
    raw = open(path, "rb").read()
    magic, _version, keys, data, count = struct.unpack_from("<4sIIII", raw)
    if magic != b"\0PSF":
        raise SystemExit(f"{path}: not a param.sfo")
    out = {}
    for i in range(count):
        key_off, fmt, length, _max, data_off = struct.unpack_from("<HHIII", raw, 20 + 16 * i)
        key = raw[keys + key_off:raw.index(b"\0", keys + key_off)].decode()
        value = raw[data + data_off:data + data_off + length]
        out[key] = struct.unpack("<I", value[:4])[0] if fmt == 0x0404 else value.split(b"\0")[0].decode("utf-8", "replace")
    return out


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("export", help="folder exported by Apollo (contains sce_sys/param.sfo)")
    ap.add_argument("--data", required=True, help="PortGaGotoku-data folder")
    ap.add_argument("--slot", help="target slot (default: SAVEDATA_DIRECTORY from param.sfo)")
    ap.add_argument("--title", help="save title id (default: from the game's param.sfo, else CUSA12776)")
    ap.add_argument("--game", help="game folder for --title (default: the launcher's, from <data>/launcher.json)")
    ap.add_argument("--user", default="1")
    ap.add_argument("--apply", action="store_true", help="write (default: only show the plan)")
    a = ap.parse_args()
    a.title = a.title or save_title(a.game, a.data)

    sfo_path = os.path.join(a.export, "sce_sys", "param.sfo")
    sfo = read_sfo(sfo_path) if os.path.isfile(sfo_path) else {}
    slot = a.slot or sfo.get("SAVEDATA_DIRECTORY")
    if not slot or any(c in slot for c in "/\\") or slot.startswith("."):
        raise SystemExit("slot name missing or invalid: pass --slot")
    files = sorted(n for n in os.listdir(a.export) if n != "sce_sys" and os.path.isfile(os.path.join(a.export, n)))
    if not files:
        raise SystemExit(f"{a.export}: no save files")
    if sfo.get("TITLE_ID") and sfo["TITLE_ID"] != a.title:
        print(f"WARNING: param.sfo TITLE_ID {sfo['TITLE_ID']} differs from {a.title}")

    base = os.path.join(a.data, "user", "savedata", a.user, a.title)
    host, meta = os.path.join(base, slot), os.path.join(base, slot + ".sce_sys")
    icon = os.path.join(a.export, "sce_sys", "icon0.png")
    print(f"source: {a.export} ({len(files)} files, param.sfo {'found' if sfo else 'MISSING'})")
    print(f"title: {sfo.get('MAINTITLE', '')!r} / {sfo.get('SUBTITLE', '')!r}")
    print(f"target: {host} (+ {os.path.basename(meta)})")
    exists = os.path.isdir(host) or os.path.isdir(meta)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = os.path.join(a.data, "save-backups", f"{slot}-import-{stamp}")
    print(f"existing slot: {'yes, backup to ' + backup if exists else 'no'}")
    if not a.apply:
        print("dry run: nothing written (add --apply)")
        return
    if game_running():
        raise SystemExit("the game is running (bb-probe.exe): close it first")

    if exists:
        for src in (host, meta):
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(backup, os.path.basename(src)))
        print(f"backup written: {backup}")
    os.makedirs(host, exist_ok=True)
    os.makedirs(meta, exist_ok=True)
    for name in files:
        shutil.copy2(os.path.join(a.export, name), os.path.join(host, name))
    if os.path.isfile(icon):
        shutil.copy2(icon, os.path.join(meta, "icon0.png"))
    enc = lambda s, n: s.encode("utf-8")[:n - 1]
    param = PARAM.pack(enc(sfo.get("MAINTITLE", ""), 128), enc(sfo.get("SUBTITLE", ""), 128),
                       enc(sfo.get("DETAIL", ""), 1024), sfo.get("SAVEDATA_LIST_PARAM", 0), 0,
                       int(datetime.datetime.now().timestamp()), b"")
    with open(os.path.join(meta, "param.bin"), "wb") as f:
        f.write(param)

    bad = [n for n in files if sha(os.path.join(a.export, n)) != sha(os.path.join(host, n))]
    extra = sorted(set(os.listdir(host)) - set(files))
    if bad:
        raise SystemExit(f"VERIFY FAILED: {bad}; restore from {backup if exists else '(no backup: new slot)'}")
    print(f"imported and verified {len(files)} files into slot {slot}")
    if extra:
        print(f"note: files in the slot not in the export (left as they were): {extra}")


if __name__ == "__main__":
    sys.exit(main())

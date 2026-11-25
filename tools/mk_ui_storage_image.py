"""Build the `storage` partition image (littlefs) that carries the web app, from a release kit zip.

    python mk_ui_storage_image.py <rbgrid-ui_vX.zip> <out.bin> [--size 0x500000]

The hub serves /storage/ui/cur/<stored> (rbwebserver_ui.c) and reads the kit version from
/storage/ui/cur/ui-manifest.json, so the image holds exactly that tree. Every file is checked against the
kit manifest (size + sha256) before it goes in. littlefs settings follow the firmware's esp_littlefs config
(sdkconfig CONFIG_LITTLEFS_*): block 4096, read/prog 128, lookahead 128, name_max 64, disk version 2.1.
Flash the image at the `storage` offset (0xA20000 in partitions.csv). Needs littlefs-python >= 0.7.1.
"""
import argparse
import hashlib
import json
import sys
import zipfile

from littlefs import LittleFS

BLOCK = 4096
CFG = dict(block_size=BLOCK, read_size=128, prog_size=128, lookahead_size=128, name_max=64,
           disk_version=0x00020001)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kit")
    ap.add_argument("out")
    ap.add_argument("--size", default="0x500000", help="storage partition size (partitions.csv)")
    a = ap.parse_args()
    size = int(a.size, 0)
    if size % BLOCK:
        sys.exit("size is not a multiple of the block size")

    z = zipfile.ZipFile(a.kit)
    names = z.namelist()
    mname = next((n for n in names if n.endswith("ui-manifest.json")), None)
    if not mname:
        sys.exit("no ui-manifest.json in the kit")
    prefix = mname[: -len("ui-manifest.json")]
    mraw = z.read(mname)
    man = json.loads(mraw)
    files = man.get("files") or []
    if not files:
        sys.exit("kit manifest lists no files")

    fs = LittleFS(block_count=size // BLOCK, **CFG)
    for d in ("ui", "ui/cur"):
        fs.mkdir(d)
    made = {"ui", "ui/cur"}
    total = 0
    for f in files:
        stored = f["stored"]
        if stored.startswith("/") or ".." in stored:
            sys.exit(f"bad path in manifest: {stored}")
        cand = [prefix + stored, prefix + "kit/" + stored, "kit/" + stored]
        zn = next((c for c in cand if c in names), None)
        if zn is None:
            sys.exit(f"{stored}: not in the kit zip")
        data = z.read(zn)
        if len(data) != f["size"] or hashlib.sha256(data).hexdigest() != f["sha256"]:
            sys.exit(f"{stored}: size/sha256 differ from the kit manifest")
        parts = ("ui/cur/" + stored).split("/")
        for i in range(2, len(parts)):
            d = "/".join(parts[:i])
            if d not in made:
                fs.mkdir(d)
                made.add(d)
        with fs.open("ui/cur/" + stored, "wb") as fh:
            fh.write(data)
        total += len(data)
    with fs.open("ui/cur/ui-manifest.json", "wb") as fh:
        fh.write(mraw)

    img = bytes(fs.context.buffer)
    if len(img) != size:
        sys.exit(f"image is {len(img)} B, expected {size}")
    open(a.out, "wb").write(img)

    # read back with a fresh instance, as the hub would mount it
    rd = LittleFS(block_count=size // BLOCK, mount=False, **CFG)
    rd.context.buffer[:] = img
    rd.mount()
    for f in files:
        with rd.open("ui/cur/" + f["stored"], "rb") as fh:
            if hashlib.sha256(fh.read()).hexdigest() != f["sha256"]:
                sys.exit(f"read-back mismatch: {f['stored']}")
    print(f"ui {man.get('ui_version') or man.get('version')}: {len(files)} files, {total} B, image {size} B "
          f"sha256 {hashlib.sha256(img).hexdigest()}; read-back OK")


if __name__ == "__main__":
    main()

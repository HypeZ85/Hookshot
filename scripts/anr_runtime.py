"""Import a user-downloaded ANR FG package into a local Hookshot game. No downloads or proxy DLL injection.

python -I scripts/anr_runtime.py --package <official full/addon ZIP> --game <game folder>
The runtime is external software: obtain it from Apex's release page and review its terms there.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import zipfile

VERSION = "FG-V.B15"
RELEASE = "https://github.com/apex-compute/ANR/releases/tag/" + VERSION
# Published by Apex in that release's SHA256SUMS.txt. The complete package is checked before extraction.
PACKAGE_SHA256 = {
    "45969be1cf9916f404c5e7e6999a494619ef167a7fdb7393b2fa3136e9b7160a": "full",
    "717e93b3e63cb08dbaf6240cd585bae94d498c2749d8603d800d17ad97ef9e41": "addon",
}
PROVIDER_SHA256 = "ae7424b87d13a0e3a9a99dd02d1ea35b9de96c2caa9cd4300d73bac5bb0cf4e8"

def digest(path):
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            result.update(block)
    return result.hexdigest()

def is_link(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)  # Windows reparse point

def install(package, game, *, portable=False):
    package, game = Path(package).resolve(), Path(game).resolve()
    if not ((game / "hookshot.exe").is_file() or (game / "bin/hookshot.exe").is_file()):
        raise ValueError("Choose a built Hookshot game folder, with hookshot.exe or bin/hookshot.exe.")
    sha = digest(package)
    if sha not in PACKAGE_SHA256:
        raise ValueError("This is not a supported, unmodified ANR FG V.B15 full/addon ZIP. Obtain it from " + RELEASE)
    parent = game
    target = game / "anr-fg"   # the external-provider directory used by the ANR host in PR #2958
    if is_link(target):
        raise ValueError("The game's anr-fg folder must not be a link.")
    # Never overwrite a user's runtime/configuration or follow an existing target link.
    if target.exists() or is_link(target):
        raise ValueError(str(target) + " already exists; nothing was replaced.")
    with tempfile.TemporaryDirectory(prefix=".anr-import-", dir=parent) as tmp:
        stage = Path(tmp) / "runtime"
        stage.mkdir()
        seen = set()
        with zipfile.ZipFile(package) as archive:
            for member in archive.infolist():
                if member.is_dir():
                    continue
                parts = PurePosixPath(member.filename).parts
                if "dlssg" not in parts:
                    continue
                relative = PurePosixPath(*parts[parts.index("dlssg") + 1:])
                if not relative.parts or any(p in (".", "..") or ":" in p or "\\" in p for p in relative.parts):
                    raise ValueError("Unsafe runtime member in package: " + member.filename)
                key = str(relative).casefold()
                if key in seen:
                    raise ValueError("Duplicate runtime member in package: " + member.filename)
                seen.add(key)
                dest = stage.joinpath(*relative.parts)
                dest.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as src, open(dest, "xb") as dst:
                    shutil.copyfileobj(src, dst)
        provider = stage / "nvngx_dlssg.dll"
        if not provider.is_file() or digest(provider) != PROVIDER_SHA256:
            raise ValueError("The ANR provider DLL does not match the supported release.")
        for name in ("adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
                     "programs/fg-lite-any-portable-srgb8/pack.nrx", "path_table.json", "size_table.json", "nr_fg.ini"):
            if not (stage / name).is_file():
                raise ValueError("Incomplete runtime package: missing " + name)
        config = stage / "nr_fg.ini"
        original = config.read_bytes()
        if portable:
            (stage / "nr_fg.ini.upstream").write_bytes(original)
            text = original.decode("utf-8-sig")
            text, count = re.subn(r"(?m)^\s*path\s*=.*$", "path = portable", text)
            if count != 1:
                raise ValueError("Expected exactly one ANR kernel path setting.")
            config.write_text(text, encoding="utf-8", newline="\n")
        manifest = {"version": VERSION, "source": RELEASE, "package_sha256": sha,
                    "provider_sha256": PROVIDER_SHA256, "kernel_path": "portable" if portable else "upstream"}
        (stage / "hookshot_anr.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        # A complete installation becomes visible at once. Target is new, so no existing user files are replaced.
        stage.rename(target)
    return target

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--portable", action="store_true", help="explicitly select the portable kernel path; keep the upstream config by default")
    args = parser.parse_args()
    try:
        target = install(args.package, args.game, portable=args.portable)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, "ANR import: " + str(error) + "\n")
    print("ANR FG imported to " + str(target))
    print("Requires an ANR-enabled Hookshot build (PR #2958 or later). Nothing was downloaded or launched.")

if __name__ == "__main__":
    main()

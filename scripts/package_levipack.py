import argparse
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[1]


def package(library: Path, output: Path) -> None:
    listing = json.loads((ROOT / "levimod.json").read_text(encoding="utf-8"))
    info = listing["info"]
    version = listing["version"]
    if f"Immersive Storms {version}\0".encode() not in library.read_bytes():
        raise ValueError("The compiled version differs from levimod.json")
    files = {
        library.name: library,
        "icon.png": ROOT / info["icon"],
        "LICENSE": ROOT / "LICENSE",
        "biomes.example.cfg": ROOT / "assets" / "biomes.example.cfg",
    }
    manifest = {
        "type": "preload-native",
        "name": info["name"],
        "description": info["description"],
        "author": info["author"],
        "version": version,
        "entry": library.name,
        "icon": "icon.png",
        "overwrite_files": [name for name in files if name != library.name],
        "overwrite_folders": [],
    }
    temporary = output.with_suffix(".tmp")
    with ZipFile(temporary, "w", ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        for name, path in files.items():
            archive.write(path, name)
    with ZipFile(temporary) as archive:
        if invalid := archive.testzip():
            raise ValueError(f"Bad LeviPack entry {invalid}")
    temporary.replace(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("library", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    package(args.library, args.output)

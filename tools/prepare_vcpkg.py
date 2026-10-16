"""Create an FFmpeg overlay from the pinned vcpkg checkout, with NCO's AMF timeout fix."""
import argparse
import json
from pathlib import Path
import shutil


def prepare(vcpkg_root: Path, overlay_root: Path) -> Path:
    source = (vcpkg_root / "ports" / "ffmpeg").resolve()
    target = (overlay_root / "ffmpeg").resolve()
    if target == source or source in target.parents or target in source.parents:
        raise ValueError("The overlay must be outside the original FFmpeg port")
    metadata = json.loads((source / "vcpkg.json").read_text(encoding="utf-8"))
    if metadata.get("version") != "9.0.2":
        raise ValueError("NCO's AMF patch requires the pinned FFmpeg 9.0.2 port")
    portfile = (source / "portfile.cmake").read_text(encoding="utf-8")
    marker = "    PATCHES\n"
    if portfile.count(marker) != 1:
        raise ValueError("Cannot locate the pinned FFmpeg patch list")
    patch = Path(__file__).resolve().parents[1] / "patches" / "ffmpeg-amf-timeout.patch"
    if not patch.is_file():
        raise FileNotFoundError(patch)
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)
    shutil.copyfile(patch, target / "nco-amf-timeout.patch")
    (target / "portfile.cmake").write_text(
        portfile.replace(marker, marker + "        nco-amf-timeout.patch\n"), encoding="utf-8"
    )
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vcpkg", type=Path)
    parser.add_argument("overlay", type=Path)
    args = parser.parse_args()
    print(prepare(args.vcpkg, args.overlay))

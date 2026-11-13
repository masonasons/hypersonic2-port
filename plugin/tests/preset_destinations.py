"""Exercise export into disposable destinations using an existing HS2 library."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exporter", type=Path)
    parser.add_argument("content", type=Path)
    parser.add_argument("scratch", type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="preset-roots-", dir=args.scratch) as folder:
        root = Path(folder)
        shared = root / "Shared AU and VST3"
        nks = root / "Komplete Kontrol"
        new_nks = root / "New Komplete Kontrol"

        def export(destination):
            subprocess.run(
                [str(args.exporter), "--content", str(args.content), "--vst3", str(shared),
                 "--au", str(shared), "--nks", str(destination), "--no-previews"],
                check=True, timeout=180,
            )

        def receipt(directory, extension):
            data = json.loads((directory / f".hs2port-{extension}-presets.json").read_text(encoding="utf-8"))
            assert data["product"] == "HS2 Port"
            assert data["files"]
            assert all((directory / name).is_file() for name in data["files"])
            return data["files"]

        export(nks)
        vst = receipt(shared, "vstpreset")
        au = receipt(shared, "aupreset")
        kk = receipt(nks, "nksf")
        owned = shared / vst[0]
        custom = owned.with_stem(owned.stem + " (2)")
        custom.write_bytes(b"user preset")
        old_nks_file = nks / kk[0]
        before = old_nks_file.read_bytes()
        colliding = new_nks / kk[0]
        colliding.parent.mkdir(parents=True)
        colliding.write_bytes(b"user NKS preset")

        export(new_nks)
        assert old_nks_file.read_bytes() == before
        assert custom.read_bytes() == b"user preset"
        assert receipt(shared, "vstpreset") == vst
        assert receipt(shared, "aupreset") == au
        assert colliding.read_bytes() == b"user NKS preset"
        assert len(receipt(new_nks, "nksf")) == len(kk)
        assert kk[0] not in receipt(new_nks, "nksf")
        print(f"Verified {len(vst)} VST3, {len(au)} AU and {len(kk)} NKS paths, receipts and reinstallation.")


if __name__ == "__main__":
    main()

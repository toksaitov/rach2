"""Prepare pinned, prebuilt WebGPU assets under this clone's build directory."""
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import urllib.request


def main():
    """Check the downloaded archives and publish just the JavaScript used by the cases."""
    here = Path(__file__).resolve().parent
    tree = here.parents[2]
    cache = tree / "build/cache/browser-acceptance"
    cache.mkdir(parents=True, exist_ok=True)
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    part = output.with_suffix(".tmp")
    try:
        with tarfile.open(part, "w:gz") as bundle:
            for line in (here / "sources").read_text().splitlines():
                if not line or line.startswith("#"):
                    continue
                name, url, digest = line.split()
                archive = cache / (digest + ".tar.gz")
                pending = archive.with_suffix(".part")
                try:
                    if not archive.exists():
                        print("browser-assets: fetching " + name, flush=True)
                        urllib.request.urlretrieve(url, pending)
                        assert hashlib.sha256(pending.read_bytes()).hexdigest() == digest, name
                        pending.replace(archive)
                    assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest, name
                finally:
                    pending.unlink(missing_ok=True)
                with tarfile.open(archive) as source:
                    for member in source:
                        if not member.isfile():
                            continue
                        rel = member.name.split("/", 1)[-1]
                        if name == "cts":
                            if not rel.startswith("out/") or not rel.endswith(".js"):
                                continue
                            rel = rel[4:]
                        elif not (rel in ("js/iframe-helper.js",) or
                                  rel.startswith(("sample/helloTriangle/", "sample/helloTriangleMSAA/"))):
                            continue
                        if ".." in Path(rel).parts:
                            raise ValueError(rel)
                        member.name = "browser/assets/" + name + "/" + rel
                        bundle.addfile(member, source.extractfile(member))
            data = (here / "sources").read_bytes()
            member = tarfile.TarInfo("browser/assets/sources")
            member.size = len(data)
            bundle.addfile(member, io.BytesIO(data))
            for name in ("browser", "lenovo", "runners", "guest-env.sh", "ref/debian"):
                bundle.add(here.parent / name, arcname=name,
                           filter=lambda m: None if "__pycache__" in m.name else m)
            bundle.add(tree / "build/tests/cases/browser/seat.so", arcname="browser/seat.so")
        part.replace(output)
    finally:
        part.unlink(missing_ok=True)


if __name__ == "__main__":
    main()

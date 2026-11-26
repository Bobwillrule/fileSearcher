"""Fetch pinned Qt source archives and collect their notices without building Qt.

Sources stay in ignored .tools. No document contents or credentials are sent.
The conservative notice collection includes unused code; it is not a legal audit.
"""
import hashlib
import json
import pathlib
import tarfile
import urllib.request
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / "tools/release-sources.lock.json").read_text(encoding="utf-8"))
sources = ROOT / ".tools/release-sources"
notices = ROOT / ".tools/release-notices"
sources.mkdir(parents=True, exist_ok=True)
notices.mkdir(parents=True, exist_ok=True)
records = []
for entry in lock["archives"]:
    name = entry.get("archive", f'{entry["module"]}-everywhere-src-{lock["version"]}.tar.xz')
    archive = sources / name
    url = entry.get("git", lock["origin"] + name)
    if not archive.exists() and entry.get("git"):
        checkout = ROOT / ".tools" / ("release-" + entry["module"])
        subprocess.run(["git", "init", str(checkout)], check=True)
        subprocess.run(["git", "-C", str(checkout), "fetch", "--depth", "1", url, entry["revision"]], check=True)
        revision = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "FETCH_HEAD"], text=True).strip()
        if revision != entry["revision"]:
            raise RuntimeError("Unexpected source revision")
        subprocess.run(["git", "-C", str(checkout), "archive", "--format=tar.gz", "--prefix=qtdeclarative-2ec235a2/", "--output=" + str(archive), "FETCH_HEAD"], check=True)
    elif not archive.exists():
        print("Downloading", name, flush=True)
        partial = archive.with_suffix(archive.suffix + ".partial")
        with urllib.request.urlopen(url, timeout=60) as response, partial.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
        if hashlib.file_digest(partial.open("rb"), "sha256").hexdigest() != entry["sha256"]:
            raise RuntimeError("Source checksum mismatch: " + name)
        partial.replace(archive)
    digest = hashlib.file_digest(archive.open("rb"), "sha256").hexdigest()
    if digest != entry["sha256"]:
        raise RuntimeError("Source checksum mismatch: " + name)
    count, revision = 0, entry.get("revision", "")
    target = notices / (entry["module"] + ("-exact" if entry.get("git") else ""))
    target.mkdir(exist_ok=True)
    with tarfile.open(archive, "r|*") as tar:
        for member in tar:
            path = pathlib.PurePosixPath(member.name)
            if not member.isfile() or path.is_absolute() or ".." in path.parts:
                continue
            basename = path.name.lower()
            wanted = (any(word in basename for word in ("license", "copying", "copyright", "notice"))
                      or "licenses" in [part.lower() for part in path.parts]
                      or basename in ("qt_attribution.json", ".tag", ".gitmodules", "ftl.txt", "readme.chromium"))
            if not wanted or member.size > 8 * 1024 * 1024:
                continue
            data = tar.extractfile(member).read()
            relative = pathlib.Path(*path.parts[1:])
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            # Git archive expands .tag from an older release tag in this SDK commit.
            # For a git input, the fetched commit and pinned archive digest identify it.
            if not entry.get("git") and basename == ".tag" and len(path.parts) == 2:
                revision = data.decode("utf-8", errors="replace").strip()
            count += 1
    records.append({"module": entry["module"], "archive": name, "url": url,
                    "sha256": digest, "bytes": archive.stat().st_size,
                    "source_revision": revision, "notice_files": count, "notice_directory": target.name})
    print(entry["module"], "verified;", count, "notice files; revision", revision, flush=True)
sbom = ROOT / ".tools/qt/6.8.3/msvc2022_64/sbom"
for record in records:
    module = "qtpdf" if record["module"] == "qtwebengine" else record["module"]
    inventory = json.loads((sbom / (module + "-6.8.3.spdx.json")).read_text(encoding="utf-8"))
    package = next(p for p in inventory["packages"] if p["name"] == module)
    binary_revision = package["downloadLocation"].split("@")[-1]
    if binary_revision != record["source_revision"]:
        raise RuntimeError("Source revision does not match installed binary: " + module)
    record["sdk_revision_matches"] = True
(notices / "source-inventory.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")

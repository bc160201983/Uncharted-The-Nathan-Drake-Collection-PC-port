"""Launcher updates from the project's GitHub releases
(bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port).

The package's VERSION file (written by tools/package_windows.sh) names the installed version; a
copy run from the source tree has none and is never updated. An update:
  1. latest_release(): the newest release with a newer version and an Uncharted-windows.zip
     asset whose SHA-256 GitHub publishes (the asset's "digest"),
  2. download(): the zip into update\\, checked against that size and SHA-256,
  3. stage(): unpacked to update\\staged and checked (the files the package needs, its VERSION),
  4. start_apply(): the new version's own Python runs its tools\\apply_update.py, which waits for
     this launcher to close, swaps the program folders (rolling back on any error) and starts
     the new launcher. Saves, settings, logs and the shader cache (analysis\\) are not touched.
UNCHARTED_UPDATE_API points the check at another releases list (tests; http only on 127.0.0.1).
"""
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

REPOSITORY = "bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port"
RELEASES_PAGE = f"https://github.com/{REPOSITORY}/releases"
API = f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=20"
ASSET = "Uncharted-windows.zip"
# What a package must hold to be installed over this one.
REQUIRED = ("Uncharted Launcher.exe", "VERSION", "python/pythonw.exe", "launcher/uncharted_launcher.pyw",
            "launcher/updater.py", "tools/apply_update.py", "tools/probe_uncharted.py",
            "gow3_pc/out/gow3-probe.exe")


class UpdateError(Exception):
    pass


def parse_version(text):
    """'v0.1.10' / '0.1.10' -> (0, 1, 10); None if it is not a version."""
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", (text or "").strip())
    return tuple(int(part) for part in match.groups()) if match else None


def installed_version(root):
    """The package's version, or None for a copy run from the source tree."""
    try:
        return parse_version((Path(root) / "VERSION").read_text(encoding="ascii"))
    except OSError:
        return None


def version_text(version):
    return ".".join(str(part) for part in version)


def api_url():
    return os.environ.get("UNCHARTED_UPDATE_API") or API


def allowed_url(url):
    parts = urllib.parse.urlsplit(url)
    return parts.scheme == "https" or (parts.scheme == "http" and parts.hostname == "127.0.0.1"
                                       and bool(os.environ.get("UNCHARTED_UPDATE_API")))


def fetch_json(url, timeout=15):
    if not allowed_url(url):
        raise UpdateError(f"Refusing a non-HTTPS address: {url}")
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                   "User-Agent": "Uncharted-Launcher"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def latest_release(current, timeout=15):
    """The newest release newer than `current` with an installable package, as a dict (version,
    tag, page, notes, url, size, sha256), or None. Pre-releases count: every release so far is a
    beta."""
    best = None
    for release in fetch_json(api_url(), timeout):
        version = parse_version(release.get("tag_name"))
        if release.get("draft") or not version or version <= current:
            continue
        if best and version <= best["version"]:
            continue
        asset = next((a for a in release.get("assets", []) if a.get("name") == ASSET), None)
        digest = (asset or {}).get("digest") or ""
        if not asset or not digest.startswith("sha256:"):
            continue
        best = {"version": version, "tag": release["tag_name"], "page": release.get("html_url", RELEASES_PAGE),
                "notes": release.get("body") or "", "url": asset["browser_download_url"],
                "size": int(asset.get("size") or 0), "sha256": digest.split(":", 1)[1].lower()}
    return best


def update_dir(root):
    return Path(root) / "update"


def download(release, root, progress=None, cancelled=None, attempts=8, timeout=60, pause=2.0):
    """Downloads the release's zip into update\\ and checks its size and SHA-256. A connection that
    stalls or breaks (slow or busy networks: "The read operation timed out", #149/#150) is opened
    again and the download goes on where it stopped (HTTP Range; GitHub's file host supports it),
    up to `attempts` times in a row without progress."""
    if not allowed_url(release["url"]):
        raise UpdateError(f"Refusing a non-HTTPS download: {release['url']}")
    folder = update_dir(root)
    folder.mkdir(exist_ok=True)
    target = folder / ASSET
    partial = folder / (ASSET + ".part")
    digest = hashlib.sha256()
    done = 0
    failures = 0
    size = release["size"]
    with partial.open("wb") as out:
        while not size or done < size:
            headers = {"User-Agent": "Uncharted-Launcher"}
            if done:
                headers["Range"] = f"bytes={done}-"
            request = urllib.request.Request(release["url"], headers=headers)
            start = done
            try:
                # Each try asks github.com again: its redirect to the file host is signed and expires.
                with urllib.request.urlopen(request, timeout=timeout) as response:
                    if done and response.status != 206:  # the range was ignored: from the start
                        out.seek(0)
                        out.truncate()
                        digest, done = hashlib.sha256(), 0
                    while True:
                        if cancelled and cancelled():
                            raise UpdateError("Cancelled.")
                        chunk = response.read(1 << 16)
                        if not chunk:
                            break
                        out.write(chunk)
                        digest.update(chunk)
                        done += len(chunk)
                        if progress:
                            progress(done, size)
                if not size:
                    break
                if done < size:
                    raise ConnectionError(f"the connection closed after {done} of {size} bytes")
            except (OSError, http.client.HTTPException) as error:  # timeouts, resets, URLError, HTTPError
                failures = 0 if done - start >= (1 << 20) else failures + 1
                if failures >= attempts:
                    raise UpdateError(
                        f"The download from GitHub keeps stopping ({error or error.__class__.__name__}).\n\n"
                        "Try UPDATE again later, or download Uncharted-windows.zip from the releases page and "
                        "extract it over this folder (saves and settings in analysis\\ are kept).") from error
                time.sleep(min(pause * max(failures, 1), 15.0))
    if release["size"] and done != release["size"]:
        partial.unlink(missing_ok=True)
        raise UpdateError(f"The download is incomplete ({done} of {release['size']} bytes).")
    if digest.hexdigest() != release["sha256"]:
        partial.unlink(missing_ok=True)
        raise UpdateError("The download does not match the release's SHA-256: not installed.")
    partial.replace(target)
    return target


def stage(archive, root, version):
    """Unpacks the package to update\\staged and checks it; returns that folder."""
    staged = update_dir(root) / "staged"
    shutil.rmtree(staged, ignore_errors=True)
    staged.mkdir(parents=True)
    with zipfile.ZipFile(archive) as package:
        names = [n for n in package.namelist() if n and not n.endswith("/")]
        # Compress-Archive puts everything under one folder (Uncharted-windows/).
        tops = {n.replace("\\", "/").split("/", 1)[0] for n in names}
        prefix = tops.pop() + "/" if len(tops) == 1 and all("/" in n.replace("\\", "/") for n in names) else ""
        base = staged.resolve()
        for info in package.infolist():
            name = info.filename.replace("\\", "/")
            if name.endswith("/") or not name.startswith(prefix):
                continue
            target = (staged / name[len(prefix):]).resolve()
            if base not in target.parents:
                raise UpdateError(f"The package holds a path outside its folder: {name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(info) as source, target.open("wb") as out:
                shutil.copyfileobj(source, out, 1 << 20)
    missing = [path for path in REQUIRED if not (staged / path).is_file()]
    if missing:
        raise UpdateError("The package is incomplete: " + ", ".join(missing))
    if installed_version(staged) != version:
        raise UpdateError("The package's VERSION does not match the release.")
    return staged


def start_apply(root, staged):
    """Starts the new version's installer, which waits for this process to exit."""
    python = staged / "python" / "pythonw.exe"
    script = staged / "tools" / "apply_update.py"
    flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    subprocess.Popen([str(python), "-I", str(script), "--root", str(root), "--wait-pid", str(os.getpid())],
                     cwd=str(staged), creationflags=flags, close_fds=True)


def cleanup(root):
    """Removes what an applied or abandoned update left (best effort: files may still be in use)."""
    folder = update_dir(root)
    if not folder.exists():
        return
    for name in ("old", "staged", ASSET, ASSET + ".part"):
        path = folder / name
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
    try:
        folder.rmdir()
    except OSError:
        pass


if __name__ == "__main__":  # python launcher/updater.py: what the launcher would find
    here = Path(__file__).resolve().parent.parent
    current = installed_version(here) or (0, 0, 0)
    print("installed:", version_text(current) if installed_version(here) else "source tree")
    found = latest_release(current)
    print("newer release:", found and {k: v for k, v in found.items() if k != "notes"})
    sys.exit(0)

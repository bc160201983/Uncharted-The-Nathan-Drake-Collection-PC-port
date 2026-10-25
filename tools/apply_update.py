"""Installs a staged launcher update (launcher/updater.py). Runs with the NEW version's Python from
update\\staged, after the old launcher has closed:
  1. waits for the launcher (--wait-pid) to exit,
  2. moves each program item (everything in the package except analysis\\) to update\\old and
     copies the new one in its place; the runtime's defaults file in analysis\\uncharted-user is
     replaced too, nothing else there (saves, settings, logs, shader cache),
  3. on any error puts every moved item back, so the old version keeps working,
  4. moves back into the new folders everything of the old ones that the package does not
     install: the player's own files (ReShade's ReShade.ini, presets and reshade-shaders\\ next
     to Uncharted.exe, a game folder put inside gow3_pc\\, notes). Only files the previous package
     installed itself (installed-files.txt) and the new one no longer has stay behind (#134),
  5. starts "Uncharted Launcher.exe" (the new launcher removes update\\ when it starts).
usage: apply_update.py --root <package folder> --wait-pid <launcher pid> [--no-restart] [--quiet]
"""
import argparse
import ctypes
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEFAULTS = ("analysis/uncharted-user/disabled-optimizations.txt",)
KEEP = {"analysis", "update"}
# The files the installed package put there (relative, "/"), written after each update: the next
# update leaves out the ones its package dropped and carries over everything else.
MANIFEST = "installed-files.txt"
TITLE = "Uncharted: The Nathan Drake Collection - Launcher by BilalReloaded"


def wait_for_exit(pid, seconds):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if not handle:
        return True  # already gone
    try:
        return kernel32.WaitForSingleObject(handle, int(seconds * 1000)) == 0
    finally:
        kernel32.CloseHandle(handle)


def retry(action, seconds=15):
    """Files of a process that just exited can stay locked for a moment (antivirus scans too)."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return action()
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.5)


def copy_item(source, target):
    if source.is_dir():
        shutil.copytree(source, target)
    else:
        shutil.copy2(source, target)


def remove_item(path):
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def package_files(staged):
    """The new package's program files (relative, "/"), as installed into the root."""
    return sorted(p.relative_to(staged).as_posix() for p in staged.rglob("*")
                  if p.is_file() and p.relative_to(staged).parts[0] not in KEEP)


def read_manifest(root):
    try:
        return {line.strip() for line in (root / MANIFEST).read_text(encoding="utf-8").splitlines() if line.strip()}
    except OSError:
        return set()  # 0.5.0 and older wrote none: everything not in the new package is carried over


def carry_over(old, new, relative, dropped, kept):
    """Moves what `old` (the previous version of a program folder) holds and `new` does not back
    into `new`, folders whole (a rename on the same drive: instant, also for a game folder)."""
    for item in sorted(old.iterdir()):
        rel = f"{relative}/{item.name}" if relative else item.name
        target = new / item.name
        if item.is_dir() and not item.is_symlink() and target.is_dir():
            carry_over(item, target, rel, dropped, kept)
        elif target.exists() or target.is_symlink():
            continue  # the package's new file
        elif item.is_file() and rel in dropped:
            continue  # the previous package's own file, gone from this one
        else:
            item.rename(target)
            kept.append(rel)


def apply(root, staged, status):
    old = root / "update" / "old"
    retry(lambda: shutil.rmtree(old) if old.exists() else None)
    old.mkdir(parents=True)
    new_files = package_files(staged)
    dropped = read_manifest(root) - set(new_files)
    items = sorted(p.name for p in staged.iterdir() if p.name not in KEEP)
    moved, added = [], []
    try:
        for name in items:
            status(f"Installing {name}...")
            current = root / name
            if current.exists():
                retry(lambda: current.rename(old / name))
                moved.append(name)
            else:
                added.append(name)
            copy_item(staged / name, current)
        for relative in DEFAULTS:
            if (staged / relative).is_file():
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    shutil.copy2(target, old / Path(relative).name)
                shutil.copy2(staged / relative, target)
    except Exception:
        status("The update failed: restoring the installed version...")
        for name in moved + added:
            if (root / name).exists() and (old / name).exists() or name in added:
                retry(lambda: remove_item(root / name))
        for name in moved:
            retry(lambda: (old / name).rename(root / name))
        for relative in DEFAULTS:
            if (old / Path(relative).name).is_file():
                shutil.copy2(old / Path(relative).name, root / relative)
        raise
    # Installed. The player's files go back into the new folders; one that can't be moved stays
    # in a folder of its own beside them (update\ is removed when the launcher starts).
    kept = []
    status("Keeping your own files...")
    for name in moved:
        if (old / name).is_dir() and (root / name).is_dir():
            try:
                carry_over(old / name, root / name, name, dropped, kept)
            except OSError as error:
                rescue = root / f"update-kept-{time.strftime('%Y%m%d-%H%M%S')}"
                status(f"Some of your files in {name} could not be moved back ({error}); they are in {rescue.name}.")
                rescue.mkdir(exist_ok=True)
                retry(lambda: (old / name).rename(rescue / name))
    for rel in kept:
        print(f"Kept: {rel}", flush=True)
    try:
        (root / MANIFEST).write_text("\n".join(new_files) + "\n", encoding="utf-8")
    except OSError as error:
        print(f"Could not write {MANIFEST}: {error}", file=sys.stderr, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--wait-pid", type=int, default=0)
    parser.add_argument("--no-restart", action="store_true")
    parser.add_argument("--quiet", action="store_true", help="No window (tests)")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    staged = Path(__file__).resolve().parent.parent
    window = label = None
    if not args.quiet:
        import tkinter as tk
        window = tk.Tk()
        window.title(TITLE)
        window.configure(bg="#14110d")
        window.resizable(False, False)
        label = tk.Label(window, text="Updating the launcher...", bg="#14110d", fg="#efe6d8",
                         font=("Segoe UI", 11), padx=28, pady=22)
        label.pack()
        window.update()

    def status(text):
        print(text, flush=True)
        if label:
            label.config(text=text)
            window.update()

    ok = True
    try:
        if args.wait_pid and not wait_for_exit(args.wait_pid, 60):
            raise RuntimeError("The launcher did not close.")
        apply(root, staged, status)
        status("Update installed.")
    except Exception as error:
        ok = False
        message = (f"The update could not be installed, the previous version is kept.\n\n{error}\n\n"
                   "Close the game if it is running and try again, or download the new version "
                   "from the releases page.")
        print(message, file=sys.stderr, flush=True)
        if window:
            from tkinter import messagebox
            messagebox.showerror(TITLE, message)
    if window:
        window.destroy()
    if not args.no_restart:
        starter = root / "Uncharted Launcher.exe"
        try:
            subprocess.Popen([str(starter)], cwd=str(root), close_fds=True)
        except OSError as error:
            print(f"Could not start {starter}: {error}", file=sys.stderr, flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

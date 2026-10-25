"""The /app0 overlay an Uncharted 2 net game needs (launcher Multiplayer page, 25 Oct 2026).

Built on the player's PC from their own game files; nothing of the game is shipped.
- u2data/build/main/pak22_ps4.txt: the package manifest the game uses (the update's when it has
  one), without the "actor X" / "sound-bank X" lines whose files the Collection does not ship
  (the co-op net levels list partner skins such as mp-hero-chloe; the package loader would wait
  for them forever). Drake's model actors are added to the co-op and competitive net levels,
  so a hero spawned there has a model.
- u2data/build/main/actor27/ps4/mp-hero.pak: the multiplayer hero actor the net game loads,
  missing from the Collection; a copy of hero-default.pak stands in.
- The player's chosen character (25 Oct: an MP skin actor such as mp-hero-elena, launcher
  Multiplayer page -> UNCHARTED_NET_SKIN) is added to the same net levels. The other players'
  characters load through the game's own request: StartNetGame asks for every slot's skin actor
  and LoadLevels waits for them (runtime_coop_mp.c).
From the co-op research's analysis/triage-1022/coop-work/make_coop_overlay.py.
"""
import os
import shutil
from pathlib import Path

MAIN = Path("u2data/build/main")
ADD = {"village-wide-coop": ["hero-default", "hero-parka"], "gompa-wide-coop": ["hero-default", "hero-parka"],
       "war-wide-coop": ["hero-default", "hero-parka"]}
for _level in ("village-multi", "warzone-multi", "shambhala-multi", "gompa-multi", "ice-cave-multi",
               "treasure-multi", "train-wreck-multi", "the-fort-multi", "ruins-multi"):
    ADD[_level] = ["hero-default"]


def _first(roots, relative):
    for root in roots:
        path = Path(root) / relative
        if path.exists():
            return path
    return None


def actor_exists(roots, name):
    """Whether the game ships the actor package <name> (u2data/build/main/actor27/ps4/<name>.pak)."""
    return bool(name) and "/" not in name and "\\" not in name and \
        _first(roots, MAIN / "actor27/ps4" / (name + ".pak")) is not None


def build_overlay(roots, out, actors=()):
    """roots: the folders laid over each other as /app0, the topmost first (the update, then the
    game). out: the overlay folder. actors: more actors for the net levels (the player's
    character). Rebuilt only when the manifest is newer than it or the actors changed."""
    out = Path(out)
    manifest = _first(roots, MAIN / "pak22_ps4.txt")
    if not manifest:
        raise FileNotFoundError("u2data/build/main/pak22_ps4.txt not found in the game")
    target = out / MAIN / "pak22_ps4.txt"
    stand_in = out / MAIN / "actor27/ps4/mp-hero.pak"
    extra = sorted({a for a in actors if actor_exists(roots, a)})
    stamp = out / "net-overlay-actors.txt"
    try:
        same_actors = stamp.read_text(encoding="ascii").split() == extra
    except OSError:
        same_actors = not extra
    if target.exists() and target.stat().st_mtime >= manifest.stat().st_mtime and same_actors and \
            (stand_in.exists() or _first(roots, MAIN / "actor27/ps4/mp-hero.pak")):
        return out
    raw = manifest.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    kept, dropped, pending, level = [], 0, [], None
    for line in raw.decode("latin-1").splitlines():
        t = line.split()
        if t and t[0] in ("package-ts", "level-name") and pending:
            kept.extend("actor " + actor for actor in pending)  # after the entry's own actors
            pending = []
        if t and t[0] == "level-name":
            level = t[1] if len(t) > 1 else None
            pending = list(ADD.get(level, []))
            if level in ADD:
                pending += [a for a in extra if a not in pending]
        if len(t) >= 2 and t[0] == "actor" and not _first(roots, MAIN / "actor27/ps4" / (t[1] + ".pak")):
            dropped += 1
            continue
        if len(t) >= 2 and t[0] == "sound-bank" and not _first(roots, MAIN / "sound1/banks/ps4" / (t[1] + ".bnk")):
            dropped += 1
            continue
        kept.append(line)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(".tmp")
    temporary.write_bytes((newline.join(kept) + newline).encode("latin-1"))
    os.replace(temporary, target)
    stamp.write_text(" ".join(extra), encoding="ascii")
    if not _first(roots, MAIN / "actor27/ps4/mp-hero.pak"):
        hero = _first(roots, MAIN / "actor27/ps4/hero-default.pak")
        if hero and not stand_in.exists():
            stand_in.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(hero, stand_in)  # same drive: no copy
            except OSError:
                shutil.copy2(hero, stand_in)
    print(f"Net game overlay: {manifest} -> {target} ({dropped} missing actor/sound lines left out"
          + (f", added {' '.join(extra)}" if extra else "") + ")")
    return out

"""Launch the experimental PC lobby using the original UI and isolated saves.

The normal game executable, launcher configuration and save files are untouched.
Build first: ninja -C gow3_pc/out/gpu gow3-probe uc2mp-test uc2mp-menu-test
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys

from probe_uncharted import ROOT, find_update, game_version


# Levels each map loads with it, after the map (gow3_pc/src/uc2mp_map_levels.h, PS3 net.bin).
MAP_LEVELS = {"warzone-multi": ("sky-multi-warzone",),
              "village-multi": ("village-multi-wide", "sky-village-multi", "village-multi-tank"),
              "shambhala-multi": ("sky-sham-descent", "sham-multi-wide"),
              "treasure-multi": ("sky-village-pre",),
              "gompa-multi": ("gompa-multi-wide", "sky-gompa"),
              "train-wreck-multi": ("sky-train-wreck",),
              "ice-cave-multi": ("sky-ice-cave",)}
# Maps with PS3 online spawners (gow3_pc/src/uc2mp_extracted_spawn_catalog.h): player spawners.
SPAWNERS = {"village-multi": 41, "warzone-multi": 35, "gompa-multi": 36, "ice-cave-multi": 33,
            "shambhala-multi": 37, "the-fort-multi": 29, "train-wreck-multi": 34,
            "treasure-multi": 31}
CATALOGUE_MAPS = tuple(SPAWNERS)


def configured_game():
    config = Path(os.environ.get("APPDATA", "")) / "Uncharted Launcher/launcher.json"
    try:
        value = json.loads(config.read_text(encoding="utf-8")).get("game_dir")
        return Path(value) if value else None
    except (OSError, ValueError):
        return None


def loading_art(game, update, overlay):
    """Map cards of the multiplayer loading screen (tools/uc2mp_loading_art.py), from the
    player's net.pak into the overlay; rebuilt only when net.pak changed."""
    from uc2mp_loading_art import CARDS, NET, OUT, build
    roots = [root for root in (update, game) if root]
    source = next((root / NET for root in roots if (root / NET).is_file()), None)
    if source is None:
        return
    cards = [overlay / OUT / f"{key}.png" for key in CARDS]
    if all(card.is_file() and card.stat().st_mtime >= source.stat().st_mtime for card in cards):
        return
    build(roots, overlay)


def restore_lightmaps(ps3_pak22, game, update, overlay):
    """Overlay copies (tools/uc2mp_lightmaps.py): treasure-multi with a neutral placeholder
    lightmap, and warzone-multi with the PS3 lightmaps the Collection dropped. The PS3 archive
    path is remembered in work/local-lobby; without one Warzone keeps its black interiors.
    Rebuilt only when a source changed."""
    from uc2mp_lightmaps import NEUTRAL_LEVELS, build_neutral, level_path
    roots = [root for root in (update, game) if root]
    for level in NEUTRAL_LEVELS:  # no PS3 data needed
        source = next((root / level_path(level) for root in roots if (root / level_path(level)).is_file()), None)
        target = overlay / level_path(level)
        if source and not (target.is_file() and target.stat().st_mtime >= source.stat().st_mtime):
            build_neutral(roots, overlay, level)
    remembered = ROOT / "work/local-lobby/ps3-pak22.txt"
    if ps3_pak22:
        remembered.parent.mkdir(parents=True, exist_ok=True)
        remembered.write_text(str(ps3_pak22.resolve()), encoding="utf-8")
    elif remembered.is_file():
        ps3_pak22 = Path(remembered.read_text(encoding="utf-8").strip())
    if not ps3_pak22 or not ps3_pak22.is_file():
        print("Warzone : interieurs noirs (lightmaps absentes) ; --ps3-pak22 <pak22.psarc PS3> les restaure.",
              flush=True)
        return
    from uc2mp_lightmaps import RELATIVE, build
    source = next((root / RELATIVE for root in roots if (root / RELATIVE).is_file()), None)
    destination = overlay / RELATIVE
    if source and destination.is_file() and destination.stat().st_mtime >= max(
            source.stat().st_mtime, ps3_pak22.stat().st_mtime):
        return
    build(roots, ps3_pak22, overlay)
    print("Warzone : lightmaps PS3 restaurees.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=configured_game())
    parser.add_argument("--name", default="Joueur")
    parser.add_argument("--seconds", type=int, default=0)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--instance", help="Separate named instance for LAN testing on this PC")
    parser.add_argument("--map-test", choices=("stage-multi",) + CATALOGUE_MAPS,
                        help="Isolated retained-engine map/player diagnostic; not a multiplayer match")
    parser.add_argument("--task", metavar="NAME",
                        help="Diagnostic: start a tasks.bin game task (e.g. deathmatch, whose "
                             "TaskManagerOnline is the native online flow) instead of the menu")
    parser.add_argument("--headless", action="store_true",
                        help="Real offscreen GPU rendering for diagnostics; no game window (default 90 seconds)")
    parser.add_argument("--preload-hero", action="store_true",
                        help="Map diagnostic only: preload hero-default before native autoSpawn")
    parser.add_argument("--defer-spawn", action="store_true",
                        help="Map diagnostic only: defer hero creation until after native level requests")
    parser.add_argument("--stage-spawn", action="store_true",
                        help="Map diagnostic: place the hero on an online spawner (stage-multi: retained "
                             "records; village/warzone: catalogue extracted from the PS3 wrappers)")
    parser.add_argument("--spawn-index", type=int,
                        help="Village/Warzone: catalogue player spawner to use (default 0)")
    network = parser.add_mutually_exclusive_group()
    network.add_argument("--host", nargs="?", const="0.0.0.0", metavar="BIND_IPV4")
    network.add_argument("--connect", metavar="HOST_IPV4")
    parser.add_argument("--port", type=int, default=3233)
    parser.add_argument("--token", help="Shared lobby key: 32 hexadecimal characters")
    parser.add_argument("--ps3-pak22", type=Path,
                        help="PS3 USRDIR/build/main/pak22.psarc: restores Warzone's lightmaps (remembered)")
    args = parser.parse_args()
    if not args.game or not (args.game / "big2-ps4_Shipping.elf").is_file():
        parser.error("Indique le dossier uroot avec --game (configuration du lanceur introuvable ou invalide).")
    if args.seconds < 0:
        parser.error("--seconds doit etre positif ou nul")
    if args.headless and not args.seconds:
        args.seconds = 90
    if args.instance and not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", args.instance):
        parser.error("--instance : 1 a 32 lettres, chiffres, tirets ou underscores")
    if not 1 <= args.port <= 65535:
        parser.error("--port : 1 a 65535")
    for address in (args.host, args.connect):
        if address is not None:
            try:
                ipaddress.IPv4Address(address)
            except ipaddress.AddressValueError:
                parser.error("Le transport experimental utilise une adresse IPv4 numerique.")
    if args.connect and not args.token:
        parser.error("--connect requiert la cle --token du salon hote")
    if args.token and not re.fullmatch(r"[0-9a-fA-F]{32}", args.token):
        parser.error("--token doit contenir exactement 32 caracteres hexadecimaux")
    if args.token and int(args.token, 16) == 0:
        parser.error("--token ne peut pas etre une cle nulle")
    if args.connect and (ipaddress.IPv4Address(args.connect).is_unspecified or
                         ipaddress.IPv4Address(args.connect).is_multicast or
                         args.connect == "255.255.255.255"):
        parser.error("--connect requiert l'adresse IPv4 d'un hote")
    if args.token and not (args.host or args.connect):
        parser.error("--token requiert --host ou --connect")
    if args.map_test and (args.host or args.connect):
        parser.error("Le diagnostic de carte n'est pas encore synchronise en LAN")
    if args.defer_spawn and not args.preload_hero:
        parser.error("--defer-spawn requiert --preload-hero")
    if args.stage_spawn and (not args.defer_spawn or not args.map_test):
        parser.error("--stage-spawn requiert --map-test <carte> --preload-hero --defer-spawn")
    # The lobby picks its map later: any index valid on every map.
    spawners = SPAWNERS.get(args.map_test, min(SPAWNERS.values()))
    if args.spawn_index is not None and (not 0 <= args.spawn_index < spawners or
                                         (args.map_test and (not args.stage_spawn or args.map_test == "stage-multi"))):
        parser.error(f"--spawn-index : 0 a {spawners - 1}, pour le salon ou avec --stage-spawn sur une carte du catalogue")
    if args.preload_hero and not args.map_test:
        parser.error("--preload-hero requiert --map-test")
    if not args.name.strip() or len(args.name.encode("utf-8")) >= 32 or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in args.name):
        parser.error("Pseudo requis : moins de 32 octets UTF-8, sans caracteres de controle.")
    game = args.game.resolve()
    update = find_update(game)
    source = update or game
    if game_version(source) != "01.02":
        parser.error("Ce raccordement experimental requiert Uncharted 2 version 1.02.")
    built = ROOT / "gow3_pc/out/gpu/gow3-probe.exe"
    if not built.is_file():
        parser.error("Compile d'abord le runtime : ninja -C gow3_pc/out/gpu gow3-probe")
    work = ROOT / "work/local-lobby"
    if args.instance:
        work = work / "instances" / args.instance
    elif args.map_test:
        work = work / "instances" / ("map-" + args.map_test)
    runtime = work / "runtime/Uncharted.exe"
    runtime.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(built, runtime)
    except PermissionError:
        parser.error("Ferme la precedente fenetre du salon experimental avant de le relancer.")
    user = work / "user"
    user.mkdir(parents=True, exist_ok=True)
    settings = user / "probe-u2.ini"
    original_settings = ROOT / "analysis/uncharted-user/probe-u2.ini"
    if not settings.exists() and original_settings.is_file():
        shutil.copy2(original_settings, settings)
    overlay = work / "menu-overlay" if args.instance else ROOT / "work/mods/u2mp-local"
    subprocess.run([sys.executable, str(ROOT / "tools/uc2mp_menu.py"),
                    str(source), str(overlay), "--local-lobby"], check=True)
    restore_lightmaps(args.ps3_pak22, game, update, overlay)
    loading_art(game, update, overlay)
    if args.stage_spawn and args.map_test == "stage-multi":
        from uc2mp_stage_wrapper import build_overlay
        build_overlay(game, update, overlay)
    env = os.environ.copy()
    env.update(UNCHARTED_LOCAL_LOBBY="1", UNCHARTED_MP_MENU="1",
               UNCHARTED_MOD_DIR=str(overlay), UC2MP_PLAYER_NAME=args.name,
               UC2MP_PLAYER_UTF8=args.name.encode("utf-8").hex(),
               UNCHARTED_UPDATE=str(update) if update else "0")
    if args.map_test or args.task:
        env["UNCHARTED_LOCAL_LOBBY"] = "0"
    env.pop("UNCHARTED_UC2MP_PRELOAD_HERO", None)
    env.pop("UNCHARTED_UC2MP_DEFER_SPAWN", None)
    env.pop("UNCHARTED_UC2MP_STAGE_SPAWN", None)
    if args.preload_hero:
        env["UNCHARTED_UC2MP_PRELOAD_HERO"] = "1"
    if args.defer_spawn:
        env["UNCHARTED_UC2MP_DEFER_SPAWN"] = "1"
    env.pop("UC2MP_SPAWN_INDEX", None)
    if args.stage_spawn:
        env["UNCHARTED_UC2MP_STAGE_SPAWN"] = "1"
    if args.spawn_index is not None:
        env["UC2MP_SPAWN_INDEX"] = str(args.spawn_index)
    if not args.map_test and not args.task:
        # The lobby's "Lancer la partie" restarts the game with "level <map> player" (LoadExec);
        # the restarted game uses the map test's spawn path. The bridge stays inactive in the
        # menu (no level argument). Host and client start on different catalogue spawners.
        env.update(UNCHARTED_UC2MP_PRELOAD_HERO="1", UNCHARTED_UC2MP_DEFER_SPAWN="1",
                   UNCHARTED_UC2MP_STAGE_SPAWN="1",
                   UC2MP_SPAWN_INDEX=str(args.spawn_index if args.spawn_index is not None
                                         else (20 if args.connect else 0)))
    if args.headless:
        env.update(GOW3_HEADLESS="1", GOW3_KEYBOARD="0", GOW3_MOUSE="0")
    # Do not inherit an override that would redirect writes into normal saves.
    env.pop("UNCHARTED_CONFIG_FILE", None)
    env.pop("UNCHARTED_TEST_GPU_USER_DIR", None)
    for key in tuple(env):
        if key.startswith("UC2MP_LAN_") or key == "UC2MP_LOCAL_ID":
            del env[key]
    if args.host or args.connect:
        token = args.token or secrets.token_hex(16)
        env.update(UC2MP_LAN_TOKEN=token, UC2MP_LAN_PORT=str(args.port))
        if args.host:
            env["UC2MP_LAN_HOST"] = args.host
            print(f"Salon LAN {args.host}:{args.port} — cle : {token}", flush=True)
        else:
            env["UC2MP_LAN_CONNECT"] = args.connect
            env["UC2MP_LOCAL_ID"] = str(secrets.randbelow(2**64 - 2) + 2)
    command = [sys.executable, str(ROOT / "tools/probe_uncharted.py"),
               "--game", str(game), "--executable", "big2-ps4_Shipping.elf",
               "--analysis-root", str(work / "analysis102"), "--user-dir", str(user),
               "--runtime-executable", str(runtime), "--boot", "--seconds", str(args.seconds)]
    if args.map_test:
        command += ["--guest-arg", "eboot.bin", "--guest-arg", "ispartofuccollection",
                    "--guest-arg", "level", "--guest-arg", args.map_test]
        for level in MAP_LEVELS.get(args.map_test, ()):
            command += ["--guest-arg", "level", "--guest-arg", level]
        command += ["--guest-arg", "player"]
        print(f"Diagnostic moteur isole : {args.map_test}, creation du personnage natif.", flush=True)
        print("Ce test ne constitue pas encore une partie multijoueur.", flush=True)
    elif args.task:
        # Without "ispartofuccollection": the Collection layer skips the task argument.
        command += ["--guest-arg", "eboot.bin", "--guest-arg", "task", "--guest-arg", args.task]
        print(f"Diagnostic : tache native {args.task}.", flush=True)
    else:
        print("Salon PC experimental : profil et groupe locaux, interface d'origine.", flush=True)
        print("Salon LAN disponible avec --host/--connect. Lancer la partie (page Partie personnalisee) "
              "relance chaque jeu sur la carte choisie ; joueurs distants pas encore visibles.", flush=True)
    if args.prepare_only:
        print("Preparation terminee ; aucun jeu lance.")
        return 0
    return subprocess.call(command, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main())

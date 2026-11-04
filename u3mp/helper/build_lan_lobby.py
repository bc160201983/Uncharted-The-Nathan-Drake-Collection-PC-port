"""Build a LAN lobby test image: compile u3_lan_profiles.c and link it into the frozen
menu-shell-v1 image.

usage: python build_lan_lobby.py VERSION [--clang PATH]
  reads  u3mp/builds/menu-shell-v1/boot-linked.bin   (frozen with freeze_u3_menu_build.py)
         u3mp/work/u3_pool_init.bin                  (build_u3_pool_blob.py --source u3_pool_init.c)
  writes u3mp/builds/lan-lobby-vVERSION/{profile-helper.bin,boot-linked.bin}
and prints the image SHA-256 for the launcher's --expected-sha256.

Nothing here is game data, but the images it writes are derived from your game dump:
keep them out of the repository (builds/ and work/ are git-ignored).
"""
import argparse, hashlib, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('version')
    p.add_argument('--clang', default='C:/msys64/clang64/bin/clang.exe')
    a = p.parse_args()
    out = ROOT / 'builds' / ('lan-lobby-v' + a.version)
    out.mkdir(parents=True, exist_ok=True)
    blob_path = out / 'profile-helper.bin'
    subprocess.check_call([sys.executable, str(HERE / 'build_u3_pool_blob.py'), '--clang', a.clang,
                           '--source', str(HERE / 'u3_lan_profiles.c'), '--out', str(blob_path)])
    import build_u3_profile_probe as bp
    raw = (ROOT / 'builds' / 'menu-shell-v1' / 'boot-linked.bin').read_bytes()
    pool = (ROOT / 'work' / 'u3_pool_init.bin').read_bytes()
    built = bp.build(raw, blob_path.read_bytes(), pool)
    if isinstance(built, tuple):
        built = built[0]
    (out / 'boot-linked.bin').write_bytes(built)
    print(hashlib.sha256(built).hexdigest(), len(built))


if __name__ == '__main__':
    main()

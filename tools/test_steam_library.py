"""Tests of the launcher's Add to Steam (launcher/steam_library.py), on temporary folders only:
Steam's real files are never read or written here.

Run: python tools/test_steam_library.py
"""
import binascii
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "launcher"))
import steam_library as steam  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

GAMES = [("u1", "Uncharted: Drake's Fortune"), ("u2", "Uncharted 2: Among Thieves"),
         ("u3", "Uncharted 3: Drake's Deception")]
ALL_NAMES = [steam.COLLECTION_NAME] + [name for _, name in GAMES]


def s(key, value):
    return b"\x01" + key + b"\0" + value + b"\0"


def i(key, value):
    return b"\x02" + key + b"\0" + struct.pack("<I", value)


def m(key, *body):
    return b"\x00" + key + b"\0" + b"".join(body) + b"\x08"


# Two shortcuts as Steam writes them, plus fields Steam rarely uses (a float, a 64-bit number), a
# lower-case key from older Steam versions and a name that is not valid UTF-8: all must survive.
OTHER_A = m(b"0", i(b"appid", 0x9ABCDEF0), s(b"AppName", b"Other Game"), s(b"Exe", b'"C:\\Games\\other.exe"'),
            s(b"StartDir", b'"C:\\Games\\"'), s(b"LaunchOptions", b"-windowed"), i(b"LastPlayTime", 1700000000),
            m(b"tags", s(b"0", b"favorite"), s(b"1", b"Shooters")))
OTHER_B = m(b"1", s(b"appname", b"Caf\xe9 \xff"), s(b"exe", b'"D:\\Emu\\emu.exe"'),
            b"\x03ratio\0" + struct.pack("<f", 1.5), b"\x07big\0" + struct.pack("<Q", 2 ** 40 + 7),
            m(b"tags"))
STEAM_FILE = m(b"shortcuts", OTHER_A, OTHER_B) + b"\x08"


def entries(file):
    return [node[2] for node in steam.shortcut_list(steam.parse_vdf(Path(file).read_bytes()))]


def by_name(file):
    return {steam.field(entry, "AppName"): entry for entry in entries(file)}


class VdfTests(unittest.TestCase):
    def test_round_trip_is_byte_exact(self):
        tree = steam.parse_vdf(STEAM_FILE)
        self.assertEqual(steam.dump_vdf(tree), STEAM_FILE)
        names = [steam.field(node[2], "AppName") for node in steam.shortcut_list(tree)]
        self.assertEqual(names[0], "Other Game")
        self.assertEqual(steam.field(steam.shortcut_list(tree)[1][2], "Exe"), '"D:\\Emu\\emu.exe"')

    def test_empty_file_steam_writes(self):
        empty = b"\x00shortcuts\x00\x08\x08"
        self.assertEqual(steam.parse_vdf(empty), [[steam.MAP, "shortcuts", []]])
        self.assertEqual(steam.dump_vdf(steam.parse_vdf(empty)), empty)

    def test_truncated_or_unknown_data_is_refused(self):
        with self.assertRaises((ValueError, IndexError)):
            steam.parse_vdf(STEAM_FILE[:-5])
        with self.assertRaises(ValueError):
            steam.parse_vdf(b"\x00shortcuts\x00\x09x\x00\x08\x08")

    def test_app_id_is_the_crc_of_exe_and_name_with_the_top_bit(self):
        exe = '"C:\\Uncharted\\Uncharted Launcher.exe"'
        expected = binascii.crc32((exe + steam.COLLECTION_NAME).encode()) | 0x80000000
        self.assertEqual(steam.app_id(exe, steam.COLLECTION_NAME), expected)
        self.assertGreaterEqual(expected, 0x80000000)


class TempSteam(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="steam-test-"))
        self.addCleanup(shutil.rmtree, self.temp, True)
        # Nothing here may find the real Steam.
        patch = mock.patch.object(steam, "steam_roots", side_effect=AssertionError("real Steam looked up"))
        patch.start()
        self.addCleanup(patch.stop)
        self.port = self.temp / "Uncharted"
        (self.port / "launcher" / "assets").mkdir(parents=True)
        self.starter = self.port / "Uncharted Launcher.exe"
        self.starter.write_bytes(b"MZ")
        self.icon = self.port / "launcher" / "assets" / "uncharted.ico"
        self.account = self.temp / "Steam" / "userdata" / "12345"
        (self.account / "config").mkdir(parents=True)
        self.file = self.account / "config" / "shortcuts.vdf"
        self.file.write_bytes(STEAM_FILE)

    def targets(self, games=None, starter=None):
        return steam.launch_targets(self.port, starter or self.starter, sys.executable,
                                    self.port / "launcher" / "uncharted_launcher.pyw", games)

    def add(self, games=None, accounts=None, game_dir=None, starter=None):
        return steam.add_to_steam(self.targets(games, starter), self.icon, game_dir,
                                  accounts or [(self.account, "Tester")], ALL_NAMES)

    def assert_others_intact(self):
        tree = steam.parse_vdf(self.file.read_bytes())
        others = [node for node in steam.shortcut_list(tree)
                  if steam.field(node[2], "AppName") in ("Other Game", "Caf\udce9 \udcff")]
        self.assertEqual(len(others), 2)
        # Same bytes as in Steam's file (apart from the slot numbers, which stay 0 and 1).
        self.assertEqual(steam.dump_vdf(others)[:-1], OTHER_A + OTHER_B)


class AddTests(TempSteam):
    def test_adds_the_collection_and_keeps_every_other_shortcut(self):
        (folder, name, result), = self.add()
        self.assertEqual((result["added"], result["updated"], result["removed"]), (1, 0, 0))
        self.assert_others_intact()
        ours = by_name(self.file)[steam.COLLECTION_NAME]
        exe = f'"{self.starter}"'
        self.assertEqual(steam.field(ours, "Exe"), exe)
        self.assertEqual(steam.field(ours, "StartDir"), f'"{self.port}"')
        self.assertEqual(steam.field(ours, "LaunchOptions"), "")
        self.assertEqual(steam.field(ours, "icon"), str(self.icon))
        self.assertEqual(steam.field(ours, "appid"), steam.app_id(exe, steam.COLLECTION_NAME))
        self.assertEqual([node[1] for node in steam.shortcut_list(steam.parse_vdf(self.file.read_bytes()))],
                         ["0", "1", "2"])
        # The backup is Steam's file as it was.
        self.assertEqual(result["backup"].read_bytes(), STEAM_FILE)
        self.assertTrue(result["backup"].name.startswith("shortcuts.vdf."))

    def test_adding_again_updates_in_place_and_keeps_the_players_fields(self):
        self.add()
        tree = steam.parse_vdf(self.file.read_bytes())
        ours = steam.shortcut_list(tree)[2][2]
        steam.set_field(ours, "LastPlayTime", steam.INT32, 1234)   # as Steam would after playing
        steam.set_field(ours, "tags", steam.MAP, [[steam.STRING, "0", "Uncharted"]])
        self.file.write_bytes(steam.dump_vdf(tree))
        (_, _, result), = self.add()
        self.assertEqual((result["added"], result["updated"], result["removed"]), (0, 1, 0))
        self.assertEqual(len(entries(self.file)), 3)
        ours = by_name(self.file)[steam.COLLECTION_NAME]
        self.assertEqual(steam.field(ours, "LastPlayTime"), 1234)
        self.assertEqual(steam.field(ours, "tags"), [[steam.STRING, "0", "Uncharted"]])
        self.assert_others_intact()

    def test_switching_between_one_entry_and_one_per_game(self):
        self.add()
        (_, _, result), = self.add(GAMES)
        self.assertEqual((result["added"], result["removed"]), (3, 1))
        names = by_name(self.file)
        self.assertNotIn(steam.COLLECTION_NAME, names)
        for key, name in GAMES:
            self.assertEqual(steam.field(names[name], "LaunchOptions"), f"--play {key}")
        self.assertEqual(len(set(result["ids"])), 3)
        self.assert_others_intact()
        (_, _, result), = self.add()
        self.assertEqual((result["added"], result["removed"]), (1, 3))
        self.assertEqual(sorted(by_name(self.file)), sorted(["Other Game", "Caf\udce9 \udcff",
                                                              steam.COLLECTION_NAME]))
        self.assert_others_intact()

    def test_from_source_the_script_is_the_launch_option(self):
        missing = self.port / "no starter.exe"
        (_, _, result), = self.add(GAMES[:1], starter=missing)
        ours = by_name(self.file)[GAMES[0][1]]
        script = self.port / "launcher" / "uncharted_launcher.pyw"
        self.assertEqual(steam.field(ours, "LaunchOptions"), f'"{script}" --play u1')
        self.assertTrue(steam.field(ours, "Exe").lower().endswith(('pythonw.exe"', 'python.exe"')))

    def test_another_python_program_with_the_same_python_is_left_alone(self):
        missing = self.port / "no starter.exe"
        exe = self.targets(starter=missing)[0][1]
        tree = steam.parse_vdf(self.file.read_bytes())
        steam.shortcut_list(tree).append([steam.MAP, "2", steam.new_entry(
            steam.app_id(exe, "Tool"), "Tool", exe, '"C:\\"', '"C:\\tool.pyw"', "")])
        self.file.write_bytes(steam.dump_vdf(tree))
        self.add(starter=missing)
        self.add(GAMES, starter=missing)
        self.assertEqual(steam.field(by_name(self.file)["Tool"], "LaunchOptions"), '"C:\\tool.pyw"')

    def test_an_entry_from_a_moved_port_is_pointed_at_the_new_folder(self):
        old = '"E:\\Old\\Uncharted Launcher.exe"'
        tree = steam.parse_vdf(self.file.read_bytes())
        steam.shortcut_list(tree).append([steam.MAP, "2", steam.new_entry(
            steam.app_id(old, steam.COLLECTION_NAME), steam.COLLECTION_NAME, old, '"E:\\Old"', "", "")])
        self.file.write_bytes(steam.dump_vdf(tree))
        (_, _, result), = self.add()
        self.assertEqual((result["added"], result["updated"]), (0, 1))
        self.assertEqual(steam.field(by_name(self.file)[steam.COLLECTION_NAME], "Exe"), f'"{self.starter}"')

    def test_a_missing_or_empty_file_is_made(self):
        self.file.unlink()
        (_, _, result), = self.add()
        self.assertIsNone(result["backup"])
        self.assertEqual(list(by_name(self.file)), [steam.COLLECTION_NAME])

    def test_an_unreadable_file_stops_before_any_account_is_written(self):
        second = self.temp / "Steam" / "userdata" / "999"
        (second / "config").mkdir(parents=True)
        (second / "config" / "shortcuts.vdf").write_bytes(STEAM_FILE[:-7])
        with self.assertRaises(steam.SteamError) as caught:
            self.add(accounts=[(self.account, "A"), (second, "B")])
        self.assertEqual(caught.exception.kind, "bad_file")
        self.assertEqual(self.file.read_bytes(), STEAM_FILE)
        self.assertEqual(list(self.file.parent.glob("*" + steam.BACKUP_SUFFIX)), [])

    def test_every_account_gets_it(self):
        second = self.temp / "Steam" / "userdata" / "999"
        (second / "config").mkdir(parents=True)
        results = self.add(accounts=[(self.account, "A"), (second, "B")])
        self.assertEqual(len(results), 2)
        self.assertEqual(list(by_name(second / "config" / "shortcuts.vdf")), [steam.COLLECTION_NAME])

    def test_no_steam(self):
        with self.assertRaises(steam.SteamError) as caught:
            steam.add_to_steam(self.targets(), self.icon, None, [], ALL_NAMES)
        self.assertEqual(caught.exception.kind, "no_steam")

    def test_only_the_newest_backups_are_kept(self):
        for _ in range(steam.KEEP_BACKUPS + 3):
            self.add()
        self.assertEqual(len(list(self.file.parent.glob("*" + steam.BACKUP_SUFFIX))), steam.KEEP_BACKUPS)


class AccountTests(TempSteam):
    def test_accounts_and_their_names(self):
        steam_dir = self.temp / "Steam"
        (steam_dir / "userdata" / "0").mkdir()
        (steam_dir / "userdata" / "anonymous").mkdir()
        (steam_dir / "config").mkdir()
        (steam_dir / "config" / "loginusers.vdf").write_text(
            '"users"\n{\n\t"76561197960278073"\n\t{\n\t\t"AccountName"\t\t"nate"\n'
            '\t\t"PersonaName"\t\t"Nathan"\n\t}\n}\n', encoding="utf-8")
        (steam_dir / "userdata" / "12345" / "config").mkdir(exist_ok=True)
        found = steam.find_accounts([steam_dir, steam_dir])
        self.assertEqual([(folder.name, name) for folder, name in found], [("12345", "Nathan")])
        self.assertEqual(steam.find_accounts([self.temp / "nothing"]), [])


@unittest.skipIf(Image is None, "Pillow is needed for the pictures")
class PictureTests(TempSteam):
    def setUp(self):
        super().setUp()
        self.game = self.temp / "game"
        (self.game / "sce_sys").mkdir(parents=True)
        Image.new("RGB", (1920, 1080), (220, 205, 180)).save(self.game / "sce_sys" / "pic1.png")
        cover = Image.new("RGB", (512, 512), (225, 210, 185))
        cover.paste((30, 20, 10), (60, 270, 450, 310))   # the title's dark letters
        cover.save(self.game / "sce_sys" / "icon0.png")

    def test_pictures_from_the_game_files(self):
        pictures = steam.make_pictures(self.game)
        self.assertEqual(pictures["p"].size, steam.TALL)
        self.assertEqual(pictures[""].size, steam.WIDE)
        self.assertEqual(pictures["_hero"].size, steam.HERO)
        logo = pictures["_logo"]
        self.assertEqual((logo.mode, logo.width), ("RGBA", steam.LOGO_WIDTH))
        self.assertEqual(logo.getpixel((logo.width // 2, logo.height // 2))[3], 255)

    def test_written_per_app_id_without_replacing_the_players_own(self):
        grid = self.account / "config" / "grid"
        grid.mkdir()
        exe = self.targets()[0][1]
        appid = steam.app_id(exe, steam.COLLECTION_NAME)
        (grid / f"{appid}p.jpg").write_bytes(b"player's own")
        (_, _, result), = self.add(game_dir=self.game)
        self.assertEqual(result["pictures"], 3)
        self.assertEqual((grid / f"{appid}p.jpg").read_bytes(), b"player's own")
        self.assertFalse((grid / f"{appid}p.png").exists())
        for suffix in ("", "_hero", "_logo"):
            self.assertTrue((grid / f"{appid}{suffix}.png").is_file())

    def test_no_game_files_no_pictures(self):
        self.assertEqual(steam.make_pictures(self.temp / "nowhere"), {})


if __name__ == "__main__":
    unittest.main()

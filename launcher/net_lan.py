"""Uncharted 2 LAN games: finds games hosted on this network (the launcher's Multiplayer page).

A hosting game (gow3_pc/src/runtime_net.c) answers UDP queries on its port number. find_games()
sends the query as a broadcast (and to this PC, for two games on one PC) and collects the
answers for a moment:
  query:  b"U2NQ" + u32 version
  answer: b"U2NI", u32 version, u32 TCP port, u32 players, u32 most players, i32 net game,
          char host name[32], char title[48]   (little-endian)
Answers to our own broadcast need no firewall rule on this PC (Windows lets the replies to an
outgoing query in); the hosting PC allows the game once when Windows asks.
"""
import socket
import struct
import time

DEFAULT_PORT = 27000
VERSION = 6  # runtime_net.c NET_VERSION (4: characters in the hello; 5: snapshot anim phases; 6: NPC events)
_ANSWER = struct.Struct("<4sIIIIi32s48s")


def _text(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def _local_addresses():
    try:
        return {info[4][0] for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)}
    except OSError:
        return set()


def _broadcast_addresses():
    """255.255.255.255 plus each local IPv4 address's /24 broadcast (Windows sends the global
    broadcast out of one network adapter only)."""
    found = ["255.255.255.255"]
    for address in _local_addresses():
        parts = address.split(".")
        if len(parts) == 4 and parts[0] != "127":
            found.append(".".join(parts[:3] + ["255"]))
    return list(dict.fromkeys(found))


def find_games(port=DEFAULT_PORT, seconds=1.5, extra_hosts=("127.0.0.1",)):
    """The games answering within the time: dicts with address, port, players, most, game
    (*net-games* index, -1 unknown), host (the host's player name), title, version."""
    query = b"U2NQ" + struct.pack("<I", VERSION)
    games = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind(("", 0))
        for target in _broadcast_addresses() + list(extra_hosts):
            try:
                s.sendto(query, (target, port))
            except OSError:
                pass
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            s.settimeout(left)
            try:
                data, (address, _) = s.recvfrom(512)
            except socket.timeout:
                break
            except OSError:  # e.g. a "port unreachable" from a PC without a game
                continue
            if len(data) < _ANSWER.size or data[:4] != b"U2NI":
                continue
            magic, version, tcp_port, players, most, game, host, title = _ANSWER.unpack_from(data)
            games[(address, tcp_port)] = {
                "address": address, "port": tcp_port, "players": players, "most": most,
                "game": game, "host": _text(host), "title": _text(title), "version": version,
            }
    # A game on this PC can answer twice (to the broadcast and to 127.0.0.1): keep the network
    # address, which other PCs can use too.
    local = _local_addresses()
    ports_seen_here = {port for (address, port) in games if address in local}
    found = [g for (address, port), g in games.items()
             if not (address == "127.0.0.1" and port in ports_seen_here)]
    return sorted(found, key=lambda g: (g["title"], g["host"], g["address"]))


if __name__ == "__main__":
    import sys
    for g in find_games(int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT):
        print(f"{g['address']}:{g['port']}  {g['title'] or '(no title)'}  host {g['host']}  "
              f"{g['players']}/{g['most']} players  net game {g['game']}  v{g['version']}")

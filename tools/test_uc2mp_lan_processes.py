"""Bounded real-UDP smoke test of two independent native lobby processes.

Build uc2mp-lan-peer first. No game, graphics, remote server or firewall change.
"""
import argparse
from pathlib import Path
import re
import secrets
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def common_args(executable):
    # Reserve an ephemeral candidate; the host still checks exclusive bind.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as candidate:
        candidate.bind(("127.0.0.1", 0))
        port = candidate.getsockname()[1]
    token = secrets.token_hex(16)
    return [str(executable.resolve()), "--port", str(port), "--token", token]


def run_pair(executable, logs, prepare=False):
    common = common_args(executable)
    prefix = "prepare-" if prepare else ""
    host_path, client_path = logs / f"{prefix}host.log", logs / f"{prefix}client.log"
    host_seconds, client_seconds = (12, 10) if prepare else (6, 2)
    scenario = ["--prepare-test"] if prepare else []
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    host = client = None
    try:
        with host_path.open("wb") as host_log, client_path.open("wb") as client_log:
            host = subprocess.Popen(common + ["--host", "127.0.0.1", "--name", "Host",
                                               "--id", "1", "--seconds", str(host_seconds)] + scenario,
                                    stdout=host_log, stderr=subprocess.STDOUT, creationflags=creationflags)
            deadline = time.monotonic() + 3
            while "state=hosting" not in host_path.read_text(errors="replace"):
                if host.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"Host failed to start; see {host_path}")
                time.sleep(0.02)
            client = subprocess.Popen(common + ["--connect", "127.0.0.1", "--name", "Peer",
                                                 "--id", "2", "--seconds", str(client_seconds), "--ready"] + scenario,
                                      stdout=client_log, stderr=subprocess.STDOUT, creationflags=creationflags)
            if client.wait(timeout=client_seconds + 3) != 0 or host.wait(timeout=host_seconds + 3) != 0:
                raise RuntimeError("Native transport failed; see process logs")
        host_text = host_path.read_text(errors="replace")
        client_text = client_path.read_text(errors="replace")
        checks = {
            "distinct_processes": host.pid != client.pid,
            "client_connected": "state=connected" in client_text,
            "host_received_peer_ready": "member id=2 name=Peer ready=1" in host_text,
            "client_received_ready_snapshot": "member id=2 name=Peer ready=1" in client_text,
            "host_removed_departed_peer": "members=1" in host_text.rsplit("roster revision=", 1)[-1],
            "both_closed": all("closed socket and local lobby" in t for t in (host_text, client_text)),
        }
        if prepare:
            results = [re.search(r"prepare-test result=PASS id=([1-9][0-9]*) returned-lobby members=2 ready=0", t)
                       for t in (host_text, client_text)]
            attempt_id = results[0].group(1) if results[0] else None
            checks["same_attempt_returned_to_lobby"] = bool(all(results) and results[1].group(1) == attempt_id)
            checks["both_protocol_v2"] = all("Protocol v2." in t for t in (host_text, client_text))
            checks["both_completed_before_duration"] = all("stopping reason=prepare-test-complete" in t
                                                          for t in (host_text, client_text))
            checks["both_cleared_preparation_masks"] = all(
                f"preparation phase=lobby id={attempt_id} prepared=0 loading=0" in t
                for t in (host_text, client_text))
            events = (
                (host_text, ["begin", "ack-preparing", "authorize-loading", "ack-loading", "confirmed-loading"]),
                (client_text, ["ack-preparing", "ack-loading", "confirmed-loading", "cancel-request"]),
            )
            for role, (log, sequence) in zip(("host", "client"), events):
                positions = [log.find(f"prepare-test event={event} id={attempt_id}\n") for event in sequence]
                checks[f"{role}_ordered_handshake"] = all(p >= 0 for p in positions) and positions == sorted(positions)
        for check, passed in checks.items():
            print(f"{prefix}{check}: {'PASS' if passed else 'FAIL'}")
        if not all(checks.values()):
            raise RuntimeError("Two-process smoke assertion failed")
        print(f"Actual loopback UDP {'preparation' if prepare else 'lobby'} passed. Logs: {logs}")
    finally:
        for process in (client, host):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


def missing_peer(executable, logs):
    path = logs / "prepare-missing-peer.log"
    with path.open("wb") as output:
        result = subprocess.run(common_args(executable) + ["--host", "127.0.0.1", "--seconds", "3", "--prepare-test"],
                                stdout=output, stderr=subprocess.STDOUT, timeout=6,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    log = path.read_text(errors="replace")
    if result.returncode != 4 or "prepare-test result=FAIL" not in log or "result=PASS" in log:
        raise RuntimeError("Missing peer must cause bounded preparation failure")
    print("prepare-missing_peer_fails_bounded: PASS")


def invalid_options(executable):
    common = common_args(executable) + ["--host", "127.0.0.1", "--prepare-test"]
    for options in (["--seconds", "2"], ["--seconds", "121"], ["--prepare-test"]):
        result = subprocess.run(common + options, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=3, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if result.returncode != 2:
            raise RuntimeError(f"Invalid preparation options accepted: {options}")
    print("prepare-invalid_options_rejected: PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--peer-exe", type=Path, default=ROOT / "gow3_pc/out/gpu/uc2mp-lan-peer.exe")
    parser.add_argument("--logs", type=Path, default=ROOT / "work/local-lobby/lan-smoke")
    args = parser.parse_args()
    if not args.peer_exe.is_file():
        parser.error("Build the uc2mp-lan-peer target first")
    args.logs.mkdir(parents=True, exist_ok=True)
    invalid_options(args.peer_exe)
    run_pair(args.peer_exe, args.logs)
    run_pair(args.peer_exe, args.logs, prepare=True)
    missing_peer(args.peer_exe, args.logs)


if __name__ == "__main__":
    main()

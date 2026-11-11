#!/usr/bin/env python3
"""
Protocol conformance check for the bundled scrcpy device server.

This mirrors, step for step, what src/device/adb-client.c and
src/device/scrcpy-session.c do in C:

  1. host:devices-l over the adb socket, parsed the same way
  2. ro.build.version.sdk via the shell: service
  3. push scrcpy-server to /data/local/tmp
  4. reverse:forward:localabstract:scrcpy_<scid>;tcp:<port>
  5. launch the server through app_process on a shell: socket
  6. accept the video socket, then the audio socket
  7. read the 64-byte device-name field
  8. read each stream's 4-byte big-endian codec id
  9. read the video session packet (resolution)
 10. read a handful of media packets and sanity-check the framing

Run it after bumping the pinned scrcpy version, or whenever a device misbehaves.
If this passes, the C implementation's assumptions hold for that device; if it
fails, it fails with the exact byte offset rather than a black preview in OBS.

Usage:
  python scripts/protocol-check.py [--serial SERIAL] [--packets N] [--no-audio]
"""

from __future__ import annotations

import argparse
import os
import random
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "data", "bin")
ADB = os.path.join(BIN, "adb.exe")
SERVER_JAR = os.path.join(BIN, "scrcpy-server")

# Must match CORD_SCRCPY_VERSION in src/device/scrcpy-proto.h
SCRCPY_VERSION = "5.0.1"
DEVICE_SERVER_PATH = "/data/local/tmp/scrcpy-server.jar"
SERVER_CLASS = "com.genymobile.scrcpy.Server"

DEVICE_NAME_FIELD_LENGTH = 64
PACKET_HEADER_SIZE = 12
FLAG_CONFIG = 1 << 62
FLAG_KEY_FRAME = 1 << 61
PTS_MASK = FLAG_KEY_FRAME - 1

CODEC_IDS = {
    0x68323634: "h264",
    0x68323635: "h265",
    0x00617631: "av1",
    0x6F707573: "opus",
    0x00616163: "aac",
    0x666C6163: "flac",
    0x00726177: "raw",
}

ADB_PORT = 5037

ok_count = 0
fail_count = 0


def ok(msg: str) -> None:
    global ok_count
    ok_count += 1
    print(f"  [ok]   {msg}")


def fail(msg: str) -> None:
    global fail_count
    fail_count += 1
    print(f"  [FAIL] {msg}")


def info(msg: str) -> None:
    print(f"         {msg}")


# --------------------------------------------------------------------------
# adb wire protocol - same framing as adb_send_request()/adb_read_status()
# --------------------------------------------------------------------------


def adb_connect() -> socket.socket:
    s = socket.create_connection(("127.0.0.1", ADB_PORT), 5)
    return s


def adb_request(sock: socket.socket, service: str) -> None:
    payload = service.encode()
    sock.sendall(b"%04x" % len(payload) + payload)


def adb_status(sock: socket.socket, what: str) -> bool:
    status = recv_exact(sock, 4)
    if status == b"OKAY":
        return True
    if status == b"FAIL":
        n = int(recv_exact(sock, 4), 16)
        reason = recv_exact(sock, n).decode(errors="replace")
        fail(f"{what}: {reason}")
        return False
    fail(f"{what}: malformed reply {status!r}")
    return False


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"wanted {n} bytes, got {len(buf)} before EOF")
        buf += chunk
    return buf


def adb_query(service: str) -> str:
    s = adb_connect()
    try:
        adb_request(s, service)
        if not adb_status(s, service):
            return ""
        n = int(recv_exact(s, 4), 16)
        return recv_exact(s, n).decode(errors="replace") if n else ""
    finally:
        s.close()


def adb_open_service(serial: str, service: str) -> socket.socket:
    """Switch a fresh connection to the device, then open a service on it."""
    s = adb_connect()
    adb_request(s, f"host:transport:{serial}")
    if not adb_status(s, f"host:transport:{serial}"):
        s.close()
        raise RuntimeError("transport failed")
    adb_request(s, service)
    if not adb_status(s, service):
        s.close()
        raise RuntimeError(f"service failed: {service}")
    return s


# --------------------------------------------------------------------------
# steps
# --------------------------------------------------------------------------


def parse_device_line(line: str) -> dict:
    """Mirrors parse_device_line() in adb-client.c."""
    i = 0
    while i < len(line) and line[i] not in " \t":
        i += 1
    serial = line[:i]
    while i < len(line) and line[i] in " \t":
        i += 1
    j = i
    while j < len(line) and line[j] not in " \t":
        j += 1
    state = line[i:j]
    model = ""
    idx = line.find("model:", j)
    if idx >= 0:
        rest = line[idx + 6 :]
        model = rest.split()[0].replace("_", " ") if rest.split() else ""
    return {
        "serial": serial,
        "state": state,
        "model": model or serial,
        "is_tcpip": ":" in serial,
    }


def step_devices() -> list[dict]:
    print("\n[1] host:devices-l")
    payload = adb_query("host:devices-l")
    devices = [parse_device_line(l) for l in payload.splitlines() if l.strip()]
    if not devices:
        fail("no devices; connect a phone with USB debugging enabled")
        return []
    for d in devices:
        ok(
            f"serial={d['serial']} state={d['state']} "
            f"model='{d['model']}' tcpip={d['is_tcpip']}"
        )
    # The CLI prints a "List of devices attached" header; the socket must not.
    if "List of devices" in payload:
        fail("socket payload contained the CLI header - parser would misread it")
    else:
        ok("no CLI header in the socket payload, as the C parser assumes")
    return devices


def step_sdk(serial: str) -> int:
    print("\n[2] shell:getprop ro.build.version.sdk")
    s = adb_open_service(serial, "shell:getprop ro.build.version.sdk")
    try:
        data = b""
        s.settimeout(3)
        while True:
            try:
                chunk = s.recv(256)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
    finally:
        s.close()
    text = data.decode(errors="replace").strip()
    try:
        sdk = int(text)
    except ValueError:
        fail(f"unparseable sdk value {text!r}")
        return 0
    ok(f"API level {sdk}")
    if sdk < 30:
        fail("audio capture needs API 30 (Android 11) or newer")
    else:
        ok("meets the API 30 minimum for audio capture")
    return sdk


def step_push(serial: str) -> bool:
    print(f"\n[3] push scrcpy-server -> {DEVICE_SERVER_PATH}")
    if not os.path.exists(SERVER_JAR):
        fail(f"{SERVER_JAR} missing - run scripts/fetch-deps.ps1")
        return False
    r = subprocess.run(
        [ADB, "-s", serial, "push", SERVER_JAR, DEVICE_SERVER_PATH],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fail(f"push failed: {r.stderr.strip()}")
        return False
    ok("pushed")
    return True


def build_command(scid: int, audio: bool) -> str:
    """Mirrors build_server_command() in scrcpy-session.c."""
    parts = [
        f"shell:CLASSPATH={DEVICE_SERVER_PATH}",
        "app_process",
        "/",
        SERVER_CLASS,
        SCRCPY_VERSION,
        f"scid={scid:08x}",
        "log_level=info",
        "control=false",
        "clipboard_autosync=false",
        "video_bit_rate=8000000",
        "max_fps=60",
        "cleanup=true",
    ]
    if not audio:
        parts.append("audio=false")
    return " ".join(parts)


def step_session(serial: str, packets: int, want_audio: bool) -> None:
    scid = random.randint(0, 0x7FFFFFFF)
    socket_name = f"scrcpy_{scid:08x}"

    print(f"\n[4] reverse tunnel localabstract:{socket_name}")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    port = listener.getsockname()[1]

    try:
        s = adb_open_service(
            serial, f"reverse:forward:localabstract:{socket_name};tcp:{port}"
        )
        s.close()
        ok(f"tunnel established on local port {port}")
    except RuntimeError:
        fail("reverse tunnel refused (the C code falls back to forward mode here)")
        listener.close()
        return

    print("\n[5] launch the device server via app_process")
    command = build_command(scid, want_audio)
    info(command)
    try:
        proc_sock = adb_open_service(serial, command)
        ok("server process started")
    except RuntimeError:
        fail("could not start the device server")
        listener.close()
        return

    video_sock = None
    audio_sock = None
    try:
        print("\n[6] accept the streams (video first, then audio)")
        listener.settimeout(10)
        try:
            video_sock, _ = listener.accept()
            ok("video socket accepted")
        except socket.timeout:
            fail("device never connected back within 10s")
            dump_server_log(proc_sock)
            return

        if want_audio:
            try:
                audio_sock, _ = listener.accept()
                ok("audio socket accepted")
            except socket.timeout:
                fail("audio socket never arrived")

        print("\n[7] device-name field (64 bytes on the first socket)")
        name_bytes = recv_exact(video_sock, DEVICE_NAME_FIELD_LENGTH)
        name = name_bytes.split(b"\x00")[0].decode(errors="replace")
        ok(f"device name: '{name}' ({len(name_bytes)} bytes consumed)")

        print("\n[8] codec ids (4 bytes big-endian per stream)")
        raw = struct.unpack(">I", recv_exact(video_sock, 4))[0]
        if raw == 0:
            fail("video stream explicitly disabled by the device")
            return
        if raw == 1:
            fail("device could not configure the video stream")
            return
        vname = CODEC_IDS.get(raw, f"unknown(0x{raw:08x})")
        if raw in CODEC_IDS:
            ok(f"video codec: {vname}")
        else:
            fail(f"video codec id not recognised: 0x{raw:08x}")

        if audio_sock:
            araw = struct.unpack(">I", recv_exact(audio_sock, 4))[0]
            if araw == 0:
                fail(
                    "audio stream DISABLED by the device - this is the "
                    "'silent teammates' case the C code reports"
                )
                audio_sock.close()
                audio_sock = None
            elif araw == 1:
                fail("device could not configure the audio stream")
                audio_sock.close()
                audio_sock = None
            else:
                aname = CODEC_IDS.get(araw, f"unknown(0x{araw:08x})")
                if araw in CODEC_IDS:
                    ok(f"audio codec: {aname}")
                else:
                    fail(f"audio codec id not recognised: 0x{araw:08x}")

        print("\n[9] video session packet (resolution)")
        hdr = recv_exact(video_sock, PACKET_HEADER_SIZE)
        if not (hdr[0] & 0x80):
            fail("first video packet was not a session header")
        else:
            w, h = struct.unpack(">II", hdr[4:12])
            if w and h:
                ok(f"resolution {w}x{h}")
            else:
                fail(f"invalid session resolution {w}x{h}")

        print(f"\n[10] media packet framing (first {packets} video packets)")
        read_packets(video_sock, packets, "video")

        if audio_sock:
            print(f"\n[11] media packet framing (first {packets} audio packets)")
            read_packets(audio_sock, packets, "audio")

    except EOFError as e:
        fail(f"stream ended early: {e}")
        dump_server_log(proc_sock)
    finally:
        for s in (video_sock, audio_sock, proc_sock):
            if s:
                try:
                    s.close()
                except OSError:
                    pass
        listener.close()
        # Closing the shell socket is what kills app_process on the device.
        subprocess.run(
            [ADB, "-s", serial, "reverse", "--remove", f"localabstract:{socket_name}"],
            capture_output=True,
        )


def read_packets(sock: socket.socket, count: int, label: str) -> None:
    sock.settimeout(5)
    configs = 0
    keys = 0
    first_pts = None
    last_pts = None
    total = 0

    for i in range(count):
        hdr = recv_exact(sock, PACKET_HEADER_SIZE)
        if hdr[0] & 0x80:
            w, h = struct.unpack(">II", hdr[4:12])
            info(f"  packet {i}: session header, now {w}x{h}")
            continue
        flags_pts, size = struct.unpack(">QI", hdr)
        is_config = bool(flags_pts & FLAG_CONFIG)
        is_key = bool(flags_pts & FLAG_KEY_FRAME)
        pts = flags_pts & PTS_MASK

        if size == 0 or size > 16 * 1024 * 1024:
            fail(f"packet {i}: implausible size {size} - framing is wrong")
            return

        payload = recv_exact(sock, size)
        total += len(payload)

        if is_config:
            configs += 1
        if is_key:
            keys += 1
        if not is_config:
            if first_pts is None:
                first_pts = pts
            last_pts = pts

        tag = []
        if is_config:
            tag.append("config")
        if is_key:
            tag.append("key")
        info(
            f"  packet {i}: {size:>7} bytes  pts={pts:>12}"
            f"{'  [' + ','.join(tag) + ']' if tag else ''}"
        )

    ok(f"{count} {label} packets read, {total} bytes, {configs} config, {keys} key")

    if first_pts is not None and last_pts is not None and last_pts > first_pts:
        span_ms = (last_pts - first_pts) / 1000.0
        ok(f"PTS advances monotonically over {span_ms:.1f} ms - timebase is µs")
    elif first_pts is not None:
        info("not enough media packets to confirm PTS progression")


def dump_server_log(proc_sock: socket.socket) -> None:
    """Whatever the device server said is usually the real explanation."""
    try:
        proc_sock.settimeout(2)
        data = b""
        while len(data) < 4096:
            chunk = proc_sock.recv(1024)
            if not chunk:
                break
            data += chunk
    except (socket.timeout, OSError):
        pass
    else:
        if data:
            print("         --- device server output ---")
            for line in data.decode(errors="replace").splitlines():
                print(f"         | {line}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="device serial (default: the only device)")
    ap.add_argument("--packets", type=int, default=8, help="media packets to read")
    ap.add_argument("--no-audio", action="store_true", help="skip the audio stream")
    args = ap.parse_args()

    print(f"scrcpy protocol check (pinned version {SCRCPY_VERSION})")

    if not os.path.exists(ADB):
        print(f"adb not found at {ADB} - run scripts/fetch-deps.ps1", file=sys.stderr)
        return 2

    subprocess.run([ADB, "start-server"], capture_output=True)

    devices = step_devices()
    if not devices:
        return 1

    serial = args.serial
    if not serial:
        ready = [d for d in devices if d["state"] == "device"]
        if len(ready) != 1:
            print(
                f"\nexpected exactly one ready device, found {len(ready)}; "
                "pass --serial",
                file=sys.stderr,
            )
            return 1
        serial = ready[0]["serial"]

    step_sdk(serial)
    if not step_push(serial):
        return 1
    step_session(serial, args.packets, not args.no_audio)

    print(f"\n{'=' * 60}")
    print(f"passed: {ok_count}   failed: {fail_count}")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

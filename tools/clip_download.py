#!/usr/bin/env python3
"""Download a committed SDK clip through the local maintenance socket and verify SHA-256.

This is not a remote SDK transfer or cloud upload client. Existing output is never replaced.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import tempfile
import time


def download(socket_path, serial, slot, record_id, output):
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,31}", serial) or not 0 <= slot < 4 or not re.fullmatch(r"[0-9a-f]{16}", record_id):
        raise ValueError("invalid camera identity, slot or recording ID")
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    deadline = time.monotonic() + 55
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect(str(socket_path))
        sock.sendall(f"if-serial {serial} record-get {slot} {record_id}\n".encode("ascii"))
        line = bytearray()
        while not line.endswith(b"\n"):
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError("recording download deadline exceeded")
            sock.settimeout(min(5, remaining))
            part = sock.recv(1)
            if not part or len(line) >= 2048:
                raise ValueError("incomplete or excessive response header")
            line += part
        header = json.loads(line)
        if not isinstance(header, dict):
            raise ValueError("invalid response header")
        if not header.get("ok"):
            raise ValueError(header.get("error", "record request failed"))
        size, expected = header.get("bytes"), header.get("sha256", "")
        if (header.get("format") != "mpegts-h264-aac" or type(size) is not int or
            not 0 < size <= 512*1024*1024 or size % 188 or not isinstance(expected, str) or
            not re.fullmatch(r"[0-9a-f]{64}", expected)):
            raise ValueError("expected SDK TS recording with a committed SHA-256")
        fd, temporary = tempfile.mkstemp(prefix=".myvava-download-", dir=output.parent)
        try:
            digest, received = hashlib.sha256(), 0
            with os.fdopen(fd, "wb") as target:
                while received < size:
                    remaining = deadline-time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("recording download deadline exceeded")
                    sock.settimeout(min(5, remaining))
                    part = sock.recv(min(65536, size-received))
                    if not part:
                        raise EOFError("incomplete recording")
                    target.write(part); digest.update(part); received += len(part)
                if digest.hexdigest() != expected:
                    raise ValueError("recording SHA-256 mismatch")
                target.flush(); os.fsync(target.fileno())
            # Same-directory link is an atomic no-replace publication.
            os.link(temporary, output)
            os.unlink(temporary)
            directory = os.open(output.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
            return {"ok": True, "bytes": received, "sha256": expected, "output": str(output)}
        finally:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, type=Path)
    parser.add_argument("--serial", required=True)
    parser.add_argument("slot", type=int, choices=range(4))
    parser.add_argument("record_id")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        result = download(args.socket, args.serial, args.slot, args.record_id, args.output)
    except (OSError, EOFError, ValueError) as e:
        parser.exit(1, f"clip download failed: {e}\n")
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()

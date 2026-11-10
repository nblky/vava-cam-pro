#!/usr/bin/env python3
"""Local control client for myvava-server (private Unix socket, no HTTP)."""
import argparse
import json
import math
import secrets
import socket
import sys
import time


def request(path, command):
    if "\n" in command or "\r" in command or "\0" in command:
        raise ValueError("command must be a single line")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect(str(path))
        sock.sendall(command.encode() + b"\n")
        data = bytearray()
        while not data.endswith(b"\n"):
            part = sock.recv(8192)
            if not part:
                raise EOFError("incomplete server response")
            data.extend(part)
            if len(data) > 210000:
                raise ValueError("server response too large")
    return json.loads(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("devices")
    result = sub.add_parser("result")
    result.add_argument("--epoch", required=True, help="server epoch returned with the original task")
    result.add_argument("id")
    lookup = sub.add_parser("lookup", help="recover a retained task after losing its submission reply")
    lookup.add_argument("device")
    lookup.add_argument("request_key")
    history = sub.add_parser("history", help="list a page of retained task metadata")
    history.add_argument("device")
    history.add_argument("--before", default="0000000000000000")
    events = sub.add_parser("events", help="list a page of persisted station events")
    events.add_argument("device")
    events.add_argument("--before", default="0000000000000000")
    files = sub.add_parser("files", help="list a page of durable remote clips")
    files.add_argument("device")
    files.add_argument("--page", type=int, default=0)
    run = sub.add_parser("run")
    run.add_argument("--request-key", default=None, help="retry the original 32-hex key unchanged; new keys are issued automatically")
    run.add_argument("--timeout", type=float, default=35)
    run.add_argument("device")
    run.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.action != "run":
        if args.action == "devices":
            command = "devices"
        elif args.action == "result":
            command = "result " + args.epoch + " " + args.id
        elif args.action == "files":
            command = "files " + args.device + " " + str(args.page)
        elif args.action == "lookup":
            command = "lookup " + args.device + " " + args.request_key
        else:
            command = args.action + " " + args.device + " " + args.before
        reply = request(args.socket, command)
    else:
        if not args.command:
            parser.error("a device command is required")
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            parser.error("timeout must be a positive finite number")
        snapshot = request(args.socket, "devices")
        if not snapshot.get("ok"):
            print(json.dumps(snapshot, ensure_ascii=False, indent=2))
            raise SystemExit(1)
        device = next((d for d in snapshot["devices"] if d["id"] == args.device), None)
        if not device or not device["online"]:
            parser.error("device is unknown or offline")
        key = args.request_key or snapshot["request_prefix"] + secrets.token_hex(8)
        # Emit the recovery handle before any write request can leave this process.
        print(json.dumps({"request_key": key, "device": args.device, "epoch": snapshot["epoch"]}), file=sys.stderr, flush=True)
        reply = request(args.socket, " ".join(["submit", args.device, device["boot"], snapshot["epoch"], key, *args.command]))
        if reply.get("ok"):
            task_id = reply["id"]
            task_epoch = reply["epoch"]
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                reply = request(args.socket, "result " + task_epoch + " " + task_id)
                if reply.get("state") not in {"queued", "dispatched"}:
                    break
                time.sleep(0.1)
            reply["request_key"] = key
            reply["id"] = task_id
            reply["epoch"] = task_epoch
    if reply.get("state") == "completed":
        reply["result"] = json.loads(reply["result"])
    print(json.dumps(reply, ensure_ascii=False, indent=2))
    if not reply.get("ok") or (args.action == "run" and
        (reply.get("state") != "completed" or not reply.get("result", {}).get("ok"))):
        raise SystemExit(1)


if __name__ == "__main__":
    main()

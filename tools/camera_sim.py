#!/usr/bin/env python3
"""Synthetic camera for host development. This is not a hardware protocol oracle."""
import argparse
import hashlib
import json
import selectors
import socket
import struct
import time
import zlib
from pathlib import Path


def control(command, payload=b""):
    if len(payload) > 116:
        raise ValueError("payload too long")
    body = struct.pack("<I", command) + payload.ljust(116, b"\0")
    return struct.pack("<II", 0xEB000001, zlib.crc32(body)) + body


def decode(packet):
    magic, crc, command = struct.unpack_from("<III", packet)
    if len(packet) != 128 or magic != 0xEB000001 or crc != zlib.crc32(packet[8:]):
        raise ValueError("invalid control record")
    return command, packet[12:]


def text32(value):
    b = value.encode("ascii")
    if not 1 <= len(b) <= 31 or b"\0" in b:
        raise ValueError("invalid text32")
    return b.ljust(32, b"\0")


def guest(serial, seed=0xFFFFFFFF):
    signed = seed if seed < 0x80000000 else seed - 2**32
    digest = hashlib.md5(f"vava:{signed}:2019".encode()).hexdigest().encode()
    return control(400, struct.pack("<I", seed) + digest.ljust(36, b"\0") + text32(serial))


def first(serial):
    p = bytearray(116)
    p[0:2] = bytes([0, 3])
    p[64:96] = text32(serial)
    return control(5, p)


def media(payload, stream=1, frame_type=1, tail=True):
    if tail:
        payload += bytes.fromhex("eb5555eb")
    return struct.pack("<I8BIQ", 0xEB000002, stream, 0, frame_type, 10, 0, 1,
                       int(tail), 0, len(payload), int(time.time() * 1000)) + payload


def read_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        b = sock.recv(n - len(data))
        if not b:
            raise EOFError("connection closed")
        data.extend(b)
    return bytes(data)


def connect(host, port):
    sock = socket.create_connection((host, port), timeout=3)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def pair(host, port, serial, seed=0xFFFFFFFF, ack=True):
    with connect(host, port) as temp:
        # Exercise fragmentation, independent of the C implementation.
        packet = guest(serial, seed)
        temp.sendall(packet[:9])
        temp.sendall(packet[9:])
        cmd, p = decode(read_exact(temp, 128))
        if cmd != 401 or struct.unpack_from("<i", p)[0] != 0:
            raise ValueError("pair offer rejected")
        slot = struct.unpack_from("<I", p, 4)[0]
        temp.sendall(control(402))
    sock = connect(host, port)
    sock.sendall(control(1, struct.pack("<I", slot) + text32(serial)))
    cmd, p = decode(read_exact(sock, 128))
    if cmd != 2 or struct.unpack_from("<i", p)[0] != 0:
        sock.close()
        raise ValueError("formal pairing rejected")
    if ack:
        sock.sendall(control(3))
    return slot, sock


class Camera:
    def __init__(self, host, ports, serial, payload, do_pair):
        self.host, self.ports, self.serial = host, ports, serial
        self.payload = payload
        self.selector = selectors.DefaultSelector()
        self.ctrl = self.av = self.alive = None
        self.control_buffer = bytearray()
        self.alive_buffer = bytearray()
        self.received_control = []
        self.talk_buffer = bytearray()
        self.talk_packets = []
        self.live = False
        self.auto_media = True
        self.last_heart = self.last_frame = self.last_sleep = 0.0
        self.events = []
        self.slot = None
        if do_pair:
            self.slot, self.ctrl = pair(host, ports[0], serial)
        self.start_control()

    def register(self, sock, kind):
        # Local simulator uses bounded socket timeouts, never the station runtime.
        sock.settimeout(1)
        self.selector.register(sock, selectors.EVENT_READ, kind)

    def drop(self, name):
        sock = getattr(self, name)
        if sock is not None:
            try:
                self.selector.unregister(sock)
            except KeyError:
                pass
            sock.close()
            setattr(self, name, None)

    def start_control(self):
        if self.ctrl is None:
            self.ctrl = connect(self.host, self.ports[0])
        self.control_buffer.clear()
        self.ctrl.sendall(first(self.serial))
        cmd, _ = decode(read_exact(self.ctrl, 128))
        if cmd != 6:
            raise ValueError(f"expected CONNECT_ACK, got {cmd}")
        self.register(self.ctrl, "control")
        self.av = connect(self.host, self.ports[1])
        self.register(self.av, "media")
        if self.alive is None:
            self.alive = connect(self.host, self.ports[2])
            self.register(self.alive, "alive")
        self.live = False
        self.events.append("connected")

    def sleep(self):
        self.live = False
        self.alive.sendall(b"sl")
        self.alive.sendall(b"eep\0")
        self.drop("ctrl")
        self.drop("av")
        self.events.append("sleep")

    def step(self, timeout=0.05):
        for key, _ in self.selector.select(timeout):
            data = key.fileobj.recv(4096)
            if not data:
                raise EOFError(f"station closed {key.data}")
            if key.data == "control":
                self.control_buffer.extend(data)
                while len(self.control_buffer) >= 128:
                    cmd, p = decode(bytes(self.control_buffer[:128]))
                    del self.control_buffer[:128]
                    self.events.append(cmd)
                    self.received_control.append((cmd,p))
                    self.received_control=self.received_control[-512:]
                    if cmd == 102:
                        self.live = True
                        self.ctrl.sendall(control(cmd, p))
                    elif cmd == 20:
                        self.last_frame = 0
                    elif cmd == 403:
                        self.sleep()
                        break
            elif key.data == "media":
                self.talk_buffer.extend(data)
                while len(self.talk_buffer) >= 24:
                    length=struct.unpack_from('<I',self.talk_buffer,12)[0]
                    if length>1000: raise ValueError('invalid talk frame length')
                    if len(self.talk_buffer)<24+length: break
                    self.talk_packets.append(bytes(self.talk_buffer[:24+length]))
                    del self.talk_buffer[:24+length]
            elif key.data == "alive":
                self.alive_buffer.extend(data)
                while len(self.alive_buffer) >= 6:
                    if self.alive_buffer[:6] != b"wakeup":
                        raise ValueError("unexpected wake data")
                    del self.alive_buffer[:6]
                    if self.ctrl is None:
                        self.events.append("wakeup")
                        self.start_control()
        now = time.monotonic()
        if self.ctrl is not None and now - self.last_heart >= 1:
            p = struct.pack("<iBBHIIII", -45, 80, 0, 0, 3900, 25, 0, 0)
            self.ctrl.sendall(control(0, p))
            self.last_heart = now
        if self.auto_media and self.live and self.av is not None and now - self.last_frame >= 0.2:
            packet = media(self.payload)
            self.av.sendall(packet[:17])
            self.av.sendall(packet[17:])
            self.last_frame = now
        if self.ctrl is None and now - self.last_sleep >= 1:
            self.alive.sendall(b"sleep\0")
            self.last_sleep = now

    def close(self):
        for name in ("ctrl", "av", "alive"):
            self.drop(name)
        self.selector.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ports", type=int, nargs=3, default=[16154, 18154, 20154])
    parser.add_argument("--serial", default="SIM-CAMERA-0001")
    parser.add_argument("--pair", action="store_true")
    parser.add_argument("--h264", type=Path, help="one Annex B access unit with SPS/PPS/IDR, at most 1 MiB")
    parser.add_argument("--duration", type=float, default=60)
    args = parser.parse_args()
    # Framing-only default. Use --h264 for actual decoding validation.
    payload = args.h264.read_bytes() if args.h264 else bytes.fromhex("00000001658884")
    if len(payload) > 1024 * 1024 - 4:
        parser.error("sample exceeds frame limit")
    cam = Camera(args.host, args.ports, args.serial, payload, args.pair)
    print(json.dumps({"synthetic": True, "slot": cam.slot, "serial": args.serial,
                      "decodable_sample_supplied": bool(args.h264)}), flush=True)
    until = time.monotonic() + args.duration
    try:
        while time.monotonic() < until:
            cam.step()
    except KeyboardInterrupt:
        pass
    finally:
        cam.close()


if __name__ == "__main__":
    main()

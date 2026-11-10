#!/usr/bin/env python3
"""Local media tools. Camera playback/codec compatibility still needs hardware validation."""
import argparse
import json
import select
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path


def exact(sock, n, deadline=None):
    result = bytearray()
    while len(result) < n:
        if deadline is not None:
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('capture duration reached')
            sock.settimeout(remaining)
        part = sock.recv(n-len(result))
        if not part:
            raise EOFError('media connection ended')
        result += part
    return bytes(result)


def connect(path, command):
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(20)
    try:
        sock.connect(str(path))
        sock.sendall(command.encode('ascii') + b'\n')
        line = bytearray()
        while not line.endswith(b'\n') and len(line) < 16384:
            line += exact(sock, 1)
        result = json.loads(line)
        if not result['ok']:
            raise ValueError(result['error'])
        return sock
    except BaseException:
        sock.close()
        raise


def adts_size(p):
    if len(p) < 7 or p[0] != 255 or p[1] & 0xF6 != 0xF0:
        raise ValueError('expected an ADTS AAC frame; raw audio is not silently re-labelled')
    n = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5)
    if n < 7:
        raise ValueError('invalid ADTS length')
    return n


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-s', '--socket', type=Path, default=Path('run/control.sock'))
    sub = parser.add_subparsers(dest='action', required=True)
    snapshot = sub.add_parser('snapshot', help='next decodable IDR, optionally convert to JPEG using local FFmpeg')
    snapshot.add_argument('slot', type=int, choices=range(4)); snapshot.add_argument('output', type=Path)
    snapshot.add_argument('--jpeg', action='store_true')
    audio = sub.add_parser('audio', help='capture validated ADTS payload, or complete wire records with --raw')
    audio.add_argument('slot', type=int, choices=range(4)); audio.add_argument('output', type=Path)
    audio.add_argument('--stream', type=int, choices=(8, 9), default=9)
    audio.add_argument('--seconds', type=float, default=10); audio.add_argument('--raw', action='store_true')
    talk = sub.add_parser('talk', help='send an AAC-LC ADTS 8 kHz mono file during an active live session')
    talk.add_argument('slot', type=int, choices=range(4)); talk.add_argument('input', type=Path)
    args = parser.parse_args()
    if args.action in ('snapshot', 'audio') and args.output.exists():
        parser.error('output already exists')
    if args.action == 'snapshot':
        data = bytearray()
        with connect(args.socket, f'snapshot {args.slot}') as sock:
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
                if len(data) > 2*1024*1024:
                    raise ValueError('snapshot exceeds limit')
        if not data:
            raise ValueError('no keyframe received')
        if args.jpeg:
            # Conversion is a client responsibility; the station needs no decoder.
            subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-n', '-f', 'h264', '-i', 'pipe:0',
                            '-frames:v', '1', '-update', '1', str(args.output)], input=data, check=True)
        else:
            with args.output.open('xb') as output:
                output.write(data)
    elif args.action == 'audio':
        if not 0 < args.seconds <= 120:
            parser.error('seconds must be between 0 and 120')
        with connect(args.socket, f'audio {args.slot} {args.stream}') as sock, args.output.open('xb') as output:
            end = time.monotonic()+args.seconds
            while time.monotonic() < end:
                try:
                    header = exact(sock, 24, end)
                except TimeoutError:
                    break
                magic, = struct.unpack_from('<I', header)
                length, = struct.unpack_from('<I', header, 12)
                if magic != 0xEB000002 or not 0 < length <= 1024*1024 or header[4] != args.stream or header[6] != 8:
                    raise ValueError('invalid audio record')
                try:
                    payload = exact(sock, length, end)
                except TimeoutError:
                    break  # Do not save an incomplete final record.
                if args.raw:
                    output.write(header+payload)
                else:
                    if header[5] != 3:
                        raise ValueError('not the reviewed AAC codec; use --raw for capture')
                    offset = 0
                    while offset < len(payload):
                        size = adts_size(payload[offset:])
                        offset += size
                        if offset > len(payload):
                            raise ValueError('incomplete AAC frame; use --raw for capture')
                    output.write(payload)
    else:
        with args.input.open('rb') as source, connect(args.socket, f'talk {args.slot}') as sock:
            next_frame = time.monotonic()
            while True:
                header = source.read(7)
                if not header:
                    break
                size = adts_size(header)
                if len(header) != 7 or not 8 <= size <= 1000 or (header[1]&0xF7) != 0xF1 or header[2]>>6 != 1 or \
                        (header[2]>>2)&15 != 11 or ((header[2]&1)<<2)|(header[3]>>6) != 1 or header[6]&3:
                    raise ValueError('talk requires AAC-LC 8 kHz mono, one block, no CRC, frame size 8..1000')
                body = source.read(size-7)
                if len(body) != size-7:
                    raise ValueError('truncated AAC file')
                time.sleep(max(0, next_frame-time.monotonic()))
                if select.select([sock], [], [], 0)[0]:
                    raise ValueError('talk session ended: '+sock.recv(1024).decode(errors='replace'))
                sock.sendall(struct.pack('<I', size)+header+body)
                next_frame += 1024/8000
            time.sleep(max(0, next_frame-time.monotonic()))
        print('Audio transport sent; camera playback is not verified.', file=sys.stderr)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, EOFError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)

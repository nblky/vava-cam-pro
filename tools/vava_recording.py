#!/usr/bin/env python3
"""Read-only parser/exporter for the audited Ppcs_vava SD format (not cloud RTMP).

Requires cryptography only when decrypting mode 0x1a. Never contacts a device.
The input is never modified; export requires an entirely new output directory.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

FRAME_MAGIC = 0xEB0000AA
INDEX_MAGIC = 0xEB0000BB
KEY = b'vavalic2' + b'\0' * 8
MAX_PAYLOAD = 16 * 1024 * 1024
MAX_INDEX = 32 * 1024 * 1024


class FormatError(ValueError):
    pass


def decrypt_blocks(data, key=KEY):
    """Firmware processes complete AES blocks; the short tail stays unchanged."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    count = len(data) // 16 * 16
    decoder = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return decoder.update(data[:count]) + decoder.finalize() + data[count:]


def decode_payload(payload, frame_type, mode):
    if mode not in (0, 0x1a):
        raise FormatError(f'unsupported encryption marker: {mode:#x}')
    if frame_type not in (0, 1, 8):
        raise FormatError(f'unsupported frame type: {frame_type}')
    if mode == 0:
        return payload
    if frame_type == 1:
        return decrypt_blocks(payload)
    if frame_type == 8:
        if not payload or payload[0] != 0xbb:
            raise FormatError('encrypted audio does not begin with 0xbb')
        return b'\xff' + decrypt_blocks(payload[1:])
    return payload


def file_header(raw):
    if len(raw) != 16:
        raise FormatError('truncated 16-byte file header')
    event, video, audio, rec_hint, live_hint, mode, count, size, duration = struct.unpack('<6BHII', raw)
    if mode not in (0, 0x1a):
        raise FormatError(f'unsupported encryption marker: {mode:#x}')
    return dict(event_type=event, video_codec=video, audio_codec=audio,
                camera_hint_4=rec_hint, camera_hint_5=live_hint, encryption_marker=mode,
                video_counter=count, payload_bytes=size, duration_seconds=duration)


def frames(stream):
    """Yield bounded payloads, preserving offsets and original per-packet times."""
    total_size = stream.seek(0, 2)
    stream.seek(16)
    while stream.tell() < total_size:
        offset = stream.tell()
        raw = stream.read(24)
        if len(raw) != 24:
            raise FormatError(f'truncated frame header at {offset}')
        magic, size, kind, hint, seconds, millis = struct.unpack('<6I', raw)
        if magic != FRAME_MAGIC:
            raise FormatError(f'bad frame magic at {offset}: {magic:#x}')
        if not 0 < size <= MAX_PAYLOAD:
            raise FormatError(f'invalid/oversize payload at {offset}: {size}')
        if kind not in (0, 1, 8) or millis >= 1000:
            raise FormatError(f'invalid frame type/time at {offset}')
        if size > total_size - stream.tell():
            raise FormatError(f'truncated payload at {offset}: need {size} bytes')
        payload = stream.read(size)
        if len(payload) != size:
            raise FormatError(f'input changed/truncated while reading at {offset}')
        yield dict(offset=offset, payload_size=size, frame_type=kind, hint=hint,
                   pts_ms=seconds*1000+millis), raw, payload


def inspect_record(path, decode=False):
    counts = Counter()
    prefixes = {}
    timelines = []
    total = 0
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        raw = stream.read(16)
        header = file_header(raw)
        digest.update(raw)
        for meta, packet_header, payload in frames(stream):
            digest.update(packet_header)
            digest.update(payload)
            clear = decode_payload(payload, meta['frame_type'], header['encryption_marker']) if decode else payload
            counts[meta['frame_type']] += 1
            total += len(payload)
            prefixes.setdefault(str(meta['frame_type']), clear[:32].hex())
            timelines.append(meta)
    warnings = []
    if total != header['payload_bytes']:
        warnings.append('header payload byte total differs from parsed total (unfinished file or corruption)')
    video_count = counts[0] + counts[1]
    if header['video_counter'] != video_count:
        warnings.append(f'header video counter {header["video_counter"]} differs from {video_count} parsed video frames; do not use it as an EOF limit')
    for kind in (0, 8):
        times = [f['pts_ms'] for f in timelines if (f['frame_type'] == 8) == (kind == 8)]
        if any(b < a for a, b in zip(times, times[1:])):
            warnings.append(f'{"audio" if kind == 8 else "video"} timestamps decrease')
    return dict(source=str(path.resolve()), sha256=digest.hexdigest(), header=header,
                frame_counts=dict(sorted(counts.items())), parsed_payload_bytes=total,
                decoded=decode, first_payload_prefixes=prefixes, warnings=warnings, frames=timelines)


def inspect_index(path, kind):
    if path.stat().st_size > MAX_INDEX:
        raise FormatError('index exceeds 32 MiB inspection limit')
    data = path.read_bytes()
    if len(data) < 12:
        raise FormatError('truncated index header')
    magic, total, first = struct.unpack_from('<3I', data)
    if magic != INDEX_MAGIC:
        raise FormatError('bad index magic')
    width = 10 if kind == 'date' else 16
    if len(data) != 12 + total * width or first > total:
        raise FormatError('index size/count/first mismatch')
    entries = []
    for n in range(total):
        row = data[12+n*width:12+(n+1)*width]
        seed = (row[0]-26) & 255
        start, size = (1, 8) if kind == 'date' else (3, 10)
        try:
            name = bytes(x ^ seed for x in row[start:start+size]).decode('ascii')
        except UnicodeDecodeError as exc:
            raise FormatError(f'non-ASCII index entry {n}') from exc
        pattern = r'[0-9]{8}' if kind == 'date' else r'[0-9]{6}_[A-Za-z]_[0-3]'
        if not re.fullmatch(pattern, name):
            raise FormatError(f'invalid decoded index name at entry {n}')
        entry = dict(entry=n, active=n >= first, name=name, seed=seed)
        if kind == 'file':
            entry.update(event_type=row[1], channel=row[2], duration_seconds=struct.unpack_from('<H', row, 14)[0])
        entries.append(entry)
    return dict(source=str(path.resolve()), sha256=hashlib.sha256(data).hexdigest(),
                kind=kind, total=total, first=first, entries=entries)


def export_record(path, output):
    # Validate the complete input before creating any outputs. Never publish a
    # success manifest for a truncated file, unsupported codec or failed export.
    report = inspect_record(path, decode=True)
    h = report['header']
    if h['video_codec'] != 0 or (report['frame_counts'].get(8) and h['audio_codec'] != 3):
        raise FormatError('raw export currently supports H.264 (0) and AAC (3) only')
    output.mkdir(parents=True, exist_ok=False)
    digest = hashlib.sha256()
    with path.open('rb') as src, (output/'video.h264').open('xb') as video, (output/'audio.aac').open('xb') as audio, (output/'clear.vava').open('xb') as native:
        raw = src.read(16)
        digest.update(raw)
        native.write(raw[:5] + b'\0' + raw[6:])
        for meta, frame_header, payload in frames(src):
            digest.update(frame_header)
            digest.update(payload)
            clear = decode_payload(payload, meta['frame_type'], h['encryption_marker'])
            (audio if meta['frame_type'] == 8 else video).write(clear)
            native.write(frame_header)
            native.write(clear)
    if digest.hexdigest() != report['sha256']:
        raise FormatError('input changed during export; outputs are incomplete/untrusted')
    report['outputs'] = ['video.h264', 'audio.aac', 'clear.vava']
    report['timing_note'] = 'Raw elementary streams carry no original packet timeline; frames[].pts_ms and clear.vava preserve it. No CFR MP4 conversion is performed.'
    with (output/'manifest.json').open('x') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write('\n')
    return {k: v for k, v in report.items() if k != 'frames'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    rec = sub.add_parser('inspect')
    rec.add_argument('input', type=Path)
    rec.add_argument('--decrypt', action='store_true')
    rec.add_argument('--frames', action='store_true')
    idx = sub.add_parser('index')
    idx.add_argument('input', type=Path)
    idx.add_argument('--kind', choices=['date','file'], required=True)
    exp = sub.add_parser('export')
    exp.add_argument('input', type=Path)
    exp.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    try:
        if args.command == 'index':
            result = inspect_index(args.input, args.kind)
        elif args.command == 'export':
            result = export_record(args.input, args.output)
        else:
            result = inspect_record(args.input, args.decrypt)
            if not args.frames:
                result.pop('frames')
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (OSError, FormatError, ImportError) as exc:
        print(f'error: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

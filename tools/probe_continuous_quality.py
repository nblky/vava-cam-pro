#!/usr/bin/env python3
"""Offline diagnostic: production remux arguments with changing H.264 SPS.

Only generated colour/sine samples are used. No device, camera or server is
contacted. This demonstrates container metadata, not browser compatibility.
"""
import argparse
import json
import shutil
import struct
import subprocess
from pathlib import Path

FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, timeout=45, **kwargs)


def boxes(data, start=0, end=None):
    end = len(data) if end is None else end
    while start < end:
        if start + 8 > end:
            raise ValueError("truncated box header")
        n, kind = struct.unpack_from(">I4s", data, start)
        header = 8
        if n == 1:
            n = struct.unpack_from(">Q", data, start + 8)[0]
            header = 16
        if n < header or start + n > end:
            raise ValueError("invalid box size")
        yield start, n, kind, header
        start += n


def metadata(data):
    result = {"top_level": {}, "video_entries": [], "avc_configurations": []}

    def walk(start, end):
        for p, n, kind, header in boxes(data, start, end):
            if kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl"):
                walk(p + header, p + n)
            elif kind == b"stsd":
                walk(p + header + 8, p + n)
            elif kind in (b"avc1", b"avc3"):
                w, h = struct.unpack_from(">HH", data, p + header + 24)
                result["video_entries"].append({"type": kind.decode(), "width": w, "height": h})
                walk(p + header + 78, p + n)
            elif kind == b"avcC":
                result["avc_configurations"].append({
                    "codec": "avc1." + data[p + header + 1:p + header + 4].hex(),
                    "bytes": n - header,
                })

    for _, _, kind, _ in boxes(data):
        name = kind.decode("ascii")
        result["top_level"][name] = result["top_level"].get(name, 0) + 1
    walk(0, len(data))
    return result


def frames(path):
    p = run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_frames",
             "-show_entries", "frame=width,height,best_effort_timestamp_time",
             "-of", "json", str(path)])
    rows = json.loads(p.stdout)["frames"]
    transitions = []
    for row in rows:
        size = [row["width"], row["height"]]
        if not transitions or transitions[-1]["size"] != size:
            transitions.append({"size": size, "at": row.get("best_effort_timestamp_time")})
    return {"count": len(rows), "transitions": transitions, "probe_stderr": p.stderr.decode()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "artifacts/continuous-quality-probe")
    args = parser.parse_args()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if not FFMPEG or not FFPROBE:
        raise SystemExit("ffmpeg and ffprobe are required")
    segments = []
    for i, (size, colour, level) in enumerate([
        ("640x360", "blue", "3.0"), ("1280x720", "green", "3.1"),
        ("1920x1080", "red", "4.0"), ("640x360", "blue", "3.0"),
    ]):
        path = root / ("source-%d.ts" % i)
        run([FFMPEG, "-v", "error", "-y", "-f", "lavfi", "-i",
             "color=c=%s:s=%s:r=15:d=2" % (colour, size), "-f", "lavfi", "-i",
             "sine=frequency=440:sample_rate=8000:duration=2", "-c:v", "libx264",
             "-profile:v", "baseline", "-level:v", level, "-preset", "ultrafast",
             "-tune", "zerolatency", "-bf", "0", "-g", "15", "-sc_threshold", "0",
             "-x264-params", "aud=1:repeat-headers=1", "-c:a", "aac", "-ar", "8000",
             "-ac", "1", "-b:a", "16k", "-f", "mpegts", str(path)])
        segments.append(path)
    listing = root / "concat.txt"
    listing.write_text("".join("file '%s'\n" % p.name for p in segments))
    source = root / "changing.ts"
    run([FFMPEG, "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i",
         str(listing), "-c", "copy", "-f", "mpegts", str(source)])
    # Exact arguments from server_web.c:live_stream (output file is stdout).
    p = run([FFMPEG, "-nostdin", "-v", "error", "-protocol_whitelist", "pipe",
             "-analyzeduration", "500000", "-probesize", "131072", "-f", "mpegts",
             "-i", "pipe:0", "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "copy",
             "-c:a", "aac", "-ar", "48000", "-ac", "1", "-movflags",
             "frag_keyframe+empty_moov+default_base_moof", "-frag_duration", "250000",
             "-flush_packets", "1", "-f", "mp4", "pipe:1"], input=source.read_bytes())
    output = root / "production-remux.mp4"
    output.write_bytes(p.stdout)
    report = {
        "scope": "synthetic offline remux; not device or MSE acceptance",
        "ffmpeg": run([FFMPEG, "-version"]).stdout.decode().splitlines()[0],
        "source": frames(source), "remux": frames(output), "mp4": metadata(p.stdout),
        "remux_stderr": p.stderr.decode(),
    }
    (root / "probe-result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

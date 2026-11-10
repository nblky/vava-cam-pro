#!/usr/bin/env python3
"""SDK clip commits, actual A/V decoding, timestamps and failure recovery."""
import hashlib
import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path

import sdk_integration as sdk


class ClipTests(sdk.SDKTests):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
            raise unittest.SkipTest("ffmpeg/ffprobe are needed for independent media validation")
        cls.fixtures = tempfile.TemporaryDirectory(prefix="myvava-av-")
        root = Path(cls.fixtures.name)
        video = root / "source.h264"
        audio = root / "source.aac"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=c=blue:s=128x96:r=15:d=1",
            "-c:v", "libx264", "-profile:v", "baseline", "-x264-params", "aud=1:keyint=15:repeat-headers=1",
            "-f", "h264", str(video)], check=True, timeout=20)
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=8000:duration=1",
            "-c:a", "aac", "-b:a", "16k", "-ac", "1", "-f", "adts", str(audio)], check=True, timeout=20)
        raw = video.read_bytes()
        units = [b"\0\0\0\1\x09" + x for x in raw.split(b"\0\0\0\1\x09")[1:]]
        cls.frames = [(round(i * 1000 / 15), 1 if i == 0 else 0, p) for i, p in enumerate(units)]
        raw, off, i = audio.read_bytes(), 0, 0
        while off < len(raw):
            n = ((raw[off+3] & 3) << 11) | (raw[off+4] << 3) | (raw[off+5] >> 5)
            cls.frames.append((i * 128, 8, raw[off:off+n]))
            off += n
            i += 1
        cls.frames.sort(key=lambda x: (x[0], x[1] == 8))

    @classmethod
    def tearDownClass(cls):
        cls.fixtures.cleanup()

    def begin(self):
        self.connect_camera()
        self.camera.auto_media = False
        result = sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record 0 start")
        self.assertTrue(result["ok"], result)
        self.record_id = result["id"]
        self.wait(lambda: self.camera.live)

    def send_frames(self, frames, stream=1):
        for ms, kind, payload in frames:
            audio = kind == 8
            header = struct.pack("<I8BIQ", 0xEB000002, stream+8 if audio else stream, 3 if audio else 0,
                                 kind, 8 if audio else 15, 0, 1, 0, 0, len(payload), 1000000 + ms)
            self.camera.av.sendall(header + payload)
            self.camera.step(.001)

    def finish(self):
        sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record 0 stop")
        self.wait(lambda: not self.local_status()["devices"][0]["recording"]["finishing"])
        return sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")

    def probe(self, path):
        return json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_streams", "-show_packets",
                                                   "-of", "json", str(path)], timeout=20))

    def verify_clip(self, row, frames):
        path = self.root / "station/recordings" / ("vrec-" + row["id"] + ".ts")
        self.assertEqual(row["format"], "mpegts-h264-aac")
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), row["sha256"])
        self.assertEqual(path.stat().st_size, row["bytes"])
        decoded = self.probe(path)
        self.assertEqual({s["codec_name"] for s in decoded["streams"]}, {"h264", "aac"})
        self.assertEqual(next(s for s in decoded["streams"] if s["codec_name"] == "aac")["sample_rate"], "8000")
        for track in ("video", "audio"):
            expected = [ms for ms, kind, _ in frames if (kind == 8) == (track == "audio")]
            packets = [p for p in decoded["packets"] if p["codec_type"] == track]
            self.assertEqual(len(packets), len(expected))
            for packet, ms in zip(packets, expected):
                self.assertAlmostEqual(float(packet["pts_time"]), 1 + ms/1000, places=5)
        p = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(path), "-map", "0:v", "-map", "0:a",
                            "-f", "null", "-"], capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr.decode())
        self.assertFalse(p.stderr, p.stderr.decode())
        return path

    def test_av_commit_digest_events_and_restart(self):
        self.begin()
        # Frames from the other logical stream must not enter this clip.
        self.send_frames([(0, 8, b"wrong-track")], stream=0)
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        row = self.finish()["records"][0]
        self.assertEqual(row["frames"], 15)
        self.assertEqual(row["audio_frames"], len([x for x in self.frames if x[1] == 8]))
        self.verify_clip(row, self.frames)
        self.drain_events()
        events = self.event_page()["events"]
        self.assertTrue(any(e["event"]["kind"] == "recording_committed" and e["event"]["related_id"] == row["id"] for e in events))
        self.camera.close(); self.camera = None
        self.stop_proc(self.station)
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status"))
        rows = sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")["records"]
        self.assertEqual(rows, [row])
        self.drain_events(seconds=30)
        self.assertFalse(any(e["event"]["kind"] == "recording_recovered" and e["event"]["related_id"] == row["id"]
                             for e in self.event_page()["events"]))
        marker=self.root/"station/recordings"/("vrec-"+row["id"]+".evtmark")
        self.assertTrue(marker.is_file())
        self.stop_proc(self.station);marker.unlink()
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,"status"))
        self.wait(lambda:marker.exists(),30)
        self.drain_events(seconds=30)
        self.assertTrue(any(e["event"]["kind"]=="recording_recovered" and e["event"]["related_id"]==row["id"] for e in self.event_page()["events"]))

    def test_vendor_decrypted_sample_preserves_all_timestamps(self):
        source = sdk.ROOT.parent / "固件分析/recording_validation/124437_U_0/clear.vava"
        if not source.exists():
            self.skipTest("local decrypted vendor sample unavailable")
        raw, off, frames = source.read_bytes(), 16, []
        while off < len(raw):
            magic, n, kind, hint, sec, ms = struct.unpack_from("<6I", raw, off)
            self.assertEqual(magic, 0xeb0000aa)
            frames.append((sec*1000+ms, kind, raw[off+24:off+24+n]))
            off += 24+n
        self.begin()
        self.send_frames(frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 146)
        row = self.finish()["records"][0]
        self.assertEqual((row["frames"], row["audio_frames"]), (146, 78))
        self.verify_clip(row, frames)

    def test_malformed_audio_and_timestamp_reset_fail_without_commit(self):
        self.begin()
        self.send_frames(self.frames[:3])
        self.send_frames([(50, 8, b"invalid-adts")])
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["failed"])
        self.assertEqual(self.finish()["total"], 0)
        self.drain_events()
        events = self.event_page()["events"]
        self.assertTrue(any(e["event"]["kind"] == "recording_failed" for e in events))
        self.assertFalse(any(e["event"]["kind"] == "recording_committed" for e in events))
        self.assertEqual(list((self.root / "station/recordings").glob("*.part")), [])
        self.assertTrue(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record 0 start")["ok"])
        self.send_frames([self.frames[0], (20, 0, self.frames[2][2]), (10, 0, self.frames[2][2])])
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["failed"])
        self.assertEqual(self.finish()["total"], 0)

    def test_metadata_failure_rolls_back_and_shutdown_commits(self):
        self.begin()
        directory = self.root / "station/recordings"
        obstacle = directory / ("vrec-" + self.record_id + ".meta-part")
        obstacle.mkdir()
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        self.assertEqual(self.finish()["total"], 0)
        self.assertEqual(list(directory.glob("*.ts")), [])
        self.assertEqual(list(directory.glob("*.meta")), [])
        obstacle.rmdir()
        self.assertTrue(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record 0 start")["ok"])
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        self.stop_proc(self.station)
        self.camera.close(); self.camera = None
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status"))
        row = sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")["records"][0]
        self.verify_clip(row, self.frames)

    def test_download_protects_file_and_rejects_truncated_source(self):
        self.begin()
        # Large bounded access units keep a local reader open while delete is requested.
        large = [(i*70, 1, self.frames[0][2] + b"\x55"*180000) for i in range(12)]
        self.send_frames(large)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 12)
        row = self.finish()["records"][0]
        peers = []
        try:
            for _ in range(4):
                peer = socket.socket(socket.AF_UNIX)
                peer.settimeout(4)
                peer.connect(str(self.station_socket))
                peer.sendall(("if-serial SIM-EVENT-0001 record-get 0 " + row["id"] + "\n").encode())
                peers.append(peer)
                header = bytearray()
                while not header.endswith(b"\n"):
                    header += peer.recv(1)
                result = json.loads(header)
                self.assertEqual(result["sha256"], row["sha256"])
                self.assertEqual(result["format"], "mpegts-h264-aac")
            denied = sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-get 0 " + row["id"])
            self.assertFalse(denied["ok"])
            self.assertFalse(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-delete 0 " + row["id"])["ok"])
        finally:
            for peer in peers:
                peer.close()
        # A changed size is refused before advertising a successful download.
        path = self.root / "station/recordings" / ("vrec-" + row["id"] + ".ts")
        path.write_bytes(b"truncated")
        self.wait(lambda: not sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-get 0 " + row["id"])["ok"])
        self.wait(lambda: sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-delete 0 " + row["id"])["ok"])

    def test_legacy_catalog_migration_and_corrupt_metadata_preserved(self):
        self.begin()
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        row = self.finish()["records"][0]
        self.camera.close(); self.camera = None
        self.stop_proc(self.station)
        directory = self.root / "station/recordings"
        legacy = bytearray(128)
        legacy[:8] = b"VAVAREC1"
        payload = self.frames[0][2]
        struct.pack_into("<6Q", legacy, 16, 42, 1, 1700000000, len(payload), 1, 256)
        serial = b"SIM-EVENT-0001\0"
        legacy[64:64+len(serial)] = serial
        struct.pack_into("<I", legacy, 8, zlib.crc32(legacy[16:]))
        for suffix, data in (("h264", payload), ("meta", legacy)):
            p = directory / ("vrec-000000000000002a." + suffix)
            p.write_bytes(data); p.chmod(0o600)
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status"))
        rows = sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")["records"]
        self.assertEqual({r["format"] for r in rows}, {"h264-annex-b", "mpegts-h264-aac"})
        self.stop_proc(self.station)
        meta = directory / ("vrec-" + row["id"] + ".meta")
        bad = bytearray(meta.read_bytes()); bad[128] ^= 1; meta.write_bytes(bad)
        self.allow_missing_storage=True
        self.station = self.start_station()
        self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.assertFalse(self.local_status()['storage']['available'])
        self.assertTrue(sdk.request(self.station_socket,'pair-open 30')['ok'])
        self.assertEqual(meta.read_bytes(), bad)
        self.assertTrue((directory / ("vrec-" + row["id"] + ".ts")).exists())

    def test_pir_uses_stream_zero_and_audio_eight(self):
        self.connect_camera()
        self.camera.auto_media = False
        self.assertTrue(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 arm 0 on 2")["ok"])
        first = bytearray(sdk.camera_sim.decode(sdk.camera_sim.first("SIM-EVENT-0001"))[1])
        first[19] = 1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5, first))
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["active"])
        self.send_frames([(0, 8, b"wrong-track")], stream=1)
        self.send_frames(self.frames, stream=0)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        self.assertTrue(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 arm 0 off")["ok"])
        self.wait(lambda: sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")["total"] == 1)
        row = sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")["records"][0]
        self.assertEqual((row["stream"], row["trigger"], row["end_reason"]), (0, "pir_first_connect", "disarmed"))
        self.verify_clip(row, self.frames)

    def test_video_only_clip_and_audio_before_first_idr(self):
        self.begin()
        self.send_frames([(0, 8, b"before-idr-is-not-recorded")])
        video = [f for f in self.frames if f[1] != 8]
        self.send_frames(video)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        row = self.finish()["records"][0]
        self.assertEqual((row["audio_frames"], row["sample_rate"], row["channels"]), (0, 0, 0))
        path = self.root / "station/recordings" / ("vrec-" + row["id"] + ".ts")
        self.assertEqual([s["codec_type"] for s in self.probe(path)["streams"]], ["video"])

    def test_ts_quota_failure_and_crash_orphan_cleanup(self):
        self.stop_proc(self.station)
        self.station_extra = ["--record-max-mb", "1"]
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status"))
        self.begin()
        self.send_frames([(i*70, 1, self.frames[0][2] + b"\x55"*180000) for i in range(8)])
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["failed"])
        listing = self.finish()
        self.assertEqual((listing["total"], listing["storage_bytes"]), (0, 0))
        self.assertTrue(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record 0 start")["ok"])
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        self.station.kill(); self.station.wait(timeout=5)
        self.camera.close(); self.camera = None
        directory = self.root / "station/recordings"
        parts = list(directory.glob("*.part"))
        self.assertEqual(len(parts), 1)
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status"))
        self.assertEqual(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record-list 0")["total"], 0)
        result = sdk.request(self.station_socket, "storage-cleanup")
        self.assertTrue(result["ok"])
        self.assertGreater(result["removed_bytes"], 0)
        self.assertFalse(parts[0].exists())

    def test_adts_bundles_keep_sample_timing_and_have_limits(self):
        self.begin()
        audio = [f for f in self.frames if f[1] == 8]
        grouped = [(audio[i][0], 8, b"".join(f[2] for f in audio[i:i+2])) for i in range(0, len(audio), 2)]
        grouped += [f for f in self.frames if f[1] != 8]
        grouped.sort(key=lambda f: (f[0], f[1] == 8))
        self.send_frames(grouped)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        row = self.finish()["records"][0]
        self.verify_clip(row, self.frames)
        self.assertTrue(sdk.request(self.station_socket, "if-serial SIM-EVENT-0001 record 0 start")["ok"])
        self.send_frames([self.frames[0], (0, 8, audio[0][2]*65)])
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["failed"])
        self.assertEqual(self.finish()["total"], 1)

    def test_verified_download_publishes_only_matching_digest(self):
        self.begin()
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()["devices"][0]["recording"]["frames"] == 15)
        row = self.finish()["records"][0]
        source = self.root / "station/recordings" / ("vrec-" + row["id"] + ".ts")
        command = [sys.executable, str(sdk.ROOT / "tools/clip_download.py"), "--socket", str(self.station_socket),
                   "--serial", "SIM-EVENT-0001", "0", row["id"]]
        output = self.root / "download.ts"
        result = subprocess.run(command + [str(output)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(output.read_bytes(), source.read_bytes())
        result = subprocess.run(command + [str(output)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        corrupted = bytearray(source.read_bytes()); corrupted[190] ^= 1; source.write_bytes(corrupted)
        rejected = self.root / "bad.ts"
        result = subprocess.run(command + [str(rejected)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"SHA-256 mismatch", result.stderr)
        self.assertFalse(rejected.exists())
        self.assertEqual(list(self.root.glob(".myvava-download-*")), [])


    def test_record_writes_stay_on_open_medium_after_path_replacement(self):
        self.begin();directory=self.root/'station/recordings';moved=self.root/'detached-recordings'
        directory.rename(moved);directory.mkdir(mode=0o700)
        self.send_frames(self.frames);self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15)
        row=self.finish()['records'][0]
        self.assertTrue((moved/('vrec-'+row['id']+'.ts')).is_file())
        self.assertEqual(list(directory.iterdir()),[])

if __name__ == "__main__":
    # Run only the new cases; the inherited transport suite is registered separately.
    suite = unittest.TestSuite(ClipTests(n) for n in ClipTests.__dict__ if n.startswith("test_"))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())

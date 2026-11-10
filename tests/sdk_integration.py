#!/usr/bin/env python3
"""Black-box DTLS/UDP, reconnect, duplicate and real device-dispatch tests.

Only synthetic credentials, loopback sockets and the camera wire simulator.
"""
import json
import secrets
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import camera_sim
from myvava_ctl import request

STATION = str(Path(sys.argv[1]).resolve())
SERVER = str(Path(sys.argv[2]).resolve())
sys.argv = sys.argv[:1] + sys.argv[3:]


class Proxy:
    def __init__(self, target):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(.05)
        self.port = self.sock.getsockname()[1]
        self.target = ("127.0.0.1", target)
        self.client = None
        self.stop = threading.Event()
        self.impair = False
        self.drop_all = False
        self.drop_from_server = False
        self.drop_server_when = None
        self.drop_client_when = None
        self.frames = []
        self.from_server = []
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        count = 0
        while not self.stop.is_set():
            try:
                data, source = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            count += 1
            self.frames.append(data)
            if source == self.target:
                destination = self.client
                self.from_server.append(data)
            else:
                self.client = source
                destination = self.target
            if not destination or self.drop_all or (self.drop_from_server and source == self.target) or (self.impair and count % 7 == 0):
                continue
            if source == self.target and self.drop_server_when and self.drop_server_when():
                continue
            if source != self.target and self.drop_client_when and self.drop_client_when(data):
                continue
            self.sock.sendto(data, destination)
            if self.impair and count % 3 == 0:
                self.sock.sendto(data, destination)

    def replay_responses(self):
        if self.client:
            for data in list(self.from_server):
                self.sock.sendto(data, self.client)

    def close(self):
        self.stop.set()
        self.thread.join(2)
        self.sock.close()


class SDKTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="myvava-", dir="/tmp")
        self.root = Path(self.tmp.name)
        self.procs = []
        self.logs = []
        self.camera = None
        self.event_limit = 1024
        self.event_history_limit = 8192
        self.history_limit = 1024
        self.window_seconds = 1
        self.secret = secrets.token_hex(32)
        self.second_secret = secrets.token_hex(32)
        self.key = self.private("device.key", self.secret + "\n")
        self.keys = self.private("devices.keys", "BASE_A " + self.secret + "\nBASE_B " + self.second_secret + "\n")
        self.private("wifi.key", "SyntheticWifiPass\n")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.proxy = Proxy(self.port)
        self.addCleanup(self.cleanup)
        self.server_socket = self.root / "server/server.sock"
        self.station_socket = self.root / "station/control.sock"
        self.server = self.start_server()
        self.station = self.start_station()
        self.online()

    def private(self, name, value):
        path = self.root / name
        path.write_text(value)
        path.chmod(0o600)
        return path

    def spawn(self, args):
        log = tempfile.TemporaryFile()
        proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=log)
        self.procs.append(proc)
        self.logs.append(log)
        return proc

    def start_server(self):
        proc = self.spawn([SERVER, "--keys", str(self.keys), "--state-dir", str(self.root / "server"), "--port", str(self.port),
                           "--event-history-limit", str(self.event_history_limit), "--history-limit", str(self.history_limit), "--request-window-seconds", str(self.window_seconds)] + getattr(self, "server_extra", []))
        def ready():
            if proc.poll() is not None:
                log = self.logs[-1]
                log.seek(0)
                self.fail("server exited during startup: " + log.read().decode(errors="replace"))
            return self.server_socket.exists() and self.try_request(self.server_socket, "devices")
        self.wait(ready)
        return proc

    def start_station(self, identity="BASE_A", key=None, state="station", wait_ready=True):
        events = self.root / (state + "-events")
        events.mkdir(mode=0o700, exist_ok=True)
        proc = self.spawn([STATION, "--state-dir", str(self.root / state), "--control-port", "0", "--media-port", "0",
            "--alive-port", "0", "--diagnostic-defaults", "1", "--password-file", str(self.root / "wifi.key"), "--lab-ip-association",
            "--server-host", "localhost", "--server-port", str(self.proxy.port if state == "station" else self.port), "--server-poll-ms", "50",
            "--device-id", identity, "--server-key-file", str(key or self.key),
            "--event-dir", str(events), "--event-limit", str(self.event_limit)] + getattr(self, "station_extra", []))
        if wait_ready and not getattr(self, "allow_missing_storage", False):
            path=self.root/state/'control.sock'
            def ready_storage():
                if proc.poll() is not None:
                    self.logs[-1].seek(0);self.fail("station exited: "+self.logs[-1].read().decode(errors="replace"))
                status=self.try_request(path,'status')
                if not status:return False
                workers=status.get('storage_workers',{})
                return status.get('storage',{}).get('available') and workers.get('upload_ready') and (not workers.get('nas_configured') or workers.get('nas_ready'))
            self.wait(ready_storage)
        return proc

    def try_request(self, path, command):
        try:
            return request(path, command)
        except (OSError, EOFError):
            return None

    def wait(self, predicate, seconds=12):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.camera:
                self.camera.step(.005)
            value = predicate()
            if value:
                return value
            time.sleep(.02)
        diagnostic = {}
        try:
            status = request(self.station_socket, "status")
            diagnostic = {k: status.get(k) for k in ("event_spool", "server_connection")}
            diagnostic["server"] = self.snapshot()
        except (OSError, EOFError):
            pass
        self.fail("condition timed out: " + json.dumps(diagnostic))

    def snapshot(self):
        return request(self.server_socket, "devices")

    def online(self, seconds=12):
        return self.wait(lambda: next((d for d in self.snapshot()["devices"] if d["id"] == "BASE_A" and d["online"]), None), seconds)

    def submit(self, command, key=None, snapshot=None, boot=None, identity="BASE_A"):
        snapshot = snapshot or self.snapshot()
        device = next(d for d in snapshot["devices"] if d["id"] == identity)
        return request(self.server_socket, " ".join(["submit", identity, boot or device["boot"], snapshot["epoch"],
            key or snapshot["request_prefix"] + secrets.token_hex(8), command]))

    def result(self, task_id, seconds=15, epoch=None):
        epoch = epoch or self.snapshot()["epoch"]
        def finished():
            data = request(self.server_socket, "result " + epoch + " " + task_id)
            return data if data.get("state") == "completed" else None
        data = self.wait(finished, seconds)
        return json.loads(data["result"])

    def command(self, text, seconds=15):
        task = self.submit(text)
        self.assertTrue(task["ok"], task)
        return self.result(task["id"], seconds)

    def stop_proc(self, proc):
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=6)
        self.assertEqual(proc.returncode, 0)

    def crash_server(self):
        self.server.kill()
        self.server.wait(timeout=5)

    def database(self, statement):
        db = sqlite3.connect(self.root / "server/tasks.sqlite3", timeout=2)
        try:
            db.executescript(statement)
        finally:
            db.close()

    def task_state(self, task):
        return request(self.server_socket, "result " + task["epoch"] + " " + task["id"])

    def assert_pairing_not_restarted(self, before):
        after = request(self.station_socket, "status")
        self.assertEqual(after["pairing"]["id"], before["pairing"]["id"])
        self.assertEqual(after["pairing"]["created_unix_ms"], before["pairing"]["created_unix_ms"])
        # Reconnection can outlast the original window: zero must stay zero.
        # A new operation ID also catches a replay before its AP becomes ready.
        self.assertLessEqual(after["pair_window_ms"], max(0, before["pair_window_ms"] - 1))

    def cleanup(self):
        if self.camera:
            self.camera.close()
        for proc in reversed(self.procs):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        self.proxy.close()
        for log in self.logs:
            log.seek(0)
            text = log.read().decode(errors="replace")
            self.assertNotIn(self.secret, text)
            self.assertNotIn("ERROR: AddressSanitizer", text)
            self.assertNotIn("runtime error:", text)
            log.close()
        self.tmp.cleanup()

    def event_page(self, before="0000000000000000"):
        return request(self.server_socket, "events BASE_A " + before)

    def local_status(self):
        return request(self.station_socket, "status")

    def connect_camera(self):
        status = self.local_status()
        self.assertTrue(request(self.station_socket, "pair-open 30")["ok"])
        self.camera = camera_sim.Camera("127.0.0.1", status["ports"], "SIM-EVENT-0001", b"\0\0\0\1\x65test", True)

    def report(self, result, kind=123):
        self.camera.ctrl.sendall(camera_sim.control(200, struct.pack("<II", result, kind)))

    def event_files(self):
        return sorted((self.root / "station-events").glob("*.evt"))

    def drain_events(self, seconds=20):
        return self.wait(lambda: self.local_status()["event_spool"]["retained"] == 0 and
                         self.local_status()["event_spool"]["queued"] == 0, seconds)

    def crash_station(self):
        if self.camera:
            self.camera.close()
            self.camera = None
        self.station.kill()
        self.station.wait(timeout=5)

    def test_events_survive_offline_station_and_server_restart(self):
        self.connect_camera()
        self.drain_events()
        self.proxy.drop_all = True
        for _ in range(3):
            self.report(7, 0xabcdef01)
        self.wait(lambda: len(self.event_files()) == 3)
        pending = [p.read_bytes() for p in self.event_files()]
        self.assertTrue(all(len(p) == 164 for p in pending))
        self.crash_station()
        self.station = self.start_station()
        self.proxy.drop_all = False
        self.wait(lambda: sum(e["event"]["kind"] == "camera_report_200" for e in self.event_page()["events"]) == 3, 60)
        self.drain_events()
        events = self.event_page()["events"]
        reports = [e["event"] for e in events if e["event"]["kind"] == "camera_report_200"]
        self.assertEqual(len({(e["origin"], e["sequence"]) for e in reports}), 3)
        self.assertTrue(all(e["result_raw"] == 7 and e["type_raw"] == 0xabcdef01 for e in reports))
        self.assertTrue(all(e["serial"] == "SIM-EVENT-0001" and not e["clock_valid"] for e in reports))
        self.crash_server()
        self.server = self.start_server()
        self.assertEqual(events, self.event_page()["events"])

    def test_event_commit_failure_keeps_spool_and_control_usable(self):
        self.connect_camera()
        self.drain_events()
        self.database("CREATE TRIGGER fail_event BEFORE INSERT ON events BEGIN SELECT RAISE(FAIL, 'synthetic disk failure'); END;")
        self.report(81)
        self.wait(lambda: self.local_status()["server_connection"]["event_rejections"] > 0)
        self.assertEqual(self.local_status()["event_spool"]["retained"], 1)
        self.assertFalse(any(e["event"]["result_raw"] == 81 for e in self.event_page()["events"]))
        self.assertTrue(self.command("status")["ok"])
        self.database("DROP TRIGGER fail_event;")
        self.wait(lambda: any(e["event"]["result_raw"] == 81 for e in self.event_page()["events"]))
        self.drain_events()

    def test_event_ack_loss_pruning_and_restored_spool_are_deduplicated(self):
        self.event_history_limit = 16
        self.stop_proc(self.server)
        self.server = self.start_server()
        self.online(seconds=40)
        self.command("status", seconds=35)
        self.connect_camera()
        self.drain_events()
        self.proxy.drop_all = True
        self.report(900)
        self.wait(lambda: len(self.event_files()) == 1)
        path = self.event_files()[0]
        name, saved = path.name, path.read_bytes()
        def stored():
            with sqlite3.connect(self.root / "server/tasks.sqlite3") as db:
                return db.execute("SELECT count(*) FROM events WHERE hex(substr(payload,69,4))='00000384'").fetchone()[0] > 0
        self.proxy.drop_server_when = stored
        self.proxy.drop_all = False
        self.wait(stored, 35)
        self.assertTrue(path.exists())
        self.proxy.drop_server_when = None
        self.drain_events(35)
        for i in range(40):
            self.report(1000 + i)
        self.wait(lambda: any(e["event"]["result_raw"] == 1039 for e in self.event_page()["events"]), 20)
        self.drain_events()
        page = self.event_page()
        self.assertEqual(len(page["events"]), 16)
        self.assertEqual(self.event_page(page["next_before"])["events"], [])
        self.assertGreater(self.snapshot()["event_journal"]["pruned"], 0)
        self.assertFalse(any(e["event"]["result_raw"] == 900 for e in page["events"]))
        self.crash_station()
        restored = self.root / "station-events" / name
        restored.write_bytes(saved)
        restored.chmod(0o600)
        self.station = self.start_station()
        self.wait(lambda: not restored.exists(), 60)
        self.assertFalse(any(e["event"]["result_raw"] == 900 for e in self.event_page()["events"]))

    def test_event_queue_overflow_reports_gap_without_blocking_camera(self):
        self.event_limit = 16
        boot = self.snapshot()["devices"][0]["boot"]
        self.stop_proc(self.station)
        self.station = self.start_station()
        self.wait(lambda: self.snapshot()["devices"][0]["boot"] != boot, 40)
        self.command("status", seconds=35)
        self.connect_camera()
        self.drain_events()
        self.proxy.drop_all = True
        for i in range(250):
            self.report(i)
        self.wait(lambda: self.local_status()["devices"][0]["detection_reports"] == 250)
        status = self.local_status()["event_spool"]
        self.assertLessEqual(status["retained"], 16)
        self.assertLessEqual(status["queued"], 64)
        self.assertGreater(status["dropped_since_boot"], 0)
        self.assertTrue(request(self.station_socket, "pair-close")["ok"])
        self.proxy.drop_all = False
        self.wait(lambda: any(e["event"]["kind"] == "journal_gap" for e in self.event_page()["events"]), 45)
        self.drain_events(30)
        gaps = [e for e in self.event_page()["events"] if e["event"]["kind"] == "journal_gap"]
        self.assertTrue(any(e["sequence_gap"] > 0 for e in gaps))
        self.assertGreaterEqual(gaps[0]["event"]["dropped_since_boot"], status["dropped_since_boot"])

    def test_corrupt_event_spool_preserved_and_reported(self):
        self.connect_camera()
        self.drain_events()
        self.proxy.drop_all = True
        self.report(18)
        self.wait(lambda: len(self.event_files()) == 1)
        path = self.event_files()[0]
        self.crash_station()
        corrupt = bytearray(path.read_bytes())
        corrupt[70] ^= 1
        path.write_bytes(corrupt)
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status") and self.local_status()["event_spool"]["error"])
        self.assertEqual(self.local_status()["event_spool"]["error"], "event_spool_corrupt_or_over_limit")
        self.assertFalse(self.local_status()["event_spool"]["ready"])
        self.assertEqual(path.read_bytes(), corrupt)
        self.assertTrue(request(self.station_socket, "pair-open 10")["ok"])

    def test_event_spool_io_failure_recovers_without_acknowledging_ram(self):
        self.connect_camera()
        self.drain_events()
        directory = self.root / "station-events"
        directory.chmod(0o500)
        self.addCleanup(lambda: directory.chmod(0o700))
        self.report(61)
        self.wait(lambda: self.local_status()["event_spool"]["error"] == "event_write_failed")
        status = self.local_status()["event_spool"]
        self.assertEqual(status["retained"], 0)
        self.assertGreater(status["queued"], 0)
        self.assertFalse(any(e["event"]["result_raw"] == 61 for e in self.event_page()["events"]))
        self.assertTrue(self.command("pair-close")["ok"])
        directory.chmod(0o700)
        self.wait(lambda: any(e["event"]["result_raw"] == 61 for e in self.event_page()["events"]))
        self.drain_events()
        self.assertEqual(self.local_status()["event_spool"]["error"], "")

    def test_event_schema_migrates_v2_without_losing_control_history(self):
        task = self.submit("pair-open 30")
        original = self.result(task["id"])
        self.stop_proc(self.station)
        self.stop_proc(self.server)
        self.database("DROP TABLE events; DROP TABLE event_streams; DROP TABLE event_stats; DROP TABLE clip_states; PRAGMA user_version=2;")
        self.server = self.start_server()
        restored = self.task_state(task)
        self.assertEqual(json.loads(restored["result"]), original)
        self.assertEqual(self.event_page()["events"], [])
        self.station = self.start_station()
        self.wait(lambda: self.event_page()["events"])
        self.assertEqual(self.snapshot()["ipc_version"], 3)

    def test_event_spool_symlink_refused_without_touching_target(self):
        boot = self.snapshot()["devices"][0]["boot"]
        self.drain_events()
        self.stop_proc(self.station)
        directory = self.root / "station-events"
        target = self.root / "external-events"
        directory.rename(target)
        sentinel = target / "keep.txt"
        sentinel.write_text("preserve")
        directory.symlink_to(target, target_is_directory=True)
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, "status") and self.local_status()["event_spool"]["error"])
        self.assertEqual(self.local_status()["event_spool"]["error"], "event_directory_unavailable_or_unsafe")
        self.assertEqual(sentinel.read_text(), "preserve")
        self.wait(lambda: self.snapshot()["devices"][0]["boot"] != boot, 40)
        self.assertTrue(self.command("pair-open 10")["ok"])

    def test_control_without_http_and_camera_setting(self):
        cap = self.command("capabilities")
        self.assertTrue(cap["control"])
        self.assertTrue(cap["media"])
        status = self.command("status")
        self.assertFalse(status["http"]["enabled"])
        self.assertTrue(self.command("pair-open 30")["ok"])
        self.camera = camera_sim.Camera("127.0.0.1", status["ports"], "SIM-UDP-0001", b"\0\0\0\1\x65test", True)
        paired = self.command("status")
        self.assertEqual(paired["devices"][0]["serial"], "SIM-UDP-0001")
        result = self.command("configure 0 mirror 3")
        self.assertEqual(result["error"], "camera_identity_required")
        result = self.command("if-serial WRONG configure 0 mirror 3")
        self.assertEqual(result["error"], "camera_identity_changed")
        result = self.command("if-serial SIM-UDP-0001 configure 0 mirror 3")
        self.assertTrue(result["station_saved"])
        self.wait(lambda: 11 in self.camera.events)
        result = self.command("if-serial SIM-UDP-0001 settings 0")
        self.assertTrue(result["ok"])
        # Camera emits no fake setting confirmation; saved != applied.
        self.assertIsNone(result.get("camera_applied"))
        unsupported = self.command("stream 0 1")
        self.assertEqual(unsupported["error"], "use_sdk_media_or_file_channel")
        for frame in self.proxy.frames:
            self.assertNotIn(b"configure 0 mirror 3", frame)
            self.assertNotIn(self.secret.encode(), frame)

    def test_deduplication_and_encrypted_packet_replay(self):
        key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        first = self.submit("pair-open 30", key)
        original = self.result(first["id"])
        again = self.submit("pair-open 30", key)
        self.assertEqual(first["id"], again["id"])
        self.assertTrue(again["duplicate"])
        self.assertEqual(original, self.result(again["id"]))
        conflict = self.submit("pair-open 60", key)
        self.assertEqual(conflict["error"], "request_key_conflict")
        before = request(self.station_socket, "status")
        self.proxy.replay_responses()
        time.sleep(.3)
        self.assert_pairing_not_restarted(before)

    def test_packet_loss_duplicates_and_partition_recovery(self):
        self.proxy.impair = True
        self.assertTrue(self.command("status", seconds=25)["ok"])
        self.proxy.impair = False
        self.proxy.drop_all = True
        task = self.submit("pair-open 20")
        self.assertTrue(task["ok"])
        time.sleep(.3)
        # Local device loop and maintenance remain responsive during failure.
        self.assertTrue(request(self.station_socket, "status")["ok"])
        self.proxy.drop_all = False
        self.assertTrue(self.result(task["id"], seconds=25)["ok"])

    def test_wrong_key_and_unregistered_identity(self):
        wrong = self.private("wrong.key", secrets.token_hex(32) + "\n")
        self.start_station("BASE_B", wrong, "bad-key")
        self.start_station("UNREGISTERED", self.key, "bad-id")
        time.sleep(1)
        snapshot = self.snapshot()
        self.assertFalse(next(d for d in snapshot["devices"] if d["id"] == "BASE_B")["online"])
        self.assertEqual(len(snapshot["devices"]), 2)
        self.assertTrue(self.command("status")["ok"])

    def test_two_devices_have_isolated_commands_and_results(self):
        key = self.private("second.key", self.second_secret + "\n")
        self.start_station("BASE_B", key, "second")
        self.wait(lambda: self.snapshot()["devices"][1]["online"])
        self.assertTrue(self.command("pair-open 30")["ok"])
        task = self.submit("status", identity="BASE_B")
        self.assertTrue(task["ok"])
        self.assertEqual(self.result(task["id"])["pair_window_ms"], 0)
        self.assertGreater(self.command("status")["pair_window_ms"], 0)

    def test_key_permissions_and_reused_keys_are_rejected(self):
        unsafe = self.private("unsafe.key", self.secret + "\n")
        unsafe.chmod(0o644)
        station = self.start_station("BASE_A", unsafe, "bad-permissions", wait_ready=False)
        self.assertEqual(station.wait(timeout=4), 1)
        duplicate = self.private("duplicate.keys", "A " + self.secret + "\nB " + self.secret + "\n")
        result = subprocess.run([SERVER, "--keys", str(duplicate), "--state-dir", str(self.root / "duplicate")],
                                capture_output=True, timeout=4)
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.root / "duplicate/server.sock").exists())

    def test_station_restart_invalidates_old_boot(self):
        old = self.snapshot()
        old_boot = old["devices"][0]["boot"]
        self.stop_proc(self.station)
        self.station = self.start_station()
        self.wait(lambda: self.snapshot()["devices"][0]["boot"] != old_boot)
        rejected = self.submit("pair-open 20", snapshot=old)
        self.assertEqual(rejected["error"], "device_boot_changed")
        self.assertTrue(self.command("status")["ok"])

    def test_server_restart_invalidates_old_epoch(self):
        old = self.snapshot()
        old_task = self.submit("pair-open 20")
        self.assertTrue(self.result(old_task["id"])["ok"])
        self.stop_proc(self.server)
        self.server = self.start_server()
        rejected = self.submit("pair-open 20", snapshot=old)
        self.assertEqual(rejected["error"], "server_epoch_changed")
        self.online(seconds=40)
        new_task = self.submit("status")
        self.assertGreater(int(new_task["id"], 16), int(old_task["id"], 16))
        self.assertTrue(self.result(new_task["id"])["ok"])
        stale = request(self.server_socket, "result " + old_task["epoch"] + " " + old_task["id"])
        self.assertEqual(stale["state"], "completed")
        self.assertTrue(stale["durable"])
        self.assertEqual(request(self.server_socket, "result " + old_task["epoch"] + " " + new_task["id"])["error"],
                         "task_not_retained")
        self.assertEqual(request(self.server_socket, "result " + old_task["id"])["error"],
                         "result_requires_epoch_and_task_id")
        # The CLI also requires the original epoch and must not fetch a new one.
        cli = subprocess.run([sys.executable, str(ROOT / "tools/myvava_ctl.py"), "--socket", str(self.server_socket),
                              "result", "--epoch", old_task["epoch"], old_task["id"]], capture_output=True, timeout=5)
        self.assertEqual(cli.returncode, 0)
        self.assertTrue(json.loads(cli.stdout)["result"]["ok"])
        cli = subprocess.run([sys.executable, str(ROOT / "tools/myvava_ctl.py"), "--socket", str(self.server_socket),
                              "run", "BASE_A", "status"], capture_output=True, timeout=10)
        self.assertEqual(cli.returncode, 0, cli.stderr)
        completed = json.loads(cli.stdout)
        self.assertEqual(completed["epoch"], new_task["epoch"])
        self.assertTrue(completed["result"]["ok"])

    def test_http_options_are_not_present_in_sdk_target(self):
        result = subprocess.run([STATION, "--http-port", "8080"], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn(b"--http-port", result.stderr)
        # Polling cannot exceed a third of the server's 15-second online window.
        result = subprocess.run([STATION, "--server-poll-ms", "5001"], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 2)

    def test_completed_result_survives_sigkill_and_registry_reordering(self):
        key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        task = self.submit("pair-open 30", key)
        original = self.result(task["id"])
        self.crash_server()
        self.keys.write_text("BASE_B " + self.second_secret + "\nBASE_A " + self.secret + "\n")
        self.server = self.start_server()
        recovered = request(self.server_socket, "lookup BASE_A " + key)
        self.assertEqual(recovered["epoch"], task["epoch"])
        self.assertEqual(json.loads(recovered["result"]), original)
        self.assertEqual(request(self.server_socket, "lookup BASE_B " + key)["error"], "task_not_retained")
        self.online(seconds=40)
        before = request(self.station_socket, "status")
        duplicate = self.submit("pair-open 30", key)
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual((duplicate["epoch"], duplicate["id"]), (task["epoch"], task["id"]))
        time.sleep(.15)
        self.assert_pairing_not_restarted(before)

    def test_queued_command_is_not_replayed_after_sigkill(self):
        self.proxy.drop_all = True
        time.sleep(.1)
        key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        task = self.submit("pair-open 30", key)
        self.assertEqual(self.task_state(task)["state"], "queued")
        self.crash_server()
        self.server = self.start_server()
        restored = self.task_state(task)
        self.assertEqual(restored["state"], "expired")
        self.assertEqual(restored["reason"], "server_restarted_before_dispatch")
        self.proxy.drop_all = False
        self.online(seconds=40)
        duplicate = self.submit("pair-open 30", key)
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(duplicate["id"], task["id"])
        self.assertEqual(request(self.station_socket, "status")["pair_window_ms"], 0)

    def test_executed_command_with_lost_result_stays_unknown_after_crash(self):
        self.database("CREATE TRIGGER reject_result BEFORE UPDATE OF result ON tasks BEGIN SELECT RAISE(FAIL,'simulated write failure'); END;")
        key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        task = self.submit("pair-open 30", key)
        self.wait(lambda: request(self.station_socket, "status")["pair_window_ms"] > 0)
        self.assertEqual(self.task_state(task)["state"], "dispatched")
        self.crash_server()
        self.database("DROP TRIGGER reject_result;")
        self.server = self.start_server()
        restored = self.task_state(task)
        self.assertEqual(restored["state"], "unknown")
        self.assertEqual(restored["reason"], "server_restarted_after_dispatch")
        self.assertIsNone(restored["result"])
        self.online(seconds=40)
        before = request(self.station_socket, "status")
        self.assertTrue(self.submit("pair-open 30", key)["duplicate"])
        time.sleep(.2)
        self.assert_pairing_not_restarted(before)

    def test_dispatch_must_be_committed_before_camera_effect(self):
        self.database("CREATE TRIGGER reject_dispatch BEFORE UPDATE OF state ON tasks WHEN NEW.state=1 BEGIN SELECT RAISE(FAIL,'simulated write failure'); END;")
        task = self.submit("pair-open 30")
        time.sleep(.6)
        self.assertEqual(self.task_state(task)["state"], "queued")
        self.assertEqual(request(self.station_socket, "status")["pair_window_ms"], 0)
        self.database("DROP TRIGGER reject_dispatch;")
        self.assertTrue(self.result(task["id"])["ok"])

    def test_failed_submission_has_no_ack_or_camera_effect(self):
        key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        self.database("CREATE TRIGGER reject_new BEFORE INSERT ON tasks BEGIN SELECT RAISE(FAIL,'simulated storage failure'); END;")
        failed = self.submit("pair-open 30", key)
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["error"], "storage_unavailable")
        self.assertEqual(self.snapshot()["journal"]["retained"], 0)
        self.assertEqual(request(self.station_socket, "status")["pair_window_ms"], 0)
        self.database("DROP TRIGGER reject_new;")
        task = self.submit("pair-open 30", key)
        self.assertTrue(self.result(task["id"])["ok"])

    def test_partial_committed_result_is_not_reported_as_completed(self):
        self.assertTrue(self.command("pair-open 30")["ok"])
        status = request(self.station_socket, "status")
        self.camera = camera_sim.Camera("127.0.0.1", status["ports"], "SIM-JOURNAL-0001", b"\0\0\0\1\x65test", True)
        # Four camera rows exceed a negotiated 6144-byte result batch. Reject
        # the next durable batch, rather than a transport-level Block1 ACK.
        for i in range(2,5):
            self.assertTrue(request(self.station_socket,"pair-open 30")["ok"])
            camera=camera_sim.Camera("127.0.0.1",status["ports"],f"SIM-JOURNAL-000{i}",b"\0\0\0\1\x65test",True)
            self.addCleanup(camera.close)
        self.wait(lambda: len(request(self.station_socket,"status")["devices"])==4)
        self.database("CREATE TRIGGER reject_tail BEFORE UPDATE OF result ON tasks WHEN length(NEW.result)>6144 BEGIN SELECT RAISE(FAIL,'simulated tail failure'); END;")
        task = self.submit("status")
        def has_partial():
            db = sqlite3.connect(self.root / "server/tasks.sqlite3")
            try:
                row = db.execute("SELECT received,total FROM tasks WHERE id=?", (int(task["id"], 16),)).fetchone()
                return row and row[0] == 6144 and row[1] > 6144
            finally:
                db.close()
        self.wait(has_partial)
        self.assertIsNone(self.task_state(task)["result"])
        self.crash_server()
        self.database("DROP TRIGGER reject_tail;")
        self.server = self.start_server()
        restored = self.task_state(task)
        self.assertEqual(restored["state"], "unknown")
        self.assertIsNone(restored["result"])

    def test_history_beyond_64_pruning_and_expired_key_rejection(self):
        first_key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        first = self.submit("capabilities", first_key)
        self.assertTrue(self.result(first["id"])["ok"])
        for _ in range(79):
            self.assertTrue(self.command("capabilities")["ok"])
        self.assertEqual(self.snapshot()["journal"]["retained"], 80)
        page = request(self.server_socket, "history BASE_A 0000000000000000")
        self.assertEqual(len(page["tasks"]), 32)
        second = request(self.server_socket, "history BASE_A " + page["next_before"])
        self.assertEqual(len(second["tasks"]), 32)
        self.assertFalse({t["id"] for t in page["tasks"]} & {t["id"] for t in second["tasks"]})
        self.stop_proc(self.server)
        self.history_limit = 32
        self.server = self.start_server()
        self.online(seconds=40)
        self.database("CREATE TRIGGER reject_after_prune BEFORE INSERT ON tasks BEGIN SELECT RAISE(FAIL,'simulated insert failure'); END;")
        self.assertEqual(self.submit("capabilities")["error"], "storage_unavailable")
        self.assertEqual(self.snapshot()["journal"]["retained"], 80)
        self.assertEqual(self.snapshot()["journal"]["pruned"], 0)
        self.assertEqual(self.task_state(first)["state"], "completed")
        self.database("DROP TRIGGER reject_after_prune;")
        self.assertTrue(self.command("capabilities")["ok"])
        journal = self.snapshot()["journal"]
        self.assertEqual(journal["retained"], 32)
        self.assertEqual(journal["pruned"], 49)
        self.assertEqual(self.task_state(first)["error"], "task_not_retained")
        self.assertEqual(self.submit("capabilities", first_key)["error"], "request_key_expired_or_unissued")

    def test_live_request_windows_are_protected_from_history_recycling(self):
        self.stop_proc(self.server)
        self.history_limit = 16
        self.window_seconds = 60
        self.server = self.start_server()
        self.online(seconds=40)
        first_key = self.snapshot()["request_prefix"] + secrets.token_hex(8)
        first = self.submit("capabilities", first_key)
        self.assertTrue(self.result(first["id"])["ok"])
        for _ in range(15):
            self.assertTrue(self.command("capabilities")["ok"])
        full = self.submit("pair-open 30")
        self.assertEqual(full["error"], "history_full_with_protected_requests")
        self.assertEqual(request(self.station_socket, "status")["pair_window_ms"], 0)
        self.assertTrue(self.submit("capabilities", first_key)["duplicate"])
        self.assertEqual(self.snapshot()["journal"]["pruned"], 0)

    def test_lost_final_ack_protects_result_until_station_confirms(self):
        self.stop_proc(self.server)
        self.history_limit = 16
        self.server = self.start_server()
        self.online(seconds=40)
        def a_row():
            db = sqlite3.connect(self.root / "server/tasks.sqlite3")
            try:
                return db.execute("SELECT state,reclaimable FROM tasks WHERE device='BASE_A' ORDER BY id LIMIT 1").fetchone()
            finally:
                db.close()
        # Inspect the committed test journal to drop precisely the encrypted
        # final result ACK, while allowing the preceding command through.
        self.proxy.drop_server_when = lambda: (row := a_row()) is not None and row[0] == 2
        first = self.submit("capabilities")
        self.assertTrue(self.result(first["id"])["ok"])
        self.assertEqual(a_row(), (2, 0))
        second_key = self.private("ack-second.key", self.second_secret + "\n")
        self.start_station("BASE_B", second_key, "ack-second")
        self.wait(lambda: self.snapshot()["devices"][1]["online"])
        for _ in range(20):
            def submit_b():
                task = self.submit("capabilities", identity="BASE_B")
                if task["ok"]:
                    return task
                self.assertEqual(task["error"], "history_full_with_protected_requests")
                return None
            task = self.wait(submit_b, seconds=5)
            self.assertTrue(self.result(task["id"])["ok"])
        self.assertGreater(self.snapshot()["journal"]["pruned"], 0)
        self.assertEqual(self.task_state(first)["state"], "completed")
        self.assertEqual(a_row(), (2, 0))
        self.proxy.drop_server_when = None
        self.wait(lambda: a_row() == (2, 1), seconds=40)
        task = self.submit("capabilities", identity="BASE_B")
        self.assertTrue(self.result(task["id"])["ok"])
        self.assertEqual(self.task_state(first)["error"], "task_not_retained")
        # Once the confirmed result is pruned, control must still make progress.
        self.assertTrue(self.command("capabilities")["ok"])

    def test_bad_database_is_never_silently_reset(self):
        for kind in ("corrupt", "foreign", "permissions", "symlink"):
            state = self.root / ("bad-db-" + kind)
            state.mkdir(mode=0o700)
            path = state / "tasks.sqlite3"
            if kind == "symlink":
                path.symlink_to(self.key)
            elif kind == "foreign":
                db = sqlite3.connect(path)
                db.execute("CREATE TABLE unrelated(x)")
                db.commit()
                db.close()
                path.chmod(0o600)
            else:
                path.write_bytes(b"not a task database")
                path.chmod(0o644 if kind == "permissions" else 0o600)
            original = path.read_bytes()
            proc = self.spawn([SERVER, "--keys", str(self.keys), "--state-dir", str(state), "--port", str(self.port)])
            self.assertEqual(proc.wait(timeout=4), 1)
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse((state / "server.sock").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)

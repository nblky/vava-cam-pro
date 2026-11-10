#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('maintenance',ROOT/'tools/maintenance.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MaintenanceTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
        self.state=self.root/'state';self.state.mkdir(mode=0o700);self.keys=self.root/'keys';self.keys.mkdir(mode=0o700)
        m.write_new(self.state/'registry.bin',b'configuration');m.write_new(self.keys/'station-key',b'private synthetic key')
    def test_snapshot_restore_preserves_keys_and_never_overwrites(self):
        out=self.root/'snapshot';m.snapshot(self.state,self.keys,out);restored=self.root/'restored';m.restore(out,restored)
        self.assertEqual((restored/'credentials/station-key').read_bytes(),b'private synthetic key')
        self.assertEqual((restored/'state/registry.bin').read_bytes(),b'configuration')
        with self.assertRaises(FileExistsError):m.restore(out,restored)
        (out/'credentials/station-key').write_bytes(b'changed')
        with self.assertRaises(ValueError):m.restore(out,self.root/'invalid')
        self.assertFalse((self.root/'invalid').exists())
    def test_active_station_and_symlinks_are_rejected(self):
        code='import fcntl,sys,time,os;os.umask(0o077);f=open(sys.argv[1],"w");fcntl.lockf(f,fcntl.LOCK_EX);print("locked",flush=True);time.sleep(30)'
        p=subprocess.Popen([sys.executable,'-c',code,str(self.state/'daemon.lock')],stdout=subprocess.PIPE,text=True)
        try:
            self.assertEqual(p.stdout.readline().strip(),'locked')
            with self.assertRaises(ValueError):m.snapshot(self.state,self.keys,self.root/'busy')
        finally:p.terminate();p.wait();p.stdout.close()
        (self.keys/'link').symlink_to(self.root/'outside')
        with self.assertRaises(OSError):m.snapshot(self.state,self.keys,self.root/'unsafe')
    def test_credential_staging_keeps_live_registry_and_secrets_out_of_result(self):
        registry=self.root/'devices';raw=b'BASE_A '+b'12'*32+b'\n';m.write_new(registry,raw)
        out=self.root/'bundle';result=m.credential_bundle(registry,'BASE_A',out)
        self.assertEqual(registry.read_bytes(),raw);self.assertFalse(result['services_changed']);self.assertTrue(result['replacing'])
        new=(out/'station-key').read_text().strip();self.assertNotEqual(new,'12'*32);self.assertNotIn(new,json.dumps(result))
        self.assertEqual((out/'server-keys').read_text(),'BASE_A '+new+'\n')
if __name__=='__main__':unittest.main()

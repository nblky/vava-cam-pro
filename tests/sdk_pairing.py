#!/usr/bin/env python3
import socket
import time
import unittest
from sdk_integration import SDKTests,request,camera_sim
class PairingTests(SDKTests):
    def test_original_serial_pairing_credentials_and_validation(self):
        self.station_extra=['--pairing-serial','0123456789ABCDEFG12345678']
        self.restart()
        opened=request(self.station_socket,'pair-open 30')
        self.assertEqual(opened['pair_ssid'],'VAPair-12345678')
        self.assertEqual(opened['pair_password'],'va781256')
        request(self.station_socket,'pair-close')
        self.station_extra=['--pairing-serial','0123456789ABCDEFGAbCdEfGh']
        self.restart()
        opened=request(self.station_socket,'pair-open 30')
        self.assertEqual(opened['pair_ssid'],'VAPair-AbCdEfGh')
        self.assertEqual(opened['pair_password'],'vaGhAbEf')
        self.stop_proc(self.station)
        for serial in ('short','0123456789ABCDEFG1234567!'):
            self.station_extra=['--pairing-serial',serial]
            self.assertEqual(self.start_station(wait_ready=False).wait(timeout=5),2)
    def op(self):return self.local_status()['pairing']
    def restart(self):
        self.stop_proc(self.station);self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
    def test_pair_stages_and_temporary_ap_close_before_formal_ack(self):
        opened=request(self.station_socket,'pair-open 30');self.assertTrue(opened['ok']);self.assertNotEqual(opened['operation_id'],'0'*16)
        self.assertEqual(request(self.station_socket,'pair-open 30')['error'],'pair_operation_active')
        port=self.local_status()['ports'][0];c=socket.create_connection(('127.0.0.1',port),timeout=3);self.addCleanup(c.close)
        c.sendall(camera_sim.guest('SIM-PAIR-1'));cmd,p=camera_sim.decode(camera_sim.read_exact(c,128));self.assertEqual(cmd,401)
        self.assertEqual(self.op()['phase'],'credentials_sent')
        c.sendall(camera_sim.control(402));self.wait(lambda:self.op()['phase']=='waiting_formal_connection')
        self.assertEqual(self.local_status()['pair_window_ms'],0)
        slot=int.from_bytes(p[4:8],'little');payload=slot.to_bytes(4,'little')+camera_sim.text32('SIM-PAIR-1')
        c.sendall(camera_sim.control(1,payload));self.assertEqual(camera_sim.decode(camera_sim.read_exact(c,128))[0],2)
        self.assertEqual(self.op()['phase'],'waiting_confirmation')
        c.sendall(camera_sim.control(3));self.wait(lambda:self.op()['phase']=='completed')
        self.assertEqual(self.op()['id'],opened['operation_id']);c.close();self.restart()
        self.assertEqual(self.op()['phase'],'completed');self.assertTrue(self.local_status()['devices'][0]['paired'])
    def test_restart_never_reopens_ap_and_recovers_durable_formal_intent(self):
        opened=request(self.station_socket,'pair-open 30');port=self.local_status()['ports'][0]
        _,c=camera_sim.pair('127.0.0.1',port,'SIM-RECOVER',ack=False);c.close();self.restart()
        self.assertEqual(self.op()['phase'],'interrupted');self.assertEqual(self.op()['id'],opened['operation_id']);self.assertEqual(self.local_status()['pair_window_ms'],0)
        self.assertFalse(self.local_status()['devices'][0]['paired'])
        c=socket.create_connection(('127.0.0.1',self.local_status()['ports'][0]),timeout=3);self.addCleanup(c.close);c.sendall(camera_sim.first('SIM-RECOVER'))
        self.assertEqual(camera_sim.decode(camera_sim.read_exact(c,128))[0],6);self.wait(lambda:self.op()['phase']=='completed')
    def test_expiry_cancel_and_corrupt_state_are_explicit(self):
        request(self.station_socket,'pair-open 1');self.wait(lambda:self.op()['phase']=='expired',3)
        self.assertEqual(self.local_status()['pair_window_ms'],0)
        request(self.station_socket,'pair-open 30');self.assertTrue(request(self.station_socket,'pair-close')['ok']);self.assertEqual(self.op()['phase'],'cancelled')
        self.stop_proc(self.station);path=self.root/'station/pairing.bin';data=bytearray(path.read_bytes());data[40]^=1;path.write_bytes(data)
        proc=self.start_station(wait_ready=False);self.assertNotEqual(proc.wait(timeout=5),0);self.assertEqual(path.read_bytes(),bytes(data))
if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(PairingTests(n) for n in sorted(PairingTests.__dict__) if n.startswith('test_')))
    raise SystemExit(not result.wasSuccessful())

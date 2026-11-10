#!/usr/bin/env python3
"""Camera control regression tests with loopback DTLS and synthetic camera frames."""
import struct
import unittest
import sdk_integration as sdk


class ControlConfigTests(sdk.SDKTests):
    def setUp(self):
        super().setUp()
        self.connect_camera()
        self.camera.auto_media = False

    def local(self, command, error=None):
        result = sdk.request(self.station_socket, command)
        if error:
            self.assertFalse(result['ok'], result)
            self.assertEqual(result['error'], error)
        else:
            self.assertTrue(result['ok'], result)
        return result

    def echo(self, command, start, values):
        self.wait(lambda: any(c == command for c, _ in self.camera.received_control[start:]))
        payload = next(p for c, p in self.camera.received_control[start:] if c == command)
        self.assertEqual(list(struct.unpack_from('<' + 'I' * len(values), payload)), values)
        self.camera.ctrl.sendall(sdk.camera_sim.control(command, payload))
        key = {17: 'pir-level', 43: 'pir-mode', 33: 'detection-box', 10: 'quality'}[command]
        self.wait(lambda: self.local('settings 0')['settings']['operations'][key]['phase'] == 'echo_observed')

    def arm(self, mode, level):
        start = len(self.camera.received_control)
        self.local('arm 0 ' + mode)
        self.echo(17, start, [level])

    def restart(self):
        self.camera.close()
        self.camera = None
        self.stop_proc(self.station)
        self.station = self.start_station()

    def mode_roundtrip(self, mode, values):
        self.local('policy-config 0 mode=on level=2')
        self.echo(17, 0, [2])
        start = len(self.camera.received_control)
        self.local('configure-pir 0 ' + mode)
        self.echo(43, start, values)
        self.echo(17, start, [values[1]])
        p = self.local('policy 0')['policy']
        self.assertEqual(p['level_source'], 'work_mode')
        self.assertEqual(p['level'], values[1])
        self.arm('off', 0)
        self.arm('on', values[1])

    def test_monitor_uses_high_after_rearm(self):
        self.mode_roundtrip('monitor', [1, 1, 0, 20, 1])

    def test_battery_uses_middle_after_rearm(self):
        self.mode_roundtrip('battery', [0, 2, 20, 10, 1])

    def test_custom_maximum_uses_low_after_rearm(self):
        self.mode_roundtrip('custom 3 120 60', [2, 3, 120, 60, 1])

    def test_custom_minimum_restart_and_clear(self):
        self.mode_roundtrip('custom 3 5 5', [2, 3, 5, 5, 1])
        self.restart()
        p = self.local('policy 0')['policy']
        self.assertEqual((p['level'], p['fallback_level'], p['level_source']), (3, 2, 'work_mode'))
        self.camera = sdk.camera_sim.Camera('127.0.0.1', self.local_status()['ports'],
                                            'SIM-EVENT-0001', b'\0\0\0\1\x65test', False)
        self.camera.auto_media = False
        self.echo(17, 0, [3])
        self.assertFalse(any(c == 43 for c, _ in self.camera.received_control))
        start = len(self.camera.received_control)
        self.local('clear-settings 0')
        self.echo(17, start, [2])
        self.assertEqual(self.local('policy 0')['policy']['level_source'], 'legacy_policy')

    def test_invalid_modes_and_legacy_writes_are_atomic(self):
        self.local('policy-config 0 mode=on level=2')
        self.echo(17, 0, [2])
        start = len(self.camera.received_control)
        saved = self.local('configure-pir 0 monitor')['desired']
        self.echo(43, start, [1, 1, 0, 20, 1])
        self.echo(17, start, [1])
        before = self.local('policy 0')['policy']
        for command in ['policy-config 0 mode=off level=3 record=60', 'arm 0 on 2', 'pir-mode 0 battery']:
            self.local(command, 'pir_level_owned_by_work_mode')
            after = self.local('policy 0')['policy']
            for key in ('revision', 'mode', 'level', 'record_seconds'):
                self.assertEqual(after[key], before[key])
        for args in ['0 5 5', '4 5 5', '1 0 5', '1 6 5', '1 125 5', '1 5 0', '1 5 6', '1 5 65']:
            result = sdk.request(self.station_socket, 'configure-pir 0 custom ' + args)
            self.assertFalse(result['ok'], (args, result))
            current = self.local('settings 0')['desired']
            self.assertEqual(current['values'], saved['values'])
            self.assertEqual(current['revision'], saved['revision'])
        self.local('if-revision 0 configure-pir 0 battery', 'configuration_conflict')
        self.local('if-serial OTHER configure-pir 0 battery', 'camera_identity_changed')

    def test_offline_mode_survives_restart_without_flash_replay(self):
        self.local('policy-config 0 mode=on level=2')
        self.echo(17, 0, [2])
        self.camera.close()
        self.camera = None
        self.wait(lambda: not self.local_status()['devices'][0]['control_ready'])
        self.local('configure-pir 0 custom 1 5 10')
        self.assertEqual(self.local('policy 0')['policy']['level'], 1)
        self.stop_proc(self.station)
        self.station = self.start_station()
        desired = self.local('settings 0')['desired']
        self.assertEqual(desired['values']['pir-mode']['sensitivity'], 1)
        self.assertEqual(desired['items']['pir-mode']['phase'], 'explicit_apply_required')
        self.camera = sdk.camera_sim.Camera('127.0.0.1', self.local_status()['ports'],
                                            'SIM-EVENT-0001', b'\0\0\0\1\x65test', False)
        self.camera.auto_media = False
        self.echo(17, 0, [1])
        self.assertFalse(any(c == 43 for c, _ in self.camera.received_control))
        start = len(self.camera.received_control)
        self.local('apply-settings 0 pir-mode')
        self.echo(43, start, [2, 1, 5, 10, 1])
        self.echo(17, start, [1])

    def test_plan_toggle_remote_guards_persistence_and_edit(self):
        serial = 'if-serial SIM-EVENT-0001 '
        result = self.command(serial + 'plan 0 set 0 16 23:30:00 01:00:00')
        self.assertTrue(result['ok'], result)
        p = result['policy']
        self.assertTrue(p['plans'][0]['enabled'])
        disabled = self.command(serial + f'if-revision {p["revision"]} plan 0 enable 0 0')
        self.assertTrue(disabled['ok'], disabled)
        q = disabled['policy']['plans'][0]
        self.assertEqual((q['days_mask'], q['start_seconds'], q['end_seconds'], q['enabled']), (66, 84600, 3600, False))
        self.local(f'if-revision {p["revision"]} plan 0 enable 0 1', 'configuration_conflict')
        self.local('if-serial OTHER plan 0 enable 0 1', 'camera_identity_changed')
        self.local('plan 0 enable 9 1', 'plan_not_found')
        unchanged = self.local('plan 0 enable 0 0')
        self.assertFalse(unchanged['policy_saved'])
        self.assertEqual(unchanged['policy']['revision'], disabled['policy']['revision'])
        edited = self.local('plan 0 all-day 0 0123456')['policy']['plans'][0]
        self.assertFalse(edited['enabled'])
        self.local('clock-valid 1')
        self.local('arm 0 schedule')
        self.echo(17, 0, [0])
        self.restart()
        p = self.local('policy 0')['policy']
        self.assertFalse(p['plans'][0]['enabled'])
        self.assertEqual(p['plans'][0]['days_mask'], 127)
        self.assertFalse(p['desired_armed'])
        enabled = self.local('plan 0 enable 0 1')['policy']
        self.local('clock-valid 1')
        self.assertTrue(self.local('policy 0')['policy']['desired_armed'])
        self.assertTrue(enabled['plans'][0]['enabled'])
        self.assertEqual(self.local('plan 0 delete 0')['policy']['plans'], [])

    def test_full_plan_list_fits_status_and_capabilities(self):
        for index in range(10):
            self.local(f'plan 0 set {index} 0123456 23:59:59 23:59:58')
            if index % 2:
                self.local(f'plan 0 enable {index} 0')
        self.assertEqual(len(self.local('policy 0')['policy']['plans']), 10)
        self.assertEqual(len(self.local_status()['devices'][0]['policy']['plans']), 10)
        caps = self.local('capabilities')
        self.assertTrue(caps['plan_enable'])
        self.assertTrue(caps['work_mode_owns_sensitivity'])

    def detection_identity(self, firmware='1.4.0.54'):
        first = bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1])
        first[32:48] = firmware.encode().ljust(16, b'\0')
        first[48:64] = b'VA-HS03'.ljust(16, b'\0')
        self.camera.ctrl.sendall(sdk.camera_sim.control(5, first))
        self.wait(lambda: self.local_status()['devices'][0]['identity_report']['firmware'] == firmware)

    def test_detection_identity_report_gate_and_echo(self):
        self.detection_identity('other')
        self.local('configure 0 detection-box 1')
        self.assertEqual(self.local('settings 0')['desired']['items']['detection-box']['phase'], 'detection_box_firmware_not_verified')
        self.detection_identity()
        self.wait(lambda: self.local('settings 0')['desired']['items']['detection-box']['phase'] == 'valid_current_algorithm_report_203_required')
        algorithm = bytes([0, 0, 75, 0, 0, 0, 0, 100, 0, 100])
        start = len(self.camera.received_control)
        self.camera.ctrl.sendall(sdk.camera_sim.control(203, algorithm))
        self.echo(33, start, [1, 75, 1])
        saved = self.local('settings 0')['desired']
        self.assertEqual(saved['values']['detection-box'], 1)
        self.restart()
        self.assertEqual(self.local('settings 0')['desired']['values']['detection-box'], 1)
        self.assertEqual(self.local('settings 0')['desired']['pending_mask'], 0)
        self.camera = sdk.camera_sim.Camera('127.0.0.1', self.local_status()['ports'],
                                            'SIM-EVENT-0001', b'\0\0\0\1\x65test', False)
        self.camera.auto_media = False
        self.detection_identity()
        self.camera.ctrl.sendall(sdk.camera_sim.control(203, algorithm))
        for _ in range(15): self.camera.step(.02)
        self.assertFalse(any(c == 33 for c, _ in self.camera.received_control))

    def test_quality_keeps_arming_waits_media_and_rejects_auto_wire(self):
        self.arm('on', 2)
        self.local('live 0')
        self.local('configure 0 quality 1')
        self.assertEqual(self.local('settings 0')['desired']['items']['quality']['phase'], 'quality_requires_media_idle')
        for _ in range(10): self.camera.step(.02)
        self.assertFalse(any(c == 10 for c, _ in self.camera.received_control))
        start = len(self.camera.received_control)
        self.local('stop 0')
        self.echo(10, start, [1, 1, 15, 700])
        self.assertTrue(self.local('policy 0')['policy']['desired_armed'])
        self.local('configure 0 quality 3', 'invalid_persistent_setting_use_arm_for_pir_level')

    def test_detection_media_gate_allows_unrelated_configuration(self):
        self.detection_identity()
        self.camera.ctrl.sendall(sdk.camera_sim.control(203, bytes([0, 0, 75, 0, 0, 0, 0, 100, 0, 100])))
        self.local('live 0')
        start = len(self.camera.received_control)
        self.local('configure-many 0 detection-box=1 mirror=1')
        self.wait(lambda: any(c == 11 for c, _ in self.camera.received_control[start:]))
        payload = next(p for c, p in self.camera.received_control[start:] if c == 11)
        self.camera.ctrl.sendall(sdk.camera_sim.control(11, payload))
        self.wait(lambda: self.local('settings 0')['desired']['items']['detection-box']['phase'] == 'detection_box_requires_media_idle')
        self.assertFalse(any(c == 33 for c, _ in self.camera.received_control[start:]))
        self.local('stop 0')
        self.echo(33, start, [1, 75, 1])


if __name__ == '__main__':
    suite = unittest.TestSuite(ControlConfigTests(n) for n in sorted(ControlConfigTests.__dict__) if n.startswith('test_'))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())

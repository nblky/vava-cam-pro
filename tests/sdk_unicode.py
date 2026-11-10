#!/usr/bin/env python3
"""Non-ASCII camera labels round-trip through authenticated durable task results."""
import json
import unittest
from sdk_web import WebTests

class UnicodeTests(WebTests):
    def test_camera_name_and_status_survive_task_poll_and_restart(self):
        self.connect_camera();token=self.login();name='门前 📷'
        def complete(command):
            code,task,_=self.submit_web(token,command);self.assertEqual(code,200,task);self.assertTrue(task['ok'])
            endpoint=f'/api/v1/devices/BASE_A/tasks/{task["epoch"]}/{task["id"]}'
            def done():
                status,result,_=self.web(endpoint,token=token)
                self.assertEqual(status,200,result)
                return result if result.get('state')=='completed' else None
            result=self.wait(done);self.assertTrue(json.loads(result['result'])['ok']);return result,endpoint
        saved,url=complete({'command':'name','args':['0',name.encode().hex()],'camera_serial':'SIM-EVENT-0001'})
        self.assertEqual(json.loads(saved['result'])['profile']['name'],name)
        status,_=complete({'command':'status','args':[]})
        self.assertEqual(json.loads(status['result'])['devices'][0]['profile']['name'],name)
        self.server.terminate();self.server.wait(8);self.server=self.start_server();self.online()
        code,restored,_=self.web(url,token=token);self.assertEqual(code,200,restored)
        self.assertEqual(restored['result'],saved['result'])

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(UnicodeTests(n) for n in UnicodeTests.__dict__ if n.startswith('test_')))
    raise SystemExit(not result.wasSuccessful())

#!/usr/bin/env python3
"""Actual clock regression across former 120 second and mux 180 second limits."""
import threading,time,unittest
from sdk_live import LiveTests
class DurationTests(LiveTests):
    def test_live_continues_past_three_minutes_then_releases_on_close(self):
        self.prepare();c=self.live();self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        got={'bytes':0,'ended':False,'error':None}
        def consume():
            try:
                r=c.getresponse();self.assertEqual(r.status,200)
                while data:=r.read1(32768):got['bytes']+=len(data)
            except Exception as e:got['error']=str(e)
            finally:got['ended']=True
        thread=threading.Thread(target=consume,daemon=True);thread.start()
        start=time.monotonic();cycle=0
        while time.monotonic()-start<200:
            for ms,kind,payload in self.frames:
                self.send_frames([(cycle*1280+ms,kind,payload)])
                time.sleep(.045)
            cycle+=1
            self.assertFalse(got['ended'],got)
        self.assertGreater(got['bytes'],10000)
        self.assertGreater(cycle*1280,180000)  # Also cross the mux's old media-clock limit.
        d=self.local_status()['devices'][0];self.assertEqual(d['viewers'],1);self.assertTrue(d['live_requested'])
        # Explicit session revocation closes the same authenticated response.
        self.web('/api/v1/logout',{},self.token);self.wait(self.released,6);thread.join(5);c.close()
        self.assertTrue(got['ended'])
if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([DurationTests('test_live_continues_past_three_minutes_then_releases_on_close')]))
    raise SystemExit(not result.wasSuccessful())

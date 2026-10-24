import unittest
from rf3_gui.app.receive_rate import ReceiveRateTracker


class TestReceiveRate(unittest.TestCase):
    def test_real_byte_deltas_and_window(self):
        rate=ReceiveRateTracker()
        self.assertIsNone(rate.observe('one','Receiving',100,0).bytes_per_second)
        self.assertEqual(rate.observe('one','Receiving',600,5).bytes_per_second,100)
        self.assertEqual(rate.observe('one','Receiving',1600,10).bytes_per_second,150)
        result=rate.observe('one','Receiving',2600,20)
        self.assertEqual(result.bytes_per_second,100)
        self.assertEqual(result.elapsed_seconds,20)

    def test_duplicate_frames_do_not_make_zero_rate(self):
        rate=ReceiveRateTracker()
        rate.observe('one','Receiving',0,0)
        rate.observe('one','Receiving',1024,4)
        self.assertEqual(rate.observe('one','Receiving',1024,4).bytes_per_second,256)
        self.assertAlmostEqual(rate.observe('one','Receiving',1024,4.01).bytes_per_second,1024/4.01)

    def test_stall_reports_zero_after_old_samples_leave(self):
        rate=ReceiveRateTracker()
        for second,count in ((0,0),(5,1024),(10,1024),(15,1024),(20,1024)):
            result=rate.observe('one','Receiving',count,second)
        self.assertEqual(result.bytes_per_second,0)

    def test_terminal_freezes_rate_time_and_new_transfer_resets(self):
        rate=ReceiveRateTracker()
        rate.observe('one','Receiving',0,0)
        result=rate.observe('one','Completed',1024,4)
        self.assertEqual(result.bytes_per_second,256)
        self.assertEqual(rate.observe('one','Completed',1024,100),result)
        result=rate.observe('two','Receiving',0,101)
        self.assertIsNone(result.bytes_per_second)
        self.assertEqual(result.elapsed_seconds,0)

    def test_terminal_first_has_no_invented_rate(self):
        rate=ReceiveRateTracker()
        self.assertIsNone(rate.observe('one','Completed',1024,10))
        self.assertIsNone(rate.observe('one','Completed',1024,20))

    def test_counter_reset_and_disconnect_do_not_reuse_old_rate(self):
        rate=ReceiveRateTracker()
        rate.observe('one','Receiving',500,0)
        self.assertIsNone(rate.observe('one','Receiving',0,1).bytes_per_second)
        rate.reset()
        self.assertIsNone(rate.observe('one','Completed',500,5))

    def test_cancelled_and_empty_transfers(self):
        for state,count in (('Cancelled',20),('Failed',20),('Completed',0)):
            rate=ReceiveRateTracker()
            rate.observe('one','Receiving',0,0)
            self.assertEqual(rate.observe('one',state,count,2).bytes_per_second,count/2)


if __name__=='__main__':unittest.main()

import csv
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from rf3_gui.app.delivery import Reading, DeliveryBook
from rf3_gui.app.report_csv import ReportCapture

def event(direction="TX", mode="noack", counts="1000 1000 -1 -1 0 0 1 SentUnconfirmed",
          identity="0000A104", instance="00000021"):
    board="Custom" if direction=="TX" else "Devboard"
    return Reading.parse(f"DELIVERY1 {board} {direction} {mode} {identity} {instance} {counts}")

class DeliveryTest(unittest.TestCase):
    def test_no_receiver_unknown(self):
        book=DeliveryBook();book.observe(event())
        row,=book.rows()
        self.assertEqual(row["distinct_data_sent"],1000)
        self.assertIsNone(row["loss_pct"]);self.assertIsNone(row["unique_data_received"])

    def test_peer_csv_roundtrip_both_directions_and_repeat_import(self):
        with TemporaryDirectory() as d:
            path=Path(d)/'peer.csv'
            a,b=ReportCapture(),ReportCapture()
            a.delivery.observe(event())
            b.delivery.observe(event("RX",counts="1000 1000 997 982 -1 1 1 Completed"))
            b.save("RX",path)
            a.delivery.import_csv(path);a.delivery.import_csv(path)
            for direction in ("TX",None):
                a.save(direction,path)
                with path.open(newline="") as f: rows=list(csv.DictReader(f))
                self.assertEqual([row["record_type"] for row in rows], ["summary", "endpoint"])
                r=rows[0]
                restored=DeliveryBook()
                restored.import_csv(path)
                self.assertEqual(restored.rows()[0]["loss_pct"], 0.3)
                self.assertEqual((r["missing_packets"],r["loss_pct"],r["first_pass_missing"],
                                  r["recovered_later"],r["crc_result"]),("3","0.3","18","15","passed"))
                self.assertEqual(r["local_received"],"")
            offline=DeliveryBook()
            a.save("TX",path);offline.import_csv(path)
            b.save("RX",path);offline.import_csv(path)
            self.assertEqual(offline.rows()[0]["loss_pct"],0.3)
            a.save("TX",path)
            b.delivery.import_csv(path)
            self.assertEqual(b.delivery.rows()[0]["delivery_pct"],99.7)

    def test_interrupted_denominator(self):
        b=DeliveryBook()
        tx=event(counts="1000 600 -1 -1 4 0 1 Cancelled")
        rx=event("RX",counts="1000 -1 580 -1 -1 0 1 Cancelled")
        b.observe(tx);b.peer[rx.key]=rx
        r,=b.rows()
        self.assertEqual((r["delivery_pct"],r["loss_pct"],r["transfer_sent_pct"]),(96.6667,3.3333,60))

    def test_zero_and_disconnected(self):
        b=DeliveryBook();a=event(counts="10 0 -1 -1 0 0 0 Observing");b.observe(a);b.disconnect()
        r,=b.rows();self.assertEqual(r["result"],"Disconnected");self.assertIsNone(r["loss_pct"])

    def test_text_full_partial_and_collision(self):
        b=DeliveryBook();t=event(mode="text",counts="3 3 -1 -1 1 0 1 SentUnconfirmed");b.observe(t)
        rx=event("RX","text","3 -1 2 -1 -1 0 1 ObservationEnded");b.peer[rx.key]=rx
        self.assertEqual(b.rows()[0]["missing_packets"],1)
        b.observe(replace(t,instance="12345678"));self.assertTrue(all(r["loss_pct"] is None for r in b.rows()))
        b=DeliveryBook();b.observe(event("RX","text","3 3 3 -1 -1 0 1 Reassembled"))
        self.assertEqual(b.rows()[0]["delivery_pct"],100)

    def test_ack_resume_ids_and_retries(self):
        b=DeliveryBook()
        a=event(mode="ack",counts="100 56 -1 -1 8 0 1 Failed")
        a.session="1"*32;a.resumes=0;b.observe(a)
        c=replace(a,id="12345678",instance="76543210",sent=44,received=44,verified=True,
                  result="Completed",resumes=1,retries=3);b.observe(c)
        rows=b.rows();self.assertEqual(rows[1]["session_retries"],11)
        self.assertEqual(rows[1]["distinct_data_sent"],44)

    def test_open_report_missing_evidence_legacy_and_bad_input(self):
        b=DeliveryBook()
        b.observe(event("RX",counts="1000 1000 950 -1 -1 1 0 Completed"))
        self.assertIsNone(b.rows()[0]["loss_pct"])
        for counts in ["1 1 2 -1 0 0 1 Completed","1 -2 1 -1 0 0 1 Completed"]:
            with self.assertRaises(ValueError):event(counts=counts)
        with TemporaryDirectory() as d:
            p=Path(d)/'old.csv';p.write_text('direction,transfer_id\nTX,1234\n')
            with self.assertRaises(ValueError):b.import_csv(p)

    def test_stale_peer_and_duplicate_local_update(self):
        b=DeliveryBook();a=event();b.observe(a);b.observe(a)
        r=event("RX",counts="1000 1000 992 990 -1 1 1 Completed")
        r.time="2000-01-01T00:00:00+00:00";b.peer[r.key]=r
        self.assertIsNone(b.rows()[0]["loss_pct"]);self.assertEqual(len(b.rows()),1)


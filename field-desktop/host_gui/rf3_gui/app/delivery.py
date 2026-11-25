"""Endpoint observations for individual on-air file transfers and text messages."""
import csv
import re
from dataclasses import dataclass, replace
from datetime import datetime, timezone

FIELDS = ("record_type", "board", "link_direction", "mode", "observation_id", "planned_packets",
          "distinct_data_sent", "unique_data_received", "delivery_pct", "missing_packets",
          "loss_pct", "first_pass_missing", "first_pass_loss_pct", "recovered_later",
          "resume_count", "session_retries", "session_id", "file_verified", "result", "evidence",
          "receiver_result", "crc_result", "transfer_sent_pct", "local_sent", "local_received", "local_retries", "first_pass_received", "observation_closed")
COLUMNS = ("timestamp_utc", "direction", "transfer_id", "tx_retries") + FIELDS
BOARDS = {"Custom": "Devboard", "Devboard": "Custom"}
STATES = {"Observing", "Completed", "Incomplete", "ObservationEnded", "SentUnconfirmed",
          "SentRearmFailed", "Reassembled", "TimedOut", "Failed", "Cancelled", "Disconnected"}
HEX = re.compile(r"[0-9A-F]{8}")

def timestamp():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")

def count(value):
    value = int(value)
    if not -1 <= value <= 2**31-1:
        raise ValueError("Invalid counter")
    return None if value == -1 else value

@dataclass
class Reading:
    board: str
    direction: str
    mode: str
    id: str
    instance: str
    planned: int | None
    sent: int | None
    received: int | None
    first: int | None
    retries: int | None
    verified: bool
    closed: bool
    result: str
    time: str
    resumes: int | None = None
    session: str = ""

    @property
    def key(self):
        return self.board, self.direction, self.mode, self.id, self.instance

    @property
    def evidence_rank(self):
        counters = (self.received, self.sent, self.first, self.planned, self.retries)
        known_counts = tuple(value if value is not None else -1 for value in counters)
        return (self.closed, self.verified, *known_counts, bool(self.session))

    @classmethod
    def parse(cls, line):
        parts = line.split()
        if len(parts) != 14 or parts[0] != "DELIVERY1":
            raise ValueError("Invalid delivery record")
        _, board, direction, mode, identity, instance, *values = parts
        if (board not in BOARDS or direction not in {"TX", "RX"} or mode not in {"ack", "noack", "text"}
                or not HEX.fullmatch(identity) or int(identity,16)==0 or not HEX.fullmatch(instance)
                or values[-1] not in STATES or any(f not in {"0","1"} for f in values[5:7])):
            raise ValueError("Invalid delivery record fields")
        planned, sent, received, first, retries = map(count, values[:5])
        present = [v for v in (planned,sent,received,first) if v is not None]
        if (any(v > (7 if mode=="text" else 65536) for v in present)
                or planned is not None and any(v > planned for v in present)
                or received is not None and first is not None and first > received
                or sent is not None and received is not None and received > sent):
            raise ValueError("Inconsistent delivery counters")
        return cls(board,direction,mode,identity,instance,planned,sent,received,first,retries,
                   values[5]=="1",values[6]=="1",values[7],timestamp())

    @classmethod
    def from_csv(cls, row):
        if any(row.get(name) is None for name in COLUMNS):
            raise ValueError("Incomplete delivery CSV row")
        def val(name):
            return row[name] if row[name] != "" else "-1"
        obj=cls.parse(" ".join(("DELIVERY1",row["board"],row["direction"],row["mode"],
                    row["transfer_id"],row["observation_id"],val("planned_packets"),
                    val("local_sent"),val("local_received"),val("first_pass_received"),
                    val("local_retries"),row["file_verified"],row["observation_closed"],row["result"])))
        stamp=datetime.fromisoformat(row["timestamp_utc"])
        if stamp.tzinfo is None: raise ValueError("Missing UTC offset")
        try:
            obj.time = stamp.astimezone(timezone.utc).isoformat(timespec="milliseconds")
        except OverflowError as exc:
            raise ValueError("Delivery timestamp is outside the UTC date range") from exc
        obj.resumes=count(val("resume_count"))
        session=row["session_id"]
        if session and not re.fullmatch(r"[0-9a-f]{32}",session):
            raise ValueError("Invalid session identifier")
        obj.session=session
        return obj

class DeliveryBook:
    def __init__(self):
        self.local={}
        self.peer={}
        self.latest={}

    def observe(self, entry):
        previous=self.local.get(entry.key)
        if previous:
            entry.time=previous.time
            if previous.session:
                entry.session,entry.resumes=previous.session,previous.resumes
        self.local[entry.key]=entry
        self.latest[entry.direction]=entry.key

    def disconnect(self):
        for key,row in self.local.items():
            if not row.closed:
                self.local[key]=replace(row,result="Disconnected")
        self.latest.clear()

    def import_csv(self, path):
        additions = {}
        summary_keys = set()
        with open(path, encoding="utf-8-sig", newline="") as f:
            reader = csv.DictReader(f)
            if not set(COLUMNS).issubset(reader.fieldnames or ()):
                raise ValueError("Choose an RF3 delivery CSV exported by R18 or later.")
            for data in reader:
                record_type = data.get("record_type")
                if record_type not in {"summary", "endpoint"}:
                    continue
                row = Reading.from_csv(data)
                previous = additions.get(row.key)
                if previous is None or row.evidence_rank > previous.evidence_rank:
                    additions[row.key] = row
                if record_type == "summary":
                    summary_keys.add(row.key)
        restore = not self.local
        if not additions or (restore and not summary_keys):
            raise ValueError("The CSV has no delivery summaries.")
        for row in sorted(additions.values(), key=lambda entry: entry.time):
            if restore and row.key in summary_keys:
                self.observe(row)
                continue
            book = self.local if row.key in self.local else self.peer
            previous = book.get(row.key)
            if previous is None or row.evidence_rank > previous.evidence_rank:
                book[row.key] = row
        return len(additions)

    def partner(self, row):
        def matches(other):
            return (other.board==BOARDS[row.board] and other.direction!=row.direction
                    and other.mode==row.mode and other.id==row.id
                    and (row.planned is None or other.planned is None or row.planned==other.planned)
                    and abs((datetime.fromisoformat(row.time)-datetime.fromisoformat(other.time)).total_seconds())<=300)
        candidates = {r.key: r for r in self.peer.values() if matches(r)}
        for other in self.local.values():
            if matches(other):
                imported = candidates.get(other.key)
                if imported is None or other.evidence_rank >= imported.evidence_rank:
                    candidates[other.key] = other
        if row.mode=="noack":
            exact=[r for r in candidates.values() if r.instance==row.instance]
            if len(exact)==1: return exact[0]
            # Odd RX tokens came from the sender's pass marker.
            candidates={k:r for k,r in candidates.items()
                        if not (int((r if r.direction=="RX" else row).instance,16)&1)}
        if len(candidates)!=1: return None
        peer=next(iter(candidates.values()))
        # A reused ID or multiple loop iterations must never claim the same peer evidence.
        same=[r for r in self.local.values() if r.board==row.board and r.direction==row.direction
              and r.mode==row.mode and r.id==row.id and
              abs((datetime.fromisoformat(peer.time)-datetime.fromisoformat(r.time)).total_seconds())<=300]
        return peer if len(same)==1 else None

    def summary(self, row):
        peer=self.partner(row)
        tx=row if row.direction=="TX" else peer
        rx=row if row.direction=="RX" else peer
        sent=tx.sent if tx and tx.closed else None
        if sent is None and rx: sent=rx.sent
        received = rx.received if rx else row.received
        first = rx.first if rx else None
        plan = row.planned
        complete = bool(rx and rx.closed or not rx and row.closed and row.received is not None)
        if tx and not tx.closed and rx and rx.sent is None:
            complete = False
        # A closed ACK sender can prove delivery even if an imported receiver
        # report was saved while still observing or has no usable packet count.
        acknowledged = (row.mode == "ack" and tx and tx.closed and tx.received is not None
                        and (not complete or received is None))
        if acknowledged:
            received = tx.received
            complete = True
        missing=sent-received if (complete and sent is not None and received is not None
                                   and received<=sent) else None
        def pct(n,d):
            return round(100*n/d,4) if n is not None and d else None
        first_missing=plan-first if plan is not None and first is not None else None
        later=received-first if first is not None and received is not None else None
        session=tx.session if tx else ""
        retries=tx.retries if tx else None
        total_retries=None
        if session:
            items={r.key:r for r in (*self.peer.values(),*self.local.values())
                   if r.direction=="TX" and r.session==session and r.resumes is not None
                   and tx.resumes is not None and r.resumes<=tx.resumes}
            if len(items)==tx.resumes+1 and all(r.retries is not None for r in items.values()):
                total_retries=sum(r.retries for r in items.values())
        sender=row.board if row.direction=="TX" else BOARDS[row.board]
        crc = None
        if row.mode != "text":
            crc = "passed" if row.verified or (peer and peer.verified) else "unverified"
        evidence = "delivery unknown"
        if acknowledged:
            evidence = "acknowledged"
        elif peer:
            evidence = "paired endpoints"
        elif rx:
            evidence = "receiver"
        elif row.received is not None:
            evidence = "acknowledged"
        return {
            "timestamp_utc": row.time,
            "direction": row.direction,
            "transfer_id": row.id,
            "tx_retries": retries,
            "record_type": "summary",
            "board": row.board,
            "link_direction": f"{sender} to {BOARDS[sender]}",
            "mode": row.mode,
            "observation_id": row.instance,
            "planned_packets": plan,
            "distinct_data_sent": sent,
            "unique_data_received": received,
            "delivery_pct": pct(received,sent) if missing is not None else None,
            "missing_packets": missing,
            "loss_pct": pct(missing,sent),
            "first_pass_missing": first_missing,
            "first_pass_loss_pct": pct(first_missing,plan),
            "recovered_later": later,
            "resume_count": tx.resumes if tx else None,
            "session_retries": total_retries,
            "session_id": session,
            "file_verified": int(row.verified),
            "result": row.result,
            "evidence": evidence,
            "receiver_result": rx.result if rx else None,
            "crc_result": crc,
            "transfer_sent_pct": pct(sent,plan),
            "local_sent": row.sent,
            "local_received": row.received,
            "local_retries": row.retries,
            "first_pass_received": row.first,
            "observation_closed": int(row.closed),
        }

    def rows(self,direction=None):
        rows=[]
        for row in self.local.values():
            if direction and row.direction!=direction: continue
            if direction is None and row.direction=="RX":
                peer=self.partner(row)
                if peer and peer.key in self.local: continue
            rows.append(self.summary(row))
        return rows

    def export_rows(self, direction=None):
        rows = self.rows(direction)
        keys = {(r["board"], r["direction"], r["mode"], r["transfer_id"], r["observation_id"])
                for r in rows}
        evidence = {}
        for key in keys:
            peer = self.partner(self.local[key])
            if peer is not None and peer.key not in keys:
                evidence[peer.key] = peer
        for key in sorted(evidence):
            row = self.summary(evidence[key])
            row["record_type"] = "endpoint"
            rows.append(row)
        return rows

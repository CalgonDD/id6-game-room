#!/usr/bin/env python3
"""Summarize HMI trace text or an installer report, offline and read-only.

An unmatched entry is a candidate outstanding call, not proof of deadlock:
rotation, UDP loss, a live pull or process death may remove its return record.
"""
import argparse
from collections import defaultdict
from datetime import datetime, timezone
import json
from pathlib import Path
import re

RECORD = re.compile(
    r"wall=(\d+\.\d+) mono=(\d+) pid=(\d+) tid=(\d+) seq=(\d+) lost=(\d+) (\S+) (.*)")
CALL = re.compile(r"(\w+)_(begin|end) instance=(\S+) id=(\d+) call=(\d+)(?: elapsed_ms=(\d+))?")


def analyze(text):
    seen = set()
    pending = {}
    producers = {}
    gaps = []
    missing_entries = []
    completed = defaultdict(list)
    events = []
    count = 0
    for line in text.splitlines():
        match = RECORD.search(line)
        if not match:
            continue
        wall, mono, pid, tid, seq, lost, app, event = match.groups()
        identity = match.group(0)
        if identity in seen:
            continue
        seen.add(identity)
        count += 1
        record = dict(wall_utc=datetime.fromtimestamp(float(wall), timezone.utc).isoformat(),
                      mono_ms=int(mono), pid=int(pid), tid=int(tid), seq=int(seq),
                      reported_send_failures=int(lost), app=app, event=event)
        producer = (pid, app)
        last = producers.get(producer)
        if last is not None and int(seq) != last + 1:
            gaps.append(dict(pid=int(pid), app=app, after_seq=last, next_seq=int(seq),
                             note="sequence gap/reset or input out of order"))
        producers[producer] = int(seq)
        call = CALL.fullmatch(event)
        if call:
            kind, phase, instance, identifier, call_id, duration = call.groups()
            key = (pid, tid, app, kind, instance, call_id)
            record.update(kind=kind, instance=instance, identifier=int(identifier), call=int(call_id))
            if phase == "begin":
                pending[key] = record
            else:
                entry = pending.pop(key, None)
                if entry is None:
                    missing_entries.append(record)
                if duration is not None:
                    record["elapsed_ms"] = int(duration)
                    completed[kind].append(record)
        else:
            events.append(record)
    longest = {}
    for kind, records in completed.items():
        longest[kind] = max(records, key=lambda r: r["elapsed_ms"])
    return dict(records=count, sequence_discontinuities=gaps,
                outstanding_candidates=list(pending.values()),
                returns_without_entry=missing_entries,
                longest_completed_calls=longest, recent_events=events[-20:],
                limitations=["Unmatched calls are candidates, not proof of a blocked call.",
                             "UDP acceptance does not guarantee delivery or persistence.",
                             "A zero send-failure count does not exclude receiver/file loss.",
                             "Correlate PID/build identity, watchdog and matching core evidence."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path, help="previous trace then current, or report.txt")
    args = parser.parse_args()
    result = analyze("\n".join(p.read_text(errors="replace") for p in args.files))
    print(json.dumps(result, indent=2))
    if not result["records"]:
        parser.exit(1, "No new-format trace records found; this does not exclude a freeze.\n")


if __name__ == "__main__":
    main()

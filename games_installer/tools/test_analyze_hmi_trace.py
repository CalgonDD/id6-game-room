import unittest
from analyze_hmi_trace import analyze


def row(seq, event, lost=0):
    return f"wall=1794852000.100 mono={1000+seq} pid=25 tid=22 seq={seq} lost={lost} GameRoom {event}"


class TraceAnalysisTest(unittest.TestCase):
    def test_model_outstanding_and_completed_handler(self):
        text = "\n".join([
            row(1, "selectColor_begin instance=0x100 id=2 call=1"),
            row(2, "color_button=2"),
            row(3, "selectColor_end instance=0x100 id=2 call=1 elapsed_ms=3"),
            row(4, "model_begin instance=0x100 id=110 call=4"),
        ])
        result = analyze(text)
        self.assertEqual(result["records"], 4)
        self.assertEqual(len(result["outstanding_candidates"]), 1)
        self.assertEqual(result["outstanding_candidates"][0]["identifier"], 110)
        self.assertEqual(result["longest_completed_calls"]["selectColor"]["elapsed_ms"], 3)
        self.assertFalse(result["sequence_discontinuities"])

    def test_capture_duplicates_and_missing_records(self):
        entry = row(1, "tick_begin instance=0x100 id=1 call=1")
        end = row(3, "tick_end instance=0x100 id=1 call=1 elapsed_ms=10", lost=1)
        result = analyze("\n".join([entry, entry, end,
            row(4, "model_end instance=0x100 id=60 call=2 elapsed_ms=2")]))
        self.assertEqual(result["records"], 3)
        self.assertEqual(len(result["sequence_discontinuities"]), 1)
        self.assertEqual(len(result["returns_without_entry"]), 1)
        self.assertFalse(result["outstanding_candidates"])

    def test_old_or_missing_trace_is_not_treated_as_a_clean_run(self):
        self.assertEqual(analyze("old-format trace or empty report")["records"], 0)


if __name__ == "__main__":
    unittest.main()

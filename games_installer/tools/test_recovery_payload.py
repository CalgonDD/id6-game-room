import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("recovery_stage", Path(__file__).with_name("stage_payload.py"))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class RecoveryPayloadTests(unittest.TestCase):
    def test_stale_ps1_is_rejected_without_deleting_anything(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "GameRoom").mkdir()
            good = root / "GameRoom" / "freedoom-qnx"
            stale = root / "GameRoom" / "ps1-qnx"
            good.write_bytes(b"good")
            stale.write_bytes(b"preserve evidence")
            with self.assertRaisesRegex(ValueError, "ps1-qnx"):
                stage.reject_unexpected_payload(root, {"GameRoom/freedoom-qnx"})
            self.assertEqual(stale.read_bytes(), b"preserve evidence")
            self.assertEqual(good.read_bytes(), b"good")

    def test_clean_payload_is_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "allowed").write_bytes(b"ok")
            stage.reject_unexpected_payload(root, {"allowed"})


if __name__ == "__main__":
    unittest.main()

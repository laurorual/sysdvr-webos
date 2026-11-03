"""Offline tests for the portable collector (Python standard library only)."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

script = Path(__file__).resolve().parents[1] / "get_sysdvr_log.py"
spec = importlib.util.spec_from_file_location("collector", script)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class Channel:
    def __init__(self, status): self.status = status
    def recv_exit_status(self): return self.status


class Stream(io.BytesIO):
    def __init__(self, payload, status=0):
        super().__init__(payload)
        self.channel = Channel(status)


class TV:
    def __init__(self, payload, status=0, error=b""):
        self.payload, self.status, self.error = payload, status, error

    def exec_command(self, command, timeout=0):
        assert collector.LOG_PATH in command
        return None, Stream(self.payload, self.status), Stream(self.error)


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        for name, path in [
            ("ROOT", self.directory),
            ("DATA", self.directory / "log_DATA"),
            ("PROFILES", self.directory / "log_DATA" / "tvs.json"),
        ]:
            patcher = patch.object(collector, name, path)
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_ipv4(self):
        self.assertEqual(collector.ipv4("192.168.0.5"), "192.168.0.5")
        for value in ("nope", "224.0.1.1", "2001:db8::1"):
            with self.assertRaises(collector.CollectorError):
                collector.ipv4(value)

    def test_saved_tv_and_default(self):
        self.assertEqual(collector.load_profiles(), ([], ""))
        collector.save_profile("192.168.0.5", "[LG]webOSTV")
        collector.save_profile("192.168.0.6", "Living room")
        profiles, last = collector.load_profiles()
        self.assertEqual(last, "192.168.0.6")
        self.assertEqual(len(profiles), 2)
        self.assertEqual(json.loads(collector.PROFILES.read_text())["version"], 1)
        with patch.object(collector, "ask", return_value="1"):
            self.assertEqual(collector.select_tv(), ("192.168.0.6", "Living room"))
        collector.save_profile("192.168.0.5", "Manual IP entry")
        self.assertEqual(collector.load_profiles()[0][0]["name"], "[LG]webOSTV")

    def test_manual_selection(self):
        with patch.object(collector, "ask", side_effect=["2", "192.168.0.7"]):
            self.assertEqual(collector.select_tv(), ("192.168.0.7", "Manual IP entry"))

    def test_invalid_profiles(self):
        collector.PROFILES.parent.mkdir()
        collector.PROFILES.write_text("not JSON")
        self.assertEqual(collector.load_profiles(), ([], ""))

    def test_download_and_missing_file(self):
        result = collector.retrieve_log(TV(b"example log\n"), "192.168.0.5")
        self.assertEqual(result.read_bytes(), b"example log\n")
        with self.assertRaises(collector.CollectorError):
            collector.retrieve_log(TV(b"", 44, b"LOG_NOT_FOUND"), "192.168.0.5")
        self.assertEqual(list(self.directory.glob("*.part")), [])


if __name__ == "__main__":
    unittest.main()

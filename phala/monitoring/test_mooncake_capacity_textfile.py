import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import mooncake_capacity_textfile as collector


VALUES = {
    "master_total_capacity_bytes": 1040 * 1024**3,
    "master_total_file_capacity_bytes": 27 * 1024**4,
    "master_allocated_bytes": 17,
    "master_allocated_file_size_bytes": 19,
    "master_key_count": 23,
}


def payload(values=VALUES):
    return (
        "# HELP ignored comment\n"
        + "\n".join(f"{k} {v}" for k, v in values.items())
        + "\n"
    ).encode()


class CollectorTest(unittest.TestCase):
    def test_success_is_one_pool_and_exactly_allowlisted_metrics(self):
        values = collector.parse_metrics(
            payload() + b'ignored_metric{content="not-exported"} 3\n'
        )
        self.assertEqual(values, VALUES)
        result = collector.render_metrics(values, 123.5)
        samples = [line for line in result.splitlines() if not line.startswith("#")]
        self.assertEqual(len(samples), 7)
        self.assertTrue(all('pool="deepseek-v41-usw5"' in line for line in samples))
        self.assertNotIn("not-exported", result)
        self.assertIn(
            'registered_bytes{pool="deepseek-v41-usw5",tier="dram"} 1116691496960',
            result,
        )
        self.assertIn(
            'registered_bytes{pool="deepseek-v41-usw5",tier="ssd"} 29686813949952',
            result,
        )

    def test_missing_each_required_metric_fails(self):
        for name in VALUES:
            with self.subTest(name=name), self.assertRaises(ValueError):
                collector.parse_metrics(
                    payload({k: v for k, v in VALUES.items() if k != name})
                )

    def test_invalid_values_fail(self):
        for invalid in (
            "NaN",
            "+Inf",
            "-Inf",
            "bad",
            "-1",
            "1.5",
            str(2**63),
            "1 42",
            "1 # extra",
        ):
            with self.subTest(value=invalid), self.assertRaises(ValueError):
                collector.parse_metrics(payload(dict(VALUES, master_key_count=invalid)))

    def test_duplicate_and_labeled_metrics_fail(self):
        for extra in (
            b"master_key_count 23\n",
            b"  master_key_count 23\n",
            b'master_key_count{origin="a"} 23\n',
        ):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                collector.parse_metrics(payload() + extra)

    def test_exponent_and_real_zero_are_valid(self):
        actual = collector.parse_metrics(
            payload(
                dict(VALUES, master_key_count="2.3e1", master_total_capacity_bytes="0")
            )
        )
        self.assertEqual(actual["master_key_count"], 23)
        self.assertEqual(actual["master_total_capacity_bytes"], 0)

    def test_size_and_encoding_are_bounded(self):
        for invalid in (b"x" * (collector.MAX_RESPONSE + 1), payload() + b"\xff"):
            with self.assertRaises(ValueError):
                collector.parse_metrics(invalid)

    def test_network_failure_replaces_old_capacity_with_failure_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "capacity.prom"
            collector.atomic_write(output, collector.render_metrics(VALUES, 100))
            with (
                patch.object(collector, "fetch_metrics", side_effect=TimeoutError),
                patch.object(collector.time, "time", return_value=200),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                code = collector.collect_once("master", output)
            result = output.read_text()
            self.assertEqual(code, 1)
            self.assertIn('collection_success{pool="deepseek-v41-usw5"} 0', result)
            self.assertIn("200.000000", result)
            self.assertNotIn("registered_bytes", result)
            self.assertNotIn("allocated_bytes", result)
            self.assertNotIn("_keys", result)

    def test_parse_failure_also_publishes_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "capacity.prom"
            with (
                patch.object(
                    collector, "fetch_metrics", return_value=b"master_key_count NaN\n"
                ),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(collector.collect_once("master", output), 1)
            self.assertNotIn("registered_bytes", output.read_text())

    def test_success_publishes_atomically(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "capacity.prom"
            with patch.object(collector, "fetch_metrics", return_value=payload()):
                self.assertEqual(collector.collect_once("master", output), 0)
            self.assertEqual(list(Path(tmp).iterdir()), [output])
            self.assertIn(
                'collection_success{pool="deepseek-v41-usw5"} 1', output.read_text()
            )

    def test_replace_failure_does_not_refresh_old_timestamp_or_leak_temp(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "capacity.prom"
            original = collector.render_metrics(VALUES, 100)
            output.write_text(original)
            with (
                patch.object(collector, "fetch_metrics", return_value=payload()),
                patch.object(collector.os, "replace", side_effect=PermissionError),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(collector.collect_once("master", output), 2)
            self.assertEqual(output.read_text(), original)
            self.assertEqual(list(Path(tmp).iterdir()), [output])

    def test_fsync_failure_keeps_old_complete_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "capacity.prom"
            output.write_text("old-complete\n")
            with (
                patch.object(collector.os, "fsync", side_effect=OSError),
                self.assertRaises(OSError),
            ):
                collector.atomic_write(output, "new-complete\n")
            self.assertEqual(output.read_text(), "old-complete\n")
            self.assertEqual(list(Path(tmp).iterdir()), [output])

    @patch.object(collector.subprocess, "run")
    @patch.object(collector.http.client, "HTTPConnection")
    def test_http_is_direct_bounded_and_whole_body_not_forwarded(
        self, connection_class, run
    ):
        run.return_value = Mock(stdout=b'{"bridge":{"IPAddress":"172.24.0.7"}}')
        connection = connection_class.return_value
        response = connection.getresponse.return_value
        response.status = 200
        response.getheader.return_value = None
        response.read.return_value = payload()
        self.assertEqual(collector.fetch_metrics("master"), payload())
        connection_class.assert_called_once_with("172.24.0.7", 9003, timeout=3)
        response.read.assert_called_once_with(collector.MAX_RESPONSE + 1)
        connection.close.assert_called_once()
        self.assertEqual(run.call_args.kwargs["timeout"], 3)
        self.assertEqual(run.call_args.args[0][-1], "master")

    @patch.object(collector.subprocess, "run")
    @patch.object(collector.http.client, "HTTPConnection")
    def test_http_status_length_and_oversize_fail(self, connection_class, run):
        run.return_value = Mock(stdout=b'{"bridge":{"IPAddress":"172.24.0.7"}}')
        response = connection_class.return_value.getresponse.return_value
        for status, length, body in (
            (503, None, b""),
            (200, str(collector.MAX_RESPONSE + 1), b""),
            (200, None, b"x" * (collector.MAX_RESPONSE + 1)),
        ):
            response.status, response.read.return_value = status, body
            response.getheader.return_value = length
            with (
                self.subTest(status=status, length=length),
                self.assertRaises(ValueError),
            ):
                collector.fetch_metrics("master")

    @patch.object(collector.subprocess, "run")
    def test_bad_container_network_never_becomes_arbitrary_http(self, run):
        for raw in (
            b"{}",
            b"[]",
            b'{"net":null}',
            b'{"net":{"IPAddress":"8.8.8.8"}}',
            b'{"net":{"IPAddress":"127.0.0.1"}}',
            b'{"a":{"IPAddress":"172.24.0.1"},"b":{"IPAddress":"172.25.0.1"}}',
        ):
            run.return_value = Mock(stdout=raw)
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                collector.fetch_metrics("master")

    def test_docker_timeout_publishes_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "capacity.prom"
            with (
                patch.object(
                    collector.subprocess,
                    "run",
                    side_effect=subprocess.TimeoutExpired("docker", 3),
                ),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                self.assertEqual(collector.collect_once("master", output), 1)
            self.assertNotIn("registered_bytes", output.read_text())


if __name__ == "__main__":
    unittest.main()

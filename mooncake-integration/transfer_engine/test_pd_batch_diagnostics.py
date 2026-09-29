"""CPU loopback acceptance for the actual classic Python native binding."""

import ctypes
import os
import socket
import unittest

os.environ["MC_FORCE_TCP"] = "1"
os.environ["MC_LEGACY_RPC_PORT_BINDING"] = "1"

from mooncake.engine import TransferEngine


def endpoint():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{sock.getsockname()[1]}"


class NativeBatchDiagnosticsTest(unittest.TestCase):
    def test_real_completed_tcp_batch_and_empty_invalid_controls(self):
        source, destination = TransferEngine(), TransferEngine()
        source_name, destination_name = endpoint(), endpoint()
        self.assertEqual(source.initialize(source_name, "P2PHANDSHAKE", "tcp", ""), 0)
        self.assertEqual(destination.initialize(destination_name, "P2PHANDSHAKE", "tcp", ""), 0)
        payload = ctypes.create_string_buffer(bytes(range(256)) * 32)
        target = ctypes.create_string_buffer(len(payload))
        source_address, target_address = ctypes.addressof(payload), ctypes.addressof(target)
        self.assertEqual(source.register_memory(source_address, len(payload)), 0)
        self.assertEqual(destination.register_memory(target_address, len(target)), 0)
        sizes = [4096, len(payload) - 4096]
        record = source.batch_transfer_sync_write_diagnostic(
            destination_name, [source_address, source_address + 4096],
            [target_address, target_address + 4096], sizes,
        )
        self.assertEqual(record["result"], 0)
        self.assertFalse(record["diagnostics_truncated"])
        self.assertEqual(len(record["attempts"]), 1)
        native = record["attempts"][0]
        self.assertEqual(native["terminal_status"], "completed")
        self.assertEqual(native["transferred_bytes"], sum(sizes))
        self.assertEqual(native["selected_transports"], {"tcp": 2})
        self.assertEqual(native["missing_transports"], 0)
        self.assertEqual(target.raw, payload.raw)
        empty = source.batch_transfer_sync_write_diagnostic(destination_name, [], [], [])
        self.assertEqual(empty["result"], 0)
        self.assertEqual(empty["attempts"][0]["task_count"], 0)
        invalid = source.batch_transfer_sync_write_diagnostic(
            destination_name, [source_address], [], [1]
        )
        self.assertEqual(invalid["result"], -1)
        self.assertEqual(invalid["attempts"], [])
        self.assertGreater(empty["batch_sequence"], record["batch_sequence"])
        self.assertEqual(source.unregister_memory(source_address), 0)
        self.assertEqual(destination.unregister_memory(target_address), 0)


if __name__ == "__main__":
    unittest.main()

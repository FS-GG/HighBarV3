"""Focused contract tests for correlated coordinator admission results."""

import importlib.util
import threading
import time
import unittest
from pathlib import Path

from highbar import commands_pb2, coordinator_pb2


_PATH = (Path(__file__).parents[2] / "specs" / "002-live-headless-e2e"
         / "examples" / "coordinator.py")
_SPEC = importlib.util.spec_from_file_location("highbar_example_coordinator", _PATH)
coordinator = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(coordinator)


class ActiveContext:
    def is_active(self):
        return True


def batch(sequence=1, correlation=(1 << 63) + 17):
    value = commands_pb2.CommandBatch(
        batch_seq=sequence,
        client_command_id=correlation,
        target_unit_id=9,
    )
    value.commands.add().move_unit.unit_id = 9
    return value


def activate(relay, plugin="plugin", incarnation="incarnation"):
    relay.activate_command_channel(
        plugin,
        incarnation,
        coordinator_pb2.ADMISSION_RESULT_PROTOCOL_CORRELATED_V1,
    )


def accepted(value):
    return commands_pb2.CommandBatchResult(
        batch_seq=value.batch_seq,
        client_command_id=value.client_command_id,
        status=commands_pb2.COMMAND_BATCH_ACCEPTED,
        accepted_command_count=len(value.commands),
    )


class ResultTransportTests(unittest.TestCase):
    def test_pending_before_forward_preserves_uint64_identity(self):
        relay = coordinator.Relay(result_timeout=0.5)
        activate(relay)
        value = batch()
        key, pending = relay.register_and_forward(value)
        queued_incarnation, queued = relay.cmd_forward.get_nowait()
        self.assertEqual(queued_incarnation, "incarnation")
        self.assertEqual(queued.client_command_id, (1 << 63) + 17)
        self.assertEqual(
            relay.complete_result("plugin", "incarnation", accepted(value)),
            coordinator_pb2.COMMAND_BATCH_RESULT_RECORDED)
        result = relay.await_result(key, pending, ActiveContext())
        self.assertEqual(result.client_command_id, value.client_command_id)

    def test_duplicate_wrong_incarnation_and_late_do_not_cross_complete(self):
        relay = coordinator.Relay(result_timeout=0.5)
        activate(relay)
        first = batch(1, 11)
        key, pending = relay.register_and_forward(first)
        with self.assertRaises(PermissionError):
            relay.complete_result("plugin", "old-incarnation", accepted(first))
        self.assertFalse(pending["event"].is_set())
        self.assertEqual(
            relay.complete_result("plugin", "incarnation", accepted(first)),
            coordinator_pb2.COMMAND_BATCH_RESULT_RECORDED)
        self.assertEqual(
            relay.complete_result("plugin", "incarnation", accepted(first)),
            coordinator_pb2.COMMAND_BATCH_RESULT_DUPLICATE)
        relay.await_result(key, pending, ActiveContext())
        late = batch(2, 22)
        self.assertEqual(
            relay.complete_result("plugin", "incarnation", accepted(late)),
            coordinator_pb2.COMMAND_BATCH_RESULT_LATE)

    def test_second_owner_rejected_and_disconnect_is_unknown(self):
        relay = coordinator.Relay(result_timeout=0.5)
        activate(relay)
        with self.assertRaises(RuntimeError):
            activate(relay, plugin="second", incarnation="second-incarnation")
        value = batch()
        key, pending = relay.register_and_forward(value)
        relay.deactivate_command_channel("plugin", "incarnation")
        with self.assertRaisesRegex(RuntimeError, "disconnected after forwarding"):
            relay.await_result(key, pending, ActiveContext())

    def test_timeout_unknown_and_legacy_never_fabricates_result(self):
        relay = coordinator.Relay(result_timeout=0.01)
        activate(relay)
        key, pending = relay.register_and_forward(batch())
        with self.assertRaises(TimeoutError):
            relay.await_result(key, pending, ActiveContext())

        legacy = coordinator.Relay(result_mode="legacy-observation-only")
        legacy.activate_command_channel(
            "old-plugin", "", coordinator_pb2.ADMISSION_RESULT_PROTOCOL_UNSPECIFIED)
        self.assertIsNone(legacy.register_and_forward(batch()))

    def test_immediate_result_race_and_concurrent_submitters_distinct(self):
        relay = coordinator.Relay(result_timeout=0.5)
        activate(relay)
        observed = []

        def submit(sequence):
            value = batch(sequence, (1 << 63) + sequence)
            key, pending = relay.register_and_forward(value)
            while True:
                incarnation, forwarded = relay.cmd_forward.get(timeout=0.5)
                if forwarded.batch_seq == sequence:
                    relay.complete_result("plugin", incarnation, accepted(forwarded))
                    break
                relay.cmd_forward.put((incarnation, forwarded))
                time.sleep(0.001)
            observed.append(relay.await_result(key, pending, ActiveContext()))

        threads = [threading.Thread(target=submit, args=(sequence,))
                   for sequence in (31, 32)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(
            {(item.batch_seq, item.client_command_id) for item in observed},
            {(31, (1 << 63) + 31), (32, (1 << 63) + 32)})


if __name__ == "__main__":
    unittest.main()

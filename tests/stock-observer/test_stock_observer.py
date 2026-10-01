#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]
VECTORS = json.loads((ROOT / "contracts/barc-stock-queue-v1/vectors.json").read_text())
VECTOR = {item["name"]: item for item in VECTORS["vectors"]}
HARNESS = ROOT / "tests/stock-observer/mock_spring.lua"
PACKER = ROOT / "scripts/pack-barc-stock-observer.py"


def with_length(lines: list[str]) -> bytes:
    document = "\n".join(lines) + "\n"
    document = document.replace("length=00000000", f"length={len(document.encode('ascii')):08d}", 1)
    return document.encode("ascii")


def request(domain="production", unit=42, bridge="barc-stock-queue-reader-v1", version="1") -> bytes:
    return with_length([f"BARC_QUEUE_REQUEST/{version}", "length=00000000", f"bridge={bridge}", f"domain={domain}", f"unit={unit}", "end"])


def run_lua(payload: bytes, case: str) -> bytes:
    with tempfile.NamedTemporaryFile() as source:
        source.write(payload); source.flush()
        completed = subprocess.run(["lua", str(HARNESS), source.name, case], cwd=ROOT, check=True, capture_output=True)
    return completed.stdout


def fields(response: bytes) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for line in response.decode("ascii").splitlines()[1:]:
        if "=" in line:
            key, value = line.split("=", 1); result.setdefault(key, []).append(value)
    return result


class StockObserverTests(unittest.TestCase):
    def test_restricted_synced_surface_uses_packf32_and_no_wide_word_arithmetic(self):
        source = (ROOT / "data/barc-stock-observer/LuaRules/Gadgets/barc_stock_queue_reader.lua").read_text()
        self.assertNotIn("rawget", source)
        self.assertNotIn("bit32", source)
        self.assertNotIn("0x428a2f98", source)
        self.assertNotIn("4294967296", source)
        self.assertIn("VFS.PackF32", source)
        self.assertIn('"428a2f98"', source)

    def test_contract_vectors_are_byte_exact(self):
        self.assertEqual(request(), VECTOR["production-request"]["ascii"].encode())
        self.assertEqual(run_lua(request(), "empty"), VECTOR["empty-production"]["ascii"].encode())
        self.assertEqual(run_lua(request(), "negative-zero"), VECTOR["one-production-with-negative-zero"]["ascii"].encode())
        self.assertEqual(run_lua(request(), "wrong-team"), VECTOR["wrong-team-unavailable"]["ascii"].encode())

    def test_domain_selects_exact_stock_reader_and_preserves_coded_options(self):
        response = run_lua(request("rally"), "rally")
        parsed = fields(response)
        self.assertEqual(parsed["status"], ["ok"])
        self.assertEqual(parsed["domain"], ["rally"])
        self.assertEqual(parsed["row"], ["rally|-701|32|41|3f800000"])

    def test_team_and_factory_scope_are_closed(self):
        self.assertEqual(fields(run_lua(request(), "wrong-team"))["reason"], ["wrong-team"])
        self.assertEqual(fields(run_lua(request(), "not-factory"))["reason"], ["not-factory"])

    def test_actor_zero_is_valid_and_still_team_scoped(self):
        accepted = fields(run_lua(request(unit=0), "actor0"))
        self.assertEqual(accepted["status"], ["ok"])
        self.assertEqual(accepted["unit"], ["0"])
        refused = fields(run_lua(request(unit=0), "actor0-wrong-team"))
        self.assertEqual(refused["reason"], ["wrong-team"])
        self.assertEqual(refused["unit"], ["0"])

    def test_float32_bytes_preserve_low_bits_and_extremes(self):
        low_bit = fields(run_lua(request(), "mantissa-low-bit"))["row"][0]
        self.assertEqual(low_bit.rsplit("|", 1)[1], "3f800001")
        boundaries = fields(run_lua(request(), "float-boundaries"))["row"][0]
        self.assertEqual(boundaries.rsplit("|", 1)[1],
                         "00000001,007fffff,7f7fffff")

    def test_entry_and_parameter_bounds_refuse_without_truncation(self):
        for case in ("overflow65", "params17", "total257"):
            parsed = fields(run_lua(request(), case))
            self.assertEqual(parsed["status"], ["unavailable"])
            self.assertEqual(parsed["reason"], ["overflow"])
            self.assertEqual(parsed["count"], ["0"])

    def test_exact_entry_and_parameter_boundaries_are_complete(self):
        for case, count in (("entries64", "64"), ("params16", "1"), ("total256", "16")):
            parsed = fields(run_lua(request(), case))
            self.assertEqual(parsed["status"], ["ok"])
            self.assertEqual(parsed["count"], [count])
            self.assertEqual(len(parsed["row"]), int(count))

    def test_missing_or_invalid_stock_command_shape_refuses(self):
        for case in ("bad-command", "nil-commands"):
            parsed = fields(run_lua(request(), case))
            self.assertEqual(parsed["status"], ["unavailable"])
            self.assertEqual(parsed["reason"], ["malformed"])

    def test_nonfinite_float_refuses(self):
        parsed = fields(run_lua(request(), "nonfinite"))
        self.assertEqual(parsed["reason"], ["nonfinite-float"])

    def test_version_and_bridge_refusals_keep_exact_request_binding(self):
        for payload, reason in ((request(version="2"), "wrong-version"), (request(bridge="other"), "missing-bridge")):
            parsed = fields(run_lua(payload, "empty"))
            self.assertEqual(parsed["reason"], [reason])
            self.assertEqual(parsed["request-sha256"], [hashlib.sha256(payload).hexdigest()])
            self.assertEqual(parsed["domain"], ["production"]); self.assertEqual(parsed["unit"], ["42"])

    def test_malformed_nul_oversize_and_unknown_domain_have_no_response(self):
        malformed = request().replace(b"bridge=", b"unit=", 1)
        for payload in (malformed, request() + b"\0", b"BARC_QUEUE_REQUEST/1\n" + b"x" * 8192, request("economy")):
            self.assertEqual(run_lua(payload, "empty"), b"__NIL__")

    def test_every_response_is_bounded_ascii_and_self_lengths(self):
        for case in ("empty", "negative-zero", "wrong-team", "overflow65", "params17", "total257", "nonfinite"):
            response = run_lua(request(), case)
            self.assertLessEqual(len(response), 8192); self.assertNotIn(b"\0", response)
            self.assertTrue(all(byte < 128 for byte in response)); self.assertTrue(response.endswith(b"end\n"))
            parsed = fields(response); self.assertEqual(int(parsed["length"][0]), len(response))
            self.assertTrue(all(len(line) + 1 <= 512 for line in response.splitlines()))

    def test_package_is_deterministic_archive_only_and_pins_base_dependency(self):
        metadata = json.loads((ROOT / "data/barc-stock-observer/CONTENT-METADATA.json").read_text())
        self.assertEqual(metadata["baseGameDependency"], "Beyond All Reason test-29926-0571aa8")
        before = {p: hashlib.sha256((ROOT / "data/barc-stock-observer" / p).read_bytes()).hexdigest() for p in (
            "CONTENT-METADATA.json", "LuaRules/Gadgets/barc_stock_queue_reader.lua", "modinfo.lua")}
        with tempfile.TemporaryDirectory() as temporary:
            hashes=[]
            for index in (1,2):
                archive=Path(temporary)/f"barc-stock-observer-{index}.sdz"; inventory=Path(temporary)/f"inventory-{index}.json"
                subprocess.run([sys.executable,str(PACKER),"--source",str(ROOT/"data/barc-stock-observer"),"--output",str(archive),"--inventory",str(inventory)],check=True,cwd=ROOT)
                hashes.append(hashlib.sha256(archive.read_bytes()).hexdigest())
                with zipfile.ZipFile(archive) as package:
                    self.assertEqual(package.namelist(), ["CONTENT-METADATA.json","LuaRules/Gadgets/barc_stock_queue_reader.lua","modinfo.lua"])
                    self.assertNotIn("LuaRules/Gadgets/barc_stock_queue_reader.lua", [p.name for p in archive.parent.iterdir()])
            self.assertEqual(hashes[0], hashes[1])
        after = {p: hashlib.sha256((ROOT / "data/barc-stock-observer" / p).read_bytes()).hexdigest() for p in before}
        self.assertEqual(before, after)


if __name__ == "__main__": unittest.main()

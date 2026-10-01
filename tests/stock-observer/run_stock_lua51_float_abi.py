#!/usr/bin/env python3
"""Build exact stock Recoil Lua 5.1 with float numbers and run observer regressions."""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import subprocess
import tempfile

EXPECTED_RECOIL = "2639eedac7d1fd67d793ec93ebd27f014f336a14"
ROOT = pathlib.Path(__file__).resolve().parents[2]


def request(domain="production", unit=42):
    lines = ["BARC_QUEUE_REQUEST/1", "length=00000000", "bridge=barc-stock-queue-reader-v1",
             f"domain={domain}", f"unit={unit}", "end"]
    value = "\n".join(lines) + "\n"
    return value.replace("length=00000000", f"length={len(value.encode()):08d}", 1).encode()


def fields(value: bytes):
    result = {}
    for line in value.decode("ascii").splitlines()[1:]:
        if "=" in line:
            key, item = line.split("=", 1)
            result.setdefault(key, []).append(item)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recoil-source", type=pathlib.Path, required=True)
    args = parser.parse_args()
    source = args.recoil_source.resolve()
    revision = subprocess.run(["git", "-C", source, "rev-parse", "HEAD"], check=True,
                              text=True, capture_output=True).stdout.strip()
    if revision != EXPECTED_RECOIL:
        raise SystemExit(f"wrong Recoil source: {revision}")
    lua = source / "rts/lib/lua"
    config = (lua / "include/luaconf.h").read_text()
    if "#define LUA_NUMBER\tfloat" not in config or "#define LUA_VERSION_NUM\t501" not in (lua / "include/lua.h").read_text():
        raise SystemExit("stock float Lua 5.1 ABI not present")
    sources = sorted((lua / "src").glob("*.cpp")) + [ROOT / "tests/stock-observer/stock_lua51_float_runner.cpp"]
    with tempfile.TemporaryDirectory(prefix="bar-stock-lua51-") as temporary:
        build = pathlib.Path(temporary)
        includes = [f"-I{lua / 'include'}", f"-I{source / 'rts/lib/streflop'}", f"-I{source / 'rts'}"]
        common = ["g++", "-std=c++17", "-O2", "-DBUILDING_AI", "-DNOT_USING_STREFLOP", *includes]
        def compile_one(item):
            output = build / (item.stem + ".o")
            subprocess.run([*common, "-c", item, "-o", output], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            return output
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            objects = list(executor.map(compile_one, sources))
        runner = build / "lua51-float-runner"
        subprocess.run(["g++", *objects, "-ldl", "-lm", "-o", runner], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        harness = ROOT / "tests/stock-observer/mock_spring.lua"
        def run(payload, case):
            request_path = build / "request.bin"
            request_path.write_bytes(payload)
            return subprocess.run([runner, request_path, case, harness], cwd=ROOT, check=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

        vectors = json.loads((ROOT / "contracts/barc-stock-queue-v1/vectors.json").read_text())["vectors"]
        vector = {item["name"]: item["ascii"].encode() for item in vectors}
        canonical = request()
        assert canonical == vector["production-request"]
        assert run(canonical, "empty") == vector["empty-production"]
        assert run(canonical, "negative-zero") == vector["one-production-with-negative-zero"]
        assert run(canonical, "wrong-team") == vector["wrong-team-unavailable"]
        assert fields(run(canonical, "mantissa-low-bit"))["row"][0].endswith("|3f800001")
        assert fields(run(canonical, "float-boundaries"))["row"][0].endswith(
            "|00000001,007fffff,7f7fffff")
        actor_zero = fields(run(request(unit=0), "actor0"))
        assert actor_zero["status"] == ["ok"] and actor_zero["unit"] == ["0"]
        actor_zero_wrong = fields(run(request(unit=0), "actor0-wrong-team"))
        assert actor_zero_wrong["reason"] == ["wrong-team"]
        print(json.dumps({"accepted": True, "luaVersion": "5.1", "luaNumber": "float",
                          "recoilRevision": revision, "vectors": 4,
                          "numericVectors": ["3f800001", "00000001", "007fffff", "7f7fffff", "80000000"],
                          "sourceSha256": hashlib.sha256((ROOT / "data/barc-stock-observer/LuaRules/Gadgets/barc_stock_queue_reader.lua").read_bytes()).hexdigest()},
                         sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    main()

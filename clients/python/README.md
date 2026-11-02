# HighBar Python Client

Python 3.11+ client for the HighBarV3 gRPC gateway.

## Install (dev)

```bash
cd clients/python
python -m venv .venv && source .venv/bin/activate
pip install -e '.[dev]'

# Regenerate gRPC stubs into highbar_client/highbar/.
make codegen
```

`make codegen` runs `python -m grpc_tools.protoc` with the right
include paths — it picks up every `proto/highbar/*.proto` and writes
`*_pb2.py` / `*_pb2_grpc.py` under `highbar_client/highbar/`. Other
targets:

| `make` target | What it does                                         |
|---------------|------------------------------------------------------|
| `codegen`     | regenerate stubs (above)                             |
| `install-dev` | `pip install -e '.[dev]'`                            |
| `test`        | run pytest                                           |
| `clean`       | remove generated stubs + `__pycache__`               |

The generated files **are committed** under
`highbar_client/highbar/` (per T036) so users do not need a
`grpcio-tools` install just to import the package; only contributors
who change `.proto` files or the generator need to run `make codegen`.
The generator is pinned so committed stubs reproduce exactly. Runtime minimums
match its generated guards; see [client dependency qualification](../../docs/client-dependencies.md).
For the resolved development environment, use `uv sync --locked --extra dev`.

## Live-gateway environment

The direct live topology helper and the headless scripts use the native
plugin as a same-host gRPC endpoint. Some legacy acceptance scripts can
also pass a coordinator endpoint for plugin dial-out tests. The common
environment variables are:

| Var                    | Used by                       | Notes                                                 |
|------------------------|-------------------------------|-------------------------------------------------------|
| `SPRING_HEADLESS`      | `_launch.sh`, every script    | Path to the pinned spring-headless binary. Skip-77 if unset. |
| `HIGHBAR_TOKEN_PATH`   | `live_topology`, admin suite  | Token file used by AI/admin RPCs in direct-gateway runs. |
| `HIGHBAR_COORDINATOR`  | `_launch.sh`                  | Optional coordinator endpoint for dial-out tests (e.g. `unix:/tmp/hb-coord.sock`). |
| `HIGHBAR_WRITE_DIR`    | launchers, fault checks       | Directory where the plugin writes runtime files. Defaults to `$HOME/.local/state/Beyond All Reason`. |
| `HIGHBAR_ENABLE_BUILTIN` | launchers                   | Compatibility key. Always forced to `false`; built-in BARb behavior cannot be re-enabled. |
| `HIGHBAR_BENCH_DURATION` | `latency-{uds,tcp}.sh`      | Bench window in seconds. Default 30.                  |
| `HIGHBAR_BENCH_SAMPLES`  | `latency-{uds,tcp}.sh`      | Hard cap on sample count. Default 1000.               |
| `HIGHBAR_TCP_BIND`     | `latency-tcp.sh`              | Coordinator TCP bind address. Default `127.0.0.1:50521`. |

Use `highbar_client.live_topology.run_topology(...)` for Python-driven
live runs. It writes the generated startscripts, token path, socket path,
BNV viewer launch, and report location from one `TopologyOptions`
object.

## Usage (observer)

```python
import grpc
from highbar import service_pb2, service_pb2_grpc

ch = grpc.insecure_channel('unix:/tmp/hb-coord.sock')
stub = service_pb2_grpc.HighBarProxyStub(ch)

hello = stub.Hello(service_pb2.HelloRequest(
    schema_version='1.0.0',
    role=service_pb2.Role.ROLE_OBSERVER,
    client_id='my-tool/0.1.0',
))
print('connected', hello.session_id, 'schema', hello.schema_version)

for update in stub.StreamState(
    service_pb2.StreamStateRequest(resume_from_seq=0)
):
    print(update.seq, update.frame)
```

## Usage (AI role)

```python
from highbar import service_pb2, service_pb2_grpc, commands_pb2

stub = service_pb2_grpc.HighBarProxyStub(ch)
stub.Hello(service_pb2.HelloRequest(
    schema_version='1.0.0',
    role=service_pb2.Role.ROLE_AI,
    client_id='my-ai/0.1.0',
))

batch = commands_pb2.CommandBatch(batch_seq=1)
mv = batch.commands.add().move_unit
mv.unit_id = 42
mv.target.x = 1024.0
mv.target.y = 0.0
mv.target.z = 1024.0
ack = stub.SubmitCommands(iter([batch]))
print('ack', ack.batches_accepted)
```

Strict-mode-ready command batches should set `client_command_id`,
`based_on_frame`, `based_on_state_seq`, and `conflict_policy`. The
helper `highbar_client.commands.batch(...)` accepts those optional
fields, and `commands.issue_summary(result)` turns structured
diagnostics into `(code, command_index, field_path, retry_hint)` tuples.

Use `ValidateCommandBatch` before `SubmitCommands` when a generated
client wants a dry run with no queue or simulation mutation. Capability
discovery is available through `GetCommandSchema` and
`GetUnitCapabilities`; the returned feature flags, supported arms,
option masks, queue depth/capacity, and unit legal arms are the stable
inputs for generated clients.

Admin controls are intentionally separate from AI commands. Use
`highbar_client.admin` helpers with the normal token plus run-scoped
admin metadata (`x-highbar-admin-role` and `x-highbar-client-id`) for
`GetAdminCapabilities`, `ValidateAdminAction`, and
`ExecuteAdminAction`.

`highbar_client.admin.unit_transfer_action(...)` builds the additive
`unit_transfer` admin action for transferring ownership of an existing
unit. Capability responses advertise `unit_transfer`, speed bounds,
valid fixture ids, map extents, and run-mode flags when the gateway can
execute those controls. The admin behavioral suite writes reviewable
evidence with:

```bash
tests/headless/admin-behavioral-control.sh \
  --startscript tests/headless/scripts/admin-behavior.startscript \
  --output-dir build/reports/admin-behavior \
  --timeout-seconds 10
```

See `specs/002-live-headless-e2e/examples/{observer,ai_client}.py`
for runnable end-to-end demos.

For cross-language client generation and the full service/proto map, see
[`../../docs/client-development.md`](../../docs/client-development.md)
and [`../../docs/proto-reference.md`](../../docs/proto-reference.md).

## Python AI plugins

The Spring-side Skirmish AI remains the native `highBar` proxy. Python
AI plugins are external policies that connect through `HighBarProxy` as
`ROLE_AI`, consume `StreamState`, and submit command batches back through
the same proxy.

Run a built-in policy:

```bash
hb-ai-runner-py \
  --transport uds \
  --uds-path /tmp/hb-run/highbar-1.sock \
  --token-file /tmp/highbar.token \
  --ai-plugin idle \
  --name-addon scout-a
```

Run a smoke movement policy:

```bash
hb-ai-runner-py \
  --transport uds \
  --uds-path /tmp/hb-run/highbar-1.sock \
  --token-file /tmp/highbar.token \
  --ai-plugin move-once \
  --plugin-config '{"target_unit":42,"move_to":"1024,0,1024"}' \
  --name-addon flank-test
```

Run the defensive macro policy:

```bash
hb-ai-runner-py \
  --transport uds \
  --uds-path /tmp/hb-run/highbar-1.sock \
  --token-file /tmp/highbar.token \
  --ai-plugin turtle1 \
  --name-addon turtle1-a
```

`turtle1` resolves BAR unit names through callbacks, checks each
builder's runtime build options, then builds economy, defenses,
factories, tech, and a mixed army near its own start area. It does not
emit enemy-directed move, fight, patrol, or attack commands.

## Live topology helper

For local integration runs that need the host, optional BNV viewer, and
Python AI policy wired together, the constitution requires
`highbar_client.live_topology`. Python-driven live/topology runs MUST use
`run_topology(...)` with a `TopologyOptions` object instead of
reconstructing the launch shell:

```python
from dataclasses import replace

from highbar_client.live_topology import run_topology, turtlevsnull

result = run_topology(replace(turtlevsnull, duration_seconds=20.0))
print(result.report_path, result.updates, result.batches_submitted)
```

`turtlevsnull` is a saved `TopologyOptions` preset for `turtle1` versus
`NullAI` with a BNV viewer. The helper writes generated startscripts,
logs, and `report.md` under the preset's `run_dir`, then cleans up the
host/viewer processes by default. Override fields with
`dataclasses.replace(...)` for different ports, run durations, plugin
artifacts, engine paths, or viewer settings.

For admin-control demos and live behavioral verification, use the saved
`adminbehaviornullbnv` preset. It launches `highBar` versus `NullAI`,
attaches BNV during the pregame window, starts the autohost relay needed
for global speed control, and runs the admin behavioral suite against
the same host:

```python
from highbar_client.live_topology import adminbehaviornullbnv, run_topology

result = run_topology(adminbehaviornullbnv)
print(result.viewer_status, result.admin_behavior_report_path)
```

Load a custom policy with `module:factory`:

```python
from collections.abc import Iterable

from highbar_client.ai_plugins import AIPluginContext, BaseAIPlugin
from highbar_client.highbar import commands_pb2, state_pb2


class MyPolicy(BaseAIPlugin):
    name = "my-policy"
    version = "0.1.0"

    def on_state(
        self,
        context: AIPluginContext,
        update: state_pb2.StateUpdate,
    ) -> Iterable[commands_pb2.CommandBatch]:
        return ()


def create(config):
    return MyPolicy()
```

Then run `hb-ai-runner-py --ai-plugin my_module:create ...`.
`--name-addon` is sanitized and appended to the gRPC `client_id`, so
multiple Python policies running through the same unchanged proxy can be
distinguished in coordinator logs.

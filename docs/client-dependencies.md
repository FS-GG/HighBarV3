# Client dependency qualification

[Delivery #5256](https://github.com/FS-GG/.github/issues/5256) refreshes the
Python and .NET clients from source `b57f11f290dc3239d894b0285ae272b6b21bc25d`.
Python remains 3.11+ and the F# projects remain `net8.0`.

## Stable package selection

Observed on 2026-11-02 at 14:13 UTC. PyPI versions below are the greatest
non-prerelease versions with non-yanked files. NuGet versions are the greatest
listed, non-prerelease versions in their registration catalogs.

| Package | Stable version | Selection |
|---|---|---|
| [grpcio](https://pypi.org/pypi/grpcio/json) | 1.85.1 | Runtime minimum matches generated gRPC guard |
| [grpcio-tools](https://pypi.org/pypi/grpcio-tools/json) | 1.85.1 | Exact development generator for reproducible committed output |
| [protobuf](https://pypi.org/pypi/protobuf/json) | 7.36.2 | Lockfile resolves stable; runtime range `>=7.35.1,<8` matches generator |
| [setuptools](https://pypi.org/pypi/setuptools/json) | 84.2.1 | Existing build range admits stable; installed and used successfully |
| [wheel](https://pypi.org/pypi/wheel/json) | 0.48.0 | Existing build requirement admits stable; installed successfully |
| [pytest](https://pypi.org/pypi/pytest/json) | 9.2.0 | Existing development range admits stable; resolved in lockfile |
| [pytest-asyncio](https://pypi.org/pypi/pytest-asyncio/json) | 1.4.1.post1 | Existing development range admits stable; resolved in lockfile |
| [Google.Protobuf](https://api.nuget.org/v3/registration5-gz-semver2/google.protobuf/index.json) | 3.36.2 | Explicit project reference |
| [Grpc.Net.Client](https://api.nuget.org/v3/registration5-gz-semver2/grpc.net.client/index.json) | 2.85.0 | Explicit project reference, includes net8 assets |
| [Grpc.Tools](https://api.nuget.org/v3/registration5-gz-semver2/grpc.tools/index.json) | 2.85.0 | Build-only generator, retained `PrivateAssets=all` |

`grpcio-tools` 1.85.1 requires protobuf `>=7.35.1,<8` and emits code guarded
at 7.35.1. The former protobuf 6.x range and generator cap excluded this
coherent toolchain. The regenerated gRPC code requires grpcio 1.85.1, so its
runtime floor changes too. Broad build/test ranges that already admit stable
remain unchanged. The Python lockfile was produced by `uv lock --upgrade`.

All eight Python proto modules and their gRPC companions were regenerated
with the existing `make codegen` command's `grpc_tools.protoc` invocation.
This also incorporates existing coordinator/state schema additions and the
previously uncommitted `live_control` modules. No proto source changed. C#
outputs are generated during the normal `Grpc.Tools` build and remain build
artifacts. Repository licensing and upstream provenance are unchanged.

## Local qualification

Local work used isolated virtual environments and NuGet storage, limited to
one CPU and 2 GiB per command. The retained delivery evidence includes feed
metadata, install reports, generated-file hashes and original failure logs.

- Clean pip development install on Python 3.11.18 selected every stable Python
  version above; `pip check` passed.
- A second environment installed with `uv sync --locked --extra dev` and
  regenerated all 16 files byte-for-byte.
- The maintained Python suite reported **263 passed, 4 skipped**. The skipped
  observer/AI cases require `HIGHBAR_UDS_PATH`, and AI cases also require
  `HIGHBAR_TOKEN_PATH`, from a running native engine. They were not qualified.
- A built wheel installed into a third clean environment with the declared
  minimum grpcio 1.85.1 and protobuf 7.35.1. Dependency checks and actual
  transport calls passed outside the source directory.
- SDK 10.0.403 restored and compiled the net8 proto/client projects, Observer,
  AiClient and Latency consumers. The first library build reported the
  existing duplicate `HighBar.Proto` reference conflict warning; consumer
  builds completed with zero warnings and errors.
- Python and F# clients completed Hello with AI metadata, two-message state
  streaming, admin capability reads and permission-denied propagation over
  both loopback TCP and Unix sockets against a local generated gRPC service.
  F# execution used the SHA512-verified .NET 8.0.33 runtime. This fixture checks
  client serialization and transport; it does not establish native engine
  behavior or the engine-event latency budget.

The host lacks `make`; after retaining that initial command failure, local
Python generation ran the Makefile's exact protoc command directly. The first
F# transport harness incorrectly expected a bare `RpcException` from
`Async.RunSynchronously`; its expected-rejection check was corrected to accept
the observed `AggregateException` wrapper. No product fix was needed for that
harness failure. Pytest-asyncio's existing unset fixture-loop-scope warning
remains advisory.

## Hosted qualification

The Python CI job now installs the project's development extra, checks
resolved dependencies, regenerates with the pinned tool and rejects drift in
committed stubs before running tests. Existing Actions versions and native
jobs are unchanged. Hosted checks, independent review and merge acceptance
are recorded on the owning Delivery; local results do not replace them.

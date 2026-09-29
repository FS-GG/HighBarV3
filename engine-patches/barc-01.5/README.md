# BARC-01.5 Recoil rally queue API

This directory records the bounded engine-side API extension used by the
BARC-01.5 producer rally work.

## Source identity

- Upstream base: `2639eedac7d1fd67d793ec93ebd27f014f336a14`
- Local implementation commit: `7555c836e0457b04100b20149497d088dd9c2ca7`
- Version suffix: `BARC-01.5-rally-api-v1`
- Patch: `recoil-rally-api.patch`
- Recursive submodule pins: `submodules.txt`

Reproduction starts at the upstream base and applies the patch to the index.
The build script then recreates the recorded commit object from its exact tree,
parent, message, author, and timestamp, verifies the resulting full hash, and
checks it out before configuring. Reproduced binaries therefore carry the same
safe version identity required by consumers rather than a dirty-base identity.

## Callback contract

The patch appends typed queue callbacks after the prior final
`SSkirmishAICallback` field. Queue domains use `CCommandQueue::QueueType`:
normal non-factory (`0`), factory rally/new-unit (`1`), and factory production
(`2`). The factory rally queue comes from `CFactoryCAI::newUnitCommands`.

A count of zero is a valid authoritative empty queue. Count errors are `-1`
for an inaccessible actor, `-2` for unsupported or invalid actor/domain and
cheat access, and `-3` for an unrepresentable count. Every item getter repeats
the ownership/domain lookup and validates the index. Parameter reads first
report the exact total; insufficient storage copies nothing and returns the
required total.

The generated C++ wrapper carries the queue domain and command index in every
`CurrentCommandByType` object, throws on negative callback results, and rejects
a changed parameter total. Existing callback fields and behavior are unchanged.

## Compatibility boundary

Appending fields does not make old callback tables safe to read. A consumer
must identify this patched engine through callbacks present in the old stable
table before accessing any appended field. Checking an appended pointer on an
unknown or old engine would itself read beyond that table.

## Reproduction

Run `scripts/build-barc-recoil-rally.sh RECOIL_SOURCE BUILD_DIRECTORY` against
a clean checkout at the recorded upstream base. The script applies the patch,
initializes pinned submodules, configures with CMake 3.31.10 and the known
sysroot, and builds the headless engine, C AI interface, generated C++ wrapper,
and focused ABI test. Legacy and dedicated targets stay enabled because this
engine revision's headless target depends on the `Game` target produced by
that configuration. The build's `gcc16-rmlui.cmake` hook forces inclusion of
`<cstdint>` for the RmlUi debugger and headless engine targets under GCC 16.2.1,
accommodating removed transitive declarations without changing source files,
the pinned submodule, or the special streflop math target.
The script selects the sysroot's real 7z binary because its relocatable wrapper
contains an absolute host path.

The qualified artifact hashes and read-only `--version` result are recorded in
`qualification.txt` after the build completes.

For loader qualification, set `BARC_SDL2_RUNTIME` to a directory containing a
real SDL2 `libSDL2-2.0.so.0`. The sysroot otherwise supplies the remaining
libraries. The qualified run used the extracted Arch Linux SDL2 2.30.9 package
recorded in `qualification.txt`; it did not alter the executable or install a
package.

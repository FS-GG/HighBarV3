# Native dependency overlays

These overlays use the stable upstream gRPC 1.85.1, Abseil 20260817.0 and
OpenSSL 4.1.0 sources with the vcpkg recipes from the pinned registry baseline.
`source-provenance.json` records archive hashes, recipe origin and migration
scope. The copied vcpkg recipes retain their MIT license in `LICENSE.vcpkg.txt`.

Abseil already includes two older registry fixes, so those patches are removed.
gRPC uses its matching grpc-proto revision. OpenSSL 4 removes engines, SSL3 and
`c_rehash`; its overlay removes those options and updates the remaining install
patches. A scoped patch creates the generated application-header directory when
configuring out of tree with `no-apps`.

The selected Linux graph and source configuration have been checked. Full port
compilation, generated stubs, the plugin and live engine behavior require their
own qualification. These overlays do not change the engine pin or qualify a
new native execution profile.

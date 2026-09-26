# tinyactor-pkgs

Third-party packages for TinyActor; package mechanism and v1 install/discovery convention: [`docs/packages.md`](../docs/packages.md).

Layout is flat: one directory per self-contained package. Each package can include C source, TA glue/signatures, Makefile, and tests. Build a package with `make`; currently place its `.ta` and platform shared library in the consuming project's `lib/` and run TinyActor from that project root.

| Package | Tier | Status |
|---|---|---|
| sdl | T2 | planned |
| sqlite | T2 | planned |
| markdown | T2 | planned |
| yaml | T2 | planned |
| queue | T1 | planned |
| deque | T1 | planned |
| priority_queue | T1 | planned |

`_template/` is the minimal buildable C module and smoke test. The smoke test exits nonzero on failure; a nil return has its own missing-dylib hint. See [the troubleshooting section](../docs/packages.md#troubleshooting-missing-c-library). T1/T2 are planning tiers, not compatibility guarantees.

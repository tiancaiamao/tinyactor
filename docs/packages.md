# TinyActor packages (v1)

TinyActor packages are third-party modules outside the core repository. The core/stdlib boundary is `lib`: as stated in [`stdlib-port-plan.md` §4](stdlib-port-plan.md), the C module mechanism is the package mechanism. A package is a self-contained directory with optional C source, TA glue/signatures, a Makefile, and tests.

## Repository layout

`tinyactor-pkgs/` is flat: each immediate child directory is one package (plus `_template/`). Each package owns its source, glue, build rules, and tests.

## Build and discover (v1 convention)

Clone `tinyactor-pkgs` and build the desired package, then put its `.ta` signature/glue file and shared library where the project/runtime can find them. For the current implementation, the simplest arrangement is to copy both into the consuming project's `lib/` directory and run the program from that project root. For an executable called `thing.op`, runtime auto-loading tries `lib/thing.dylib` on macOS or `lib/thing.so` on Linux relative to the process working directory.

Import and runtime loading are separate:

* `import thing` resolves source `thing.ta`. The compiler searches the importing file's directory, its `helpers/`, repository-relative `lib/`, then `lib/bootstrap/`, in that order. It does not search the dylib and does not consult `TA_*_PATH`. This is the behavior of `find_module_path` in `lib/bootstrap/driver.ta:637-667`.
* Calling `thing.op` loads the C library lazily if the function is not already registered. The VM constructs `lib/thing.dylib` / `.so` and calls `dlopen`; this path is relative to the runtime's current working directory. When `TA_MOD_TAG` is defined at compile time, the VM constructs `lib/thing_<tag>.dylib` / `.so` instead (`src/vm.c:1477-1484`). **This source does not then fall back to the untagged filename**; it selects one filename at compile time. No general package search path or C-module environment variable exists in this implementation (`src/vm.c:1466-1508).
* As an end-to-end check, run `make` in a package, copy its `.ta` and platform library into the project's `lib/`, then run an importing `.ta` from the project root. See `_template` for a reproducible smoke test.

These path rules are intentionally a v1 convention, not a package manager: no automatic install command, dependency resolution, version selection, or configurable package path is provided. `lib/` is repository-relative in the compiler but runtime dylib discovery is cwd-relative; invoke the runtime from the project root to align them.

## Troubleshooting: missing C library

The `.ta` glue/signature and the shared library are separate requirements. Importing with no `thing.ta` is an import-time error from the compiler (`driver.ta`'s module resolver returns no module). If `thing.ta` is present but the expected `.dylib`/`.so` is absent or at the wrong cwd-relative path, the lazy loader's `dlopen` fails silently; when the function is still unregistered the VM pushes `nil` (`src/vm.c:1485-1500`). In a local smoke test this produced `nil`, no runtime error, and exit code 0.

In v1, if a glue function returns `nil` when that result is not expected, first suspect that the dylib is missing or installed at the wrong path. This is a known limitation; runtime diagnostics are tracked upstream in issue #203 and are not promised by this document.

## C module ABI and build

A dynamic C module exports `void vm_load_self(VM *vm)`. The VM finds that exact symbol using `dlsym` and invokes it; the entry point registers functions through `vm_register_module(vm, "thing", funcs, count)`. Each `TaFunc` contains a function name, callback, and arity; qualified names are registered as `thing.function`. The callback type is `Val fn(VM *, Val *args, int nargs)`; return a TinyActor `Val` directly. The loader returns `0` on success and `-1` on failure in the explicit API. The lazy call path likewise searches for `vm_load_self` and then retries the qualified function lookup. Sources: `src/api.c:309-325`, `src/api.c:280-307`, `src/vm.c:1466-1505`, `ta.h` (`TaFunc` and callback declarations). The existing `lib/demo.c` is a worked example.

To build a shared library, compile the module source as position-independent shared code and include the public header. The repository's demo rule is `$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o lib/demo.$(HTTP_EXT) lib/demo.c $(MOD_LDLIBS)` (`Makefile:181-185`); `.dylib` is selected on macOS and `.so` elsewhere (`Makefile:13-18`). On Linux the VM executable exports symbols for `dlopen` (`Makefile:146-151`); macOS uses `-undefined dynamic_lookup`. An independent package can follow `_template/Makefile` and adapt linker dependencies as needed.

## Package Makefile conventions (dogfooded)

The package Makefiles in `tinyactor-pkgs` provide three working examples; these are observed conventions, not requirements enforced by the core:

* Set `TINYACTOR ?= ...` to the core repository used for headers, build inputs, and (depending on the package) test installation. The sqlite package defaults to `../tinyactor`; markdown and yaml default to `../../tinyactor`, reflecting their different directory depth. Override it on the command line when building/testing against another core checkout, e.g. `make test TINYACTOR=/tmp/tinyactor-copy` (sqlite, markdown, yaml).
* Use `$(CURDIR)` for paths to package-local smoke/test programs when the recipe changes directory into the core tree. All three package tests do this, avoiding a path that is accidentally interpreted relative to the new cwd. This matters because the runtime resolves the shared library relative to its cwd (§Build and discover).
* Provide `.PHONY: all clean test`; `all` builds the platform library, and `clean` removes that generated library (`sqlite`, `markdown`, `yaml`). `test` builds and runs a smoke program plus package tests with `--no-cache` in all three. Their installation differs: sqlite copies the glue/library into `$(TINYACTOR)/lib`, while markdown and yaml first copy the core tree into a fresh `/tmp` directory and install there. The markdown/yaml recipes remove that temporary copy on exit. These patterns are evidenced by the respective package Makefiles; they are not core-level behavior.

## Package development workflow (dogfooded)

Test against a disposable core copy under `/tmp`, not a developer's working checkout: point `TINYACTOR` at that copy. This is particularly important for sqlite, whose `test` target copies package files directly into `$(TINYACTOR)/lib`; markdown/yaml instead create their own temporary core copies inside `test`. The reports for sqlite, markdown, and yaml all record successful tests against isolated core copies.

As a development convention, pass `--no-cache` for package smoke and tests to exclude cache as a factor. This convention is based on the observable fact that the sqlite, markdown, and yaml package test recipes pass `--no-cache`; no stronger claim about its cache effects is made here.

Classify a smoke result in four states: (1) missing library / unexpected `nil` (the v1 lazy-loader failure described above), (2) expected value, (3) a non-nil but incorrect value, or (4) execution/compile failure. Check the value rather than treating a zero exit status as success: the sqlite, markdown, and yaml package reports include smoke checks and assertions, and sqlite specifically tests expected results and failures. This complements the `_template` end-to-end smoke reference above.

## Error signals and documentation status

Follow the signal vocabulary in [`stdlib-port-plan.md` §“错误处理约定”](stdlib-port-plan.md#贯穿约定v5-新增先于一切模块): `nil` means a suspended operation should be retried; `-1` means hard error. TA glue should lift these signals consistently. Do not treat `nil` as a generic error.

`docs/c-module.md` §1 still contains old error examples (including `-1` as a generic failure convention) that conflict with this vocabulary. Where the two documents disagree, the vocabulary in `stdlib-port-plan.md` is authoritative; the mismatch is already identified there for upstream correction.

## Open for v2

* Configurable package/source/library search path (for example `TA_PKG_PATH`). The sqlite/markdown/yaml dogfood confirms a concrete cwd mismatch risk for runtime libraries, but the tested workarounds are to run from the core root and use `$(CURDIR)` for package test paths, or test in an isolated core copy. A configurable path remains a v2 question; these package runs do not establish its design or claim that it is required.

* Version and dependency management, package registry, and reproducible install/update semantics.
* Whether the compiler's source lookup and VM's cwd-relative library lookup should be unified.

No behavior for these is implied by this v1 document.

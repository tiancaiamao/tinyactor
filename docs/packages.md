# TinyActor packages (v1)

TinyActor packages are third-party modules outside the core repository. The core/stdlib boundary is `lib`: as stated in [`stdlib-port-plan.md` §4](stdlib-port-plan.md), the C module mechanism is the package mechanism. A package is a self-contained directory with optional C source, TA glue/signatures, a Makefile, and tests.

## Repository layout

`tinyactor-pkgs/` is flat: each immediate child directory is one package (plus `_template/`). Each package owns its source, glue, build rules, and tests.

## Build and discover (v1 convention)

Clone `tinyactor-pkgs` and build the desired package. The consuming project declares the package as one line in its `tinyactor.deps` (a local directory source plus the tree name it should be reachable as), and `tinyactor vendor` materialises `<root>/vendor/example.org/thing -> <that directory>`:

```
example.org/thing    ../tinyactor-pkgs/thing
```

`vendor/` is generated, never committed; `tinyactor.deps` is the only thing that goes into git ([modules.md §10](modules.md)). Imports then go by that tree name (`import "example.org/thing:lib/thing"`); the shared library still has to be reachable by the cwd-relative runtime lookup described below, so run the program from the project root. **Caveat for packages that ship a C module**: its `external fn` declaration file cannot live under a third-party tree name — `qual_prefix` only keeps the prefix verbatim inside the distribution's own tree (`SELF_IDENTITY`) `lib/`, so `import "example.org/thing:lib/thing"` for a module with `external fn thing.*` fails with `[E0004] module 'thing~<hash>' has no export ...` (see [c-module.md](c-module.md) §1 and issue #425). A C package therefore has to be wired into the distribution tree's `lib/`: put its `<mod>.ta` declaration (or a link to it) at `<root>/vendor/tinyactor/lib/<mod>.ta` alongside the distribution's own declarations, and import it by that path; keep the dylib where the cwd-relative lookup finds it (`lib/<mod>.<ext>`). For an executable called `thing.op`, runtime auto-loading tries `lib/thing.dylib` on macOS or `lib/thing.so` on Linux relative to the process working directory, falling back to the same relative path next to the host executable if the cwd-relative lookup fails.

Import and runtime loading are separate:

* `import "<tree>:<path>"` resolves source by mechanical concatenation to `<root>/vendor/<tree>/<path>.ta`, where the root is the first ancestor of the program's entry file that holds a `vendor/` directory. One candidate, no search order: no bare-name form, no relative form, no second root (see [modules.md](modules.md) and [ta-language-spec.md](ta-language-spec.md) §import). It does not search the dylib and does not consult `TA_*_PATH`. This is the behavior of `find_root` / `module_file` in `lib/bootstrap/driver.ta`.
* Calling `thing.op` loads the C library lazily if the function is not already registered. The VM constructs `lib/thing.dylib` / `.so` and calls `dlopen`; this path is relative to the runtime's current working directory. When `TA_MOD_TAG` is defined at compile time, the VM constructs `lib/thing_<tag>.dylib` / `.so` instead (`find_cfunc_autoload`, `src/main.c`). **This source does not then fall back to the untagged filename**; it selects one filename at compile time. If the cwd-relative `dlopen` fails, the loader retries next to the host executable (`host_module_path` in `src/main.c`) before giving up — the error message still reports the cwd-relative path. No general package search path or C-module environment variable exists in this implementation.
* As an end-to-end check, run `make` in a package, add one line to the project's `tinyactor.deps` pointing at the package directory, run `tinyactor vendor`, copy its platform library into `lib/` (the path the runtime lookup below constructs), then run an importing `.ta` from the project root. See `_template` for a reproducible smoke test.

These path rules are intentionally a v1 convention, not a package manager. What exists today: `tinyactor vendor` materialises `vendor/` from `tinyactor.deps`, with three source kinds — a local directory (symlink, for in-tree development), `.` (the project itself), and a git remote (`git+<url>`, or a bare `github.com/<owner>/<repo>` tree name that doubles as the clone URL), which is cloned into `vendor/<tree>`. A third column pins a branch or tag; the commit it resolved to is recorded in `vendor/<tree>/.git/tinyactor-clone` (not a lock file — that, and commit-sha pinning, is #429). Compiler-side resolution knows only `<root>/vendor/<tree>/<path>.ta` and does not depend on cwd, but runtime dylib discovery is cwd-relative (with an executable-adjacent fallback); invoke the runtime from the project root.

## Troubleshooting: missing C library

The `.ta` glue/signature and the shared library are separate requirements. Importing with no `thing.ta` is an import-time error from the compiler (`driver.ta`'s module resolver returns no module). If `thing.ta` is present but the expected `.dylib`/`.so` is absent or at the wrong path, the lazy loader's `dlopen` fails and the call aborts the VM:

```
tavm: cannot load C module 'thing' from lib/thing.dylib: <dlerror detail>
tavm: build the module first (e.g. `make lib/thing.dylib`) or check the module name
```

exit code 1. The message names the module and the attempted path (since issue #281 it also prints the `dlerror()` text), so "the library is not installed" and "the module name is misspelled" are distinguishable on the spot. A separate link-time diagnostic, `tavm: unknown C function 'thing.op' (module 'thing')`, covers the opposite case: the module prefix is already registered (built-in or previously loaded) but the member name cannot be found — a misspelled or stale function name, not a missing library.

Three more distinct diagnostics cover the remaining lazy-load failure modes: a library that loads but exports no `vm_load_self` (not a TinyActor module), a library whose ABI version does not match the VM's (built against a different `ta.h`; issue #264 E3), and a library whose `vm_load_self` ran but does not provide the called function (stale build or misspelled name).

This is the behavior decided in issue #203 (and implemented for the lisp-kernel interpreter, `src/main.c`): a cfunc that resolves to nothing is an error, never a value. Before that, the call pushed `nil`, produced no runtime error, and exited 0 — and `nil` already means "suspend/retry" in TA, so the two were indistinguishable.

## C module ABI and build

A dynamic C module exports `void vm_load_self(VM *vm)`. The VM finds that exact symbol using `dlsym` and invokes it; the entry point registers functions through `vm_register_module(vm, "thing", funcs, count)`. Each `TaFunc` contains a function name, callback, and arity; qualified names are registered as `thing.function`. The callback type is `Val fn(VM *, Val *args, int nargs)`; return a TinyActor `Val` directly. The lazy call path searches for `vm_load_self` and then retries the qualified function lookup (`find_cfunc_autoload`, `src/main.c`). Sources: `src/api.c` (`vm_register_module`), `ta.h` (`TaFunc` and callback declarations). The existing `lib/demo.c` is a worked example.

The header also carries the module ABI version: `ta.h` defines `TA_ABI_VERSION` and emits a weak `int ta_abi_version(void)` probe, so any source that includes it exports the probe automatically (no separate build flag to remember). Before invoking `vm_load_self`, the loader reads the probe back with `dlsym` and, if it is missing (a module built before versioning) or differs from the VM's `TA_ABI_VERSION`, refuses to load the module — printing the module, the file and both versions (`find_cfunc_autoload`, `src/main.c`). A module built against a stale `ta.h` thus fails loudly at load time instead of corrupting NaN-boxed values at call time. Bump `TA_ABI_VERSION` on any change to the `Val` layout, the opcode set, or a module-visible `ta.h` struct, and rebuild every module.

To build a shared library, compile the module source as position-independent shared code and include the public header. The repository's demo rule is `$(CC) $(MOD_CFLAGS) -fPIC -shared $(UNDEF_OK) -o lib/demo.$(HTTP_EXT) lib/demo.c $(MOD_LDLIBS)` (`Makefile`, the `DEMO_MODS` target); `.dylib` is selected on macOS and `.so` elsewhere (`Makefile`, the `HTTP_EXT` assignment). On Linux the VM executable exports symbols for `dlopen` (`Makefile`, the `RDYNAMIC` / `-ldl` block); macOS uses `-undefined dynamic_lookup`. An independent package can follow `_template/Makefile` and adapt linker dependencies as needed.

## Package Makefile conventions (dogfooded)

The package Makefiles in `tinyactor-pkgs` provide three working examples; these are observed conventions, not requirements enforced by the core:

* Set `TINYACTOR ?= ...` to the core repository used for headers, build inputs, and (depending on the package) test installation. The sqlite package defaults to `../tinyactor`; markdown and yaml default to `../../tinyactor`, reflecting their different directory depth. Override it on the command line when building/testing against another core checkout, e.g. `make test TINYACTOR=/tmp/tinyactor-copy` (sqlite, markdown, yaml).
* Use `$(CURDIR)` for paths to package-local smoke/test programs when the recipe changes directory into the core tree. All three package tests do this, avoiding a path that is accidentally interpreted relative to the new cwd. This matters because the runtime resolves the shared library relative to its cwd (§Build and discover).
* Provide `.PHONY: all clean test`; `all` builds the platform library, and `clean` removes that generated library (`sqlite`, `markdown`, `yaml`). `test` builds and runs a smoke program plus package tests with `--no-cache` in all three. Their installation differs: sqlite copies the glue/library into `$(TINYACTOR)/lib`, while markdown and yaml first copy the core tree into a fresh `/tmp` directory and install there. The markdown/yaml recipes remove that temporary copy on exit. These patterns are evidenced by the respective package Makefiles; they are not core-level behavior.

## Package development workflow (dogfooded)

Test against a disposable core copy under `/tmp`, not a developer's working checkout: point `TINYACTOR` at that copy. This is particularly important for sqlite, whose `test` target copies package files directly into `$(TINYACTOR)/lib`; markdown/yaml instead create their own temporary core copies inside `test`. The reports for sqlite, markdown, and yaml all record successful tests against isolated core copies.

As a development convention, pass `--no-cache` for package smoke and tests to exclude cache as a factor. This convention is based on the observable fact that the sqlite, markdown, and yaml package test recipes pass `--no-cache`; no stronger claim about its cache effects is made here.

Classify a smoke result in four states: (1) missing library / unresolvable cfunc (an explicit `unknown C function` error and a non-zero exit, as described under *Troubleshooting*), (2) expected value, (3) a non-nil but incorrect value, or (4) execution/compile failure. Check the value rather than treating a zero exit status as success: the sqlite, markdown, and yaml package reports include smoke checks and assertions, and sqlite specifically tests expected results and failures. This complements the `_template` end-to-end smoke reference above.

## Red injection without shadowing (dogfooded)

Under the single-root contract there is nothing to shadow: a name has exactly one candidate, so the file an import opens is a pure function of the written name and the entry file's root. That retires the failure mode that bit the num-package eval/gen rework (batch 8) under the old importer-directory-first search — a package-local `foo.ta` silently winning over the copy under test, so that editing the copy changed nothing while `make test` stayed green. With one candidate, an edit to the resolved file always reaches the build.

Convention for red injection: inject into the file the import actually resolves to, `<root>/vendor/<tree>/<path>.ta`, following any symlink the tree name points at (see [modules.md](modules.md) §2). Editing any other copy of that source — the install tree instead of the project's `vendor/` tree, or vice versa — is not an injection. After injecting, verify the effect before drawing conclusions — confirm the suite actually fails, or diff the build output against the uninjected baseline.

## Error signals and documentation status

Follow the signal vocabulary in [`stdlib-port-plan.md` §“错误处理约定”](stdlib-port-plan.md#贯穿约定v5-新增先于一切模块): `nil` means a suspended operation should be retried; `-1` means hard error. TA glue should lift these signals consistently. Do not treat `nil` as a generic error.

`docs/c-module.md` §1 still contains old error examples (including `-1` as a generic failure convention) that conflict with this vocabulary. Where the two documents disagree, the vocabulary in `stdlib-port-plan.md` is authoritative; the mismatch is already identified there for upstream correction.

## Open for v2

* Configurable package/source/library search path (for example `TA_PKG_PATH`). The sqlite/markdown/yaml dogfood confirms a concrete cwd mismatch risk for runtime libraries, but the tested workarounds are to run from the core root and use `$(CURDIR)` for package test paths, or test in an isolated core copy. A configurable path remains a v2 question; these package runs do not establish its design or claim that it is required.

* Version and dependency management, package registry, and reproducible install/update semantics.
* Whether the compiler's source lookup and VM's cwd-relative library lookup should be unified.

No behavior for these is implied by this v1 document.

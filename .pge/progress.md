# Progress Tracker

## Status: Phase 1-6 COMPLETE, Phase 7 OPTIONAL

### Phase 1: codegen.lisp basic ✅
- Translated from codegen.ta to .lisp syntax
- Commit: 88c516f

### Phase 2: codegen.lisp full features ✅
- closure/and-or/let-multi/spawn-send-recv/tail-call/receive-scan
- 21/21 bytecode comparison tests pass
- Commit: 88c516f

### Phase 3: parser.ta pattern desugar ✅
- Pattern match desugar to if/let (no MATCH_* in output)
- Receive parsing to receive-scan form
- Commit: (in 88c516f)

### Phase 4: VM multi-module loading ✅
- Instruction length table for bytecode scanning
- rebase_code: adjusts jump addresses + fn_ids
- vm_append_module: appends code/fn_table/symbols
- vm_load_bytecode exposed as C function
- Commit: 62b6392

### Phase 5: bootstrap driver ✅
- driver.ta orchestrates tokenize → parse → compile → load → spawn
- --bootstrap mode in main.c
- .lisp import support in api.c
- Symbol dedup + PUSH_SYM rebasing
- ./tinyactor --bootstrap hello.ta works!
- Commit: a35169b

### Phase 6: self-hosting bootstrap ✅
- --bootstrap-emit mode compiles .ta → .tabc via Lisp pipeline
- Self-hosted bootstrap produces byte-identical output to C compiler
- Bootstrap chain stable (self-hosted compiles itself)
- codegen.lisp: fixed true/false emission
- Commit: ece13cf

### Phase 7: MATCH_* opcode removal (OPTIONAL, DEFERRED)
- MATCH_INT, MATCH_SYM, MATCH_NIL, MATCH_PAIR, MATCH_JUMP no longer emitted
- Can be safely removed from VM in a future cleanup
- Low priority — no functional impact

## Test Status
- 60/60 tests passing at every commit
- Self-hosting verified: bootstrap → compile driver.ta → identical bootstrap
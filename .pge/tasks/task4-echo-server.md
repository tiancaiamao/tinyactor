# Task 4: TCP Echo Server Example

Create a working TCP echo server that demonstrates TinyActor's actor concurrency with real network I/O.

## What to Build

### `example/echo_server.c`
C main program that:
1. Creates a TinyActor VM
2. Registers the net module (`vm_register_net_module`)
3. Loads and runs `echo_server.lisp`
4. Links against tinyactor object files (or compiles with -I and includes)

### `example/scripts/echo_server.lisp`
Script that:
1. Imports the net module: `(import "net")`
2. Starts a TCP server: `(define server (net.listen 8090))`
3. Defines a client handler actor that reads data and echoes it back:
```lisp
(define (handle-client conn)
  (let data (net.read conn 1024))
  (if (eq data 'eof)
      (begin (net.close conn) 'done)
      (begin (net.write conn data) (handle-client conn))))
```
4. Defines an accept loop that spawns a new actor per connection:
```lisp
(define (accept-loop server)
  (let conn (net.accept server))
  (spawn-handle-client conn)  ;; spawn actor per connection
  (accept-loop server))
```
5. Starts the accept loop

### `example/scripts/echo_test.lisp` (test client)
Script that:
1. Connects to the echo server (needs a connect function — may need to add `net.connect` to net module)
2. Sends test messages
3. Reads back responses
4. Verifies echo matches

### `example/Makefile`
Build system that compiles the echo_server example

## Important Notes

### You may need to add `net.connect` to the network module
If `net.connect` doesn't exist in src/net.c, add it:
- `net.connect(host_string, port)` — Create TCP connection to host:port
- Set non-blocking
- Return socket fd as Val int
- For simplicity, host can be "127.0.0.1" or "localhost"

### Integration Test
Create `test/scripts/echo_server_test.lisp` that:
1. Spawns the echo server as one process
2. Spawns N client actors (e.g., 3) that connect, send messages, receive echoes
3. Verifies each client gets back what it sent
4. Prints "PASS" at the end

OR: Create a C test program `example/test_echo.c` that:
1. Starts the echo server in a child process or thread
2. Connects via plain C sockets
3. Sends test data, verifies echo

### Verification
```bash
cd /Users/genius/project/tinyactor/example
make echo_server
./echo_server &
sleep 1
# Test with: echo "hello" | nc localhost 8090
echo "hello" | nc -w 2 localhost 8090
kill %1
```

The echo server must:
- Accept multiple concurrent connections (each handled by a separate actor)
- Echo data back correctly
- Not crash or hang under concurrent load

## Files to Create
1. `example/echo_server.c`
2. `example/scripts/echo_server.lisp`
3. `example/Makefile`
4. Possibly: `example/scripts/echo_test.lisp` or `example/test_echo.c`
5. Possibly: add net.connect to src/net.c

## Files to Modify
1. `src/net.c` — add net.connect if needed
2. `ta.h` — add net.connect declaration if needed
3. `src/main.c` — if net.connect needs registration

## Estimated
- ~150 lines new code
- ~20 lines modifications
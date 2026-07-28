# example/Makefile — Run TA examples with TinyActor VM
#
# Usage:
#   make echo-server    # Run echo server on port 8090
#   make http-server    # Run HTTP server on port 8080
#   make echo-test      # Run echo integration test
#   make concurrent     # Run concurrency test
#   make all            # Run all tests

TINYACTOR = ../tinyactor

all: echo-test concurrent

echo-server:
	$(TINYACTOR) scripts/echo_server.ta

http-server:
	$(TINYACTOR) scripts/http_server.ta

echo-test:
	@echo "=== Echo Integration Test ==="
	$(TINYACTOR) scripts/echo_test.ta
	@echo ""

concurrent:
	@echo "=== Concurrency Test ==="
	$(TINYACTOR) scripts/concurrent_test.ta
	@echo ""

.PHONY: all echo-server http-server echo-test concurrent
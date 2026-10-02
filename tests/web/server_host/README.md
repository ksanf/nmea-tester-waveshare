# HTTP output dispatcher regression

Run on Linux or WSL with GCC and its AddressSanitizer and UndefinedBehaviorSanitizer
runtimes, from the repository root:

```sh
sh tests/web/server_host/run.sh
```

The script resolves paths relative to itself, so it can also be invoked using
its full path from another working directory. It compiles in a temporary
directory and removes its output when finished. Set `CC` to choose another
compiler that supports the same sanitizer and linker options. `ASAN_OPTIONS`
can override the default `detect_leaks=1` setting.

The test compiles the production `main/web/web_server.c` with isolated
FreeRTOS/HTTPD substitutes and its real public header. It checks:

- bounded outgoing queues and copied payload lifetime;
- accounting for scheduled callbacks;
- close priority and retained connection state;
- logical connection identity after file-descriptor reuse;
- cleanup and retry behavior when platform API calls fail.

These deterministic substitutes do not model network behavior, ESP-IDF's
internal UDP control mailbox, startup failures, or concurrent FreeRTOS task
execution. The test does not connect to or modify a running tester. Hardware
checks remain necessary for the behaviors it does not simulate.

This suite is separate from `tests/host` and the Playwright browser tests. Its
headers are scoped to this test build and do not affect firmware compilation.
See the [main test instructions](../../../README.md#tests) for the other suites.

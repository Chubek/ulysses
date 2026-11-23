# Ulysses

Ulysses is an On1x foundation for a programmable web server and Weblet runtime.
The development profile runs Dublin on Linux loopback and serves two Weblets
in a supervised worker process. It uses the pinned MuLIB HTTP parser and an
On1x-generated C11 executable.

## Build and run

Initialize the pinned MuLIB submodule and build On1x with CMake 3.20+, a C11
compiler, a C++20 compiler, Python 3, and Perl. The fallback scanner suffices
for this profile.

```sh
git submodule update --init
cmake -S third_party/mulib/on1x -B third_party/mulib/build/on1x \
  -DON1X_USE_FALLBACK_SCANNER=ON -DON1X_BUILD_EXTENSION_SDK=OFF \
  -DON1X_BUILD_NATIVE_PPX=OFF -DBUILD_TESTING=OFF
cmake --build third_party/mulib/build/on1x --target on1xc --parallel
make test
```

For an existing compatible compiler, run `make test ON1XC=/absolute/path/to/on1xc`.
The Makefile links the Linux I/O adapter into Dublin and the worker. The root
`On1xfile` also builds the portable `ulysses-check`, `bloomc`, and `bloomrun` targets.

From the repository root:

```sh
./build/dublind
```

In a second terminal:

```sh
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/api/hello
curl http://127.0.0.1:8080/api/bloom
printf 'text Hello\ntext  world\nconcat\nreturn\n' | ./build/bloomc | ./build/bloomrun
```

| Setting | Default | Meaning |
| --- | --- | --- |
| `ULYSSES_PORT` | `8080` | Loopback port. Zero selects an ephemeral port, printed at startup. |
| `ULYSSES_WORKER` | `./build/telemachaid-worker` | Operator-selected trusted executable path. |
| `ULYSSES_MAX_CONNECTIONS` | `256` | Process lifetime budget of 1–1024 accepted connections. Dublin exits cleanly at the limit. |

## Subsystems

| Subsystem | Implemented base |
| --- | --- |
| Dublin | HTTP/1.1 request admission, trusted route dispatch, bounded socket reads and writes, and status responses. |
| Dedalus | In-process On1x handler selection for a literal homepage, liveness, and Weblet routes. |
| Rubicon | Versioned messages with generation and request identity, over bounded Unix socket records. |
| Telemachaid | A separate guest process, readiness handshake, stale request rejection, and host-controlled termination. |
| Erin | An On1x Weblet behind the guest message contract. |
| Bloomscript | A compiler and bounded interpreter for a string stack language. Both CLI tools accept stdin and write stdout. |
| Introibo | In-process single-worker reconciliation, 30-second lease expiry, generation replacement, and capped restart backoff. |

Dublin accepts one bodyless request per connection and serializes exchanges.
Request headers are capped at 8192 bytes and 32 fields. Each transport phase
has a two-second deadline. Dublin starts its worker on demand, admits requests
after the readiness probe, and renews the lease after a valid reply. A failed
worker exchange returns `502`, a worker that cannot be started or is backing
off returns `503`, and `/health` is independent of worker readiness.
The lifetime connection limit bounds cumulative On1x string allocations.

Rubicon records use a four-byte unsigned big-endian length, followed by
`U1\nKIND\nGENERATION\nREQUEST\nPAYLOAD`. Integers are canonical decimal,
generation and request IDs are positive and at most 2³¹−1, and the NUL-free
payload is limited to 4096 bytes. Kinds 1, 2, and 3 are the readiness probe,
Erin request, and Bloomscript request, respectively. Their payloads are empty.
Kinds 4 and 5 are success and failure replies. The worker accepts a readiness
probe with request ID 1, followed by strictly increasing request IDs in that
generation. The parent checks every reply against the outstanding exchange.

Bloomscript source accepts `text <bytes>`, `concat`, and `return`, one
instruction per LF-terminated line. Bytecode starts with `B1\n` and uses
`T<bytes>`, `C`, and `R`. A program must return exactly one string at the
end. Both compiler and interpreter bound the input, instruction count, stack
depth, and output. The worker runs the bundled example through this same VM.

The profile is for local development with trusted workers. Public deployment,
OS sandbox policy, streaming and multiplexing, dynamic Dedalus ABI loading,
an independent Introibo daemon with durable state, and HTTP/2 and HTTP/3 remain
future milestones.

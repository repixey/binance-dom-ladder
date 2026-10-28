# DOM ladder - Binance USD-M Futures

Minimal C++17 order-book ladder (Dear ImGui + GLFW + OpenGL3) with subtle large-volume-cluster highlighting.

## Build
Needs: C++17 compiler, CMake >= 3.20, Boost >= 1.75 (headers), OpenSSL, OpenGL. GLFW 3.4 and ImGui 1.91.9 are fetched.

    # Debian/Ubuntu:  sudo apt install build-essential cmake libboost-dev libssl-dev xorg-dev libgl1-mesa-dev
    # macOS:          brew install cmake boost openssl
    # Windows:        vcpkg install boost-beast boost-asio openssl   (then use the vcpkg toolchain file)
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ./build/dom
    ctest --test-dir build        # parser / book / sync tests

## Data flow
* Diff depth: `wss://fstream.binance.com/public/ws/<symbol>@depth[@100ms|@500ms]`
  (Binance split USD-M streams into `/public` `/market` `/private`; legacy URLs were retired 2026-04-23.)
* Snapshot: WebSocket API `wss://ws-fapi.binance.com/ws-fapi/v1`, method `depth`, limit 1000.
* Sync follows the official futures procedure (buffer, snapshot, `U <= lastUpdateId <= u`, `pu == previous u`, resync on gap).

## Controls
Symbol box + Enter/Go, update speed, price grouping (xN ticks), `auto` follow / `Center` (or `C`), mouse wheel scrolls,
`cl>Nx` slider = cluster threshold, hover a row for its size vs. the median level.

## Low-latency notes
* One network thread, single-threaded `io_context{1}`, `TCP_NODELAY`, zero-allocation JSON scan, fixed-point prices.
* Sorted flat vectors with the best level at the back, so top-of-book updates only touch the tail.
* Wait-free triple buffer to the UI; the network thread wakes the UI via `glfwPostEmptyEvent`; vsync off by default.
* Footer shows `recv>draw` (socket read to draw calls submitted), parse+apply time, and `feed ~` (local clock minus exchange event time, so it includes clock skew).
* Further options: pin the net thread, `SCHED_FIFO`, busy-poll sockets, `@0ms`-style real-time depth if your account/endpoint offers it.

## Cluster highlighting
Per side, the median size of a price bucket over the published depth (1024 levels) is the baseline. Rows at or above
`thr x median` get a brighter bar and text (smoothstep up to 2.5x the threshold) and a thin accent strip on the outer
edge, bridged across one-row gaps so adjacent heavy levels read as one cluster.

## Notes
Not compiled against real Boost/ImGui in the authoring sandbox (no network): `core.hpp` is unit-tested; `feed.hpp` and `main.cpp` are written against the documented APIs.
Market data only; no orders or keys. On Windows, if TLS fails to verify, point OpenSSL at a CA bundle (`SSL_CERT_FILE`).

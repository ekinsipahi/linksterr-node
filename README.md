# linksterr-node

Share your PC's idle internet connection and earn on [Linksterr](https://linksterr.com).
A single Windows console program, no installer, no dependencies, no UI: start it
with the node key from your Linksterr dashboard and leave it running.

```
linksterr-node.exe --key lnn_xxxxxxxx
```

The first run shows what the program does and asks you to type `I AGREE`.
Unattended use (a batch file, Task Scheduler) passes `--accept-terms` after you
have read the [terms and privacy notice](https://linksterr.com/node-terms/).

## What it does

With your permission the PC carries other people's ordinary web requests while
your connection is idle, as an exit point of the Linksterr network (operated
with Proxysterr). Websites reached that way see your connection's IP address.

* Only ordinary web ports are relayed (80, 443, 8080, 8443); addresses on your
  own network are never reachable through the PC.
* Nothing on the PC is read, stored or sent; only relayed byte counts and the
  connection's IP, country and network operator are recorded.
* **One internet connection earns once.** A second PC behind the same router is
  refused with a retry hint and takes over when the first one leaves.
* You earn a fixed amount per connection per day, shown in your dashboard, for
  days the connection actually relayed traffic; the amount is scaled by the
  time the PC was online.
* Stop any time with Ctrl+C or by closing the window. `--withdraw` deletes the
  acceptance and this PC's device identity.

## Usage

```
linksterr-node.exe --key KEY [--label NAME] [options]
linksterr-node.exe --config node.ini [options]

  --key KEY          node key from your Linksterr dashboard (repeat for more accounts)
  --label NAME       a name for the last --key, shown in the log only
  --config FILE      read settings and [account] sections from an INI file
  --accept-terms     record that you accept the terms (needed once; unattended use)
  --withdraw         delete your acceptance and this PC's device identities, then exit
  --max-streams N    concurrent connections to carry (1-64, default 16)
  --server URL       coordinator (default wss://node.ipsterr.com)
  --data-dir DIR     where consent and device identities are kept
                     (default %LOCALAPPDATA%\LinksterrNode)
  --verbose          debug logging
```

`LINKSTERR_NODE_KEY` in the environment is used when no `--key` is given, so a
batch file can be as short as:

```bat
set LINKSTERR_NODE_KEY=lnn_xxxxxxxx
linksterr-node.exe --accept-terms
```

Several accounts in one process: repeat `--key`, or use a config file
(see [`node.example.ini`](node.example.ini)). Each account gets its own tunnel;
the one-connection-earns-once rule still applies per public IP.

Exit codes: `0` stopped by you · `2` bad arguments · `3` every account refused
(check the key) · `4` terms not accepted.

## Building

Visual Studio 2022+ (or Build Tools) with the C++ workload:

```
build.bat            → dist\linksterr-node.exe
```

or with CMake (`cmake -B build && cmake --build build --config Release`). The
exe links only `winhttp.lib` and `ws2_32.lib` and uses the static CRT, so it
runs on any Windows 10/11 machine without a redistributable.

## How it talks to the coordinator

Same wire protocol as the Android SDK (`proxy-pool-sdk-android/docs/protocol.md`):
one WebSocket, JSON text frames for control (`hello` → `welcome` | `error`,
`stats`, `ping`, `bye`) and binary frames `[1B op][4B stream id][payload]` for
the relay (OPEN/DATA/CLOSE/OPEN_OK/OPEN_ERR). The hello carries
`"pool": "linksterr"` and the node key as `"account"`; the coordinator asks
Linksterr whether the key is valid and answers with the account's masked label.
Identity is the coordinator's token, stored per key under the data directory.

Source layout: `src/ws.*` WinHTTP WebSocket · `src/net.*` outbound TCP and the
private-address gate · `src/tunnel.*` handshake, relay, keep-alive, reconnect ·
`src/main.cpp` CLI, config file, consent · `src/json.*` the little JSON needed.

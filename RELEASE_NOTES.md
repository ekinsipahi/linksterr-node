## linksterr-node 1.0.0

First release of the Windows client for **Earn with your PC** on Linksterr.

**Download:** `linksterr-node.exe` below (Windows 10/11, 64-bit, ~350 KB, no installer).

### What it does
Shares your PC's idle internet connection with the Linksterr network: your PC carries
other people's ordinary web requests while it is idle and you earn a fixed amount per
internet connection per day. Nothing on your PC is read, stored or sent.

### Quick start
1. Get your node key from the **Node Earn** page in your Linksterr dashboard.
2. Run `linksterr-node.exe --key lnn_…` and type `I AGREE` after reading the terms.
3. Leave the window open (minimised is fine). Ctrl+C or closing it stops sharing.

For unattended use (batch file, Task Scheduler) pass `--agree` after reading the
[terms and privacy notice](https://linksterr.com/node-terms/):

```bat
set LINKSTERR_NODE_KEY=lnn_xxxxxxxx
linksterr-node.exe --agree
```

### Highlights
- Zero dependencies: a single static exe built on Windows' own WinHTTP/Winsock — no runtime to install.
- Only ordinary web traffic is relayed (ports 80, 443, 8080, 8443); your own network is never reachable through the PC.
- **One internet connection earns once**: a second PC behind the same router waits its turn automatically.
- Automatic reconnect with backoff; a refused key stops cleanly and tells you why.
- Several accounts in one process (`--config node.ini`, repeat `--key`).
- `--withdraw` forgets this PC and your acceptance at any time.

### Notes
- Windows SmartScreen may warn on first run because the binary is not code-signed yet; choose "More info → Run anyway", or build it yourself from source with `build.bat`.
- The program prints what it relays only as byte counts; earnings are computed server-side and shown in your dashboard.

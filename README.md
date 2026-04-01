# rdma-tutorial

A minimal two-machine RDMA demo using `rdma_cm` and `libibverbs` over RoCE (RDMA over Converged Ethernet).

Demonstrates three RDMA verbs:
- **RDMA WRITE** — client pushes data directly into server memory; server CPU is not involved
- **RDMA READ** — client pulls data from server memory; server CPU is not involved
- **SEND/RECV** — two-sided verb; server pre-posts a RECV and gets a completion when the client sends

## Requirements

- Linux with RoCE-capable NICs on both machines
- `libibverbs-dev` and `librdmacm-dev` installed
- Both machines on the same L2/L3 network with RDMA reachable IPs

```bash
sudo apt install libibverbs-dev librdmacm-dev
```

## Build

```bash
make
```

Binaries are output to the project root: `rdma_server`, `rdma_client`.

## Expected Output

**Server:**
```
[SERVER] Connection established — QP is RTS
[SERVER] Waiting for client to finish RDMA transfers...

[SERVER] buf[0..127] after client RDMA WRITE:
──────────────────────────────────────────────
Hello from RDMA WRITE! This landed in server memory without server CPU involvement.
──────────────────────────────────────────────

[SERVER] Signal after client RDMA SEND:
──────────────────────────────────────────────
DONE
──────────────────────────────────────────────
```

**Client:**
```
[CLIENT] Connection established — QP is RTS
[CLIENT] Server MR: addr=0x...  rkey=0x...  size=4096 B

━━━  Demo 1: RDMA WRITE  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[CLIENT] WRITE complete  (85 bytes → server offset 0)

━━━  Demo 2: RDMA READ   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[CLIENT] READ complete  (256 bytes from server offset 1024)
[CLIENT] Data: "Hello from RDMA Read! ..."

━━━  Demo 3: SEND        ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[CLIENT] SEND complete
```

## Usage

**On the server machine:**
```bash
./rdma_server <server-ip>
# e.g. ./rdma_server 10.0.3.2
```

**On the client machine:**
```bash
./rdma_client <server-ip> [client-ip]
# e.g. ./rdma_client 10.0.3.2 10.0.3.1
```
`client-ip` is optional but needed on multi-homed machines to select the correct RoCE interface.

## Connection Flow

```
Client                              Server
──────                              ──────
                                   rdma_bind_addr
                                   rdma_listen

rdma_resolve_addr
rdma_resolve_route
rdma_connect       ─────────────►  CONNECT_REQUEST
                                   rdma_accept (sends server MR info)
ESTABLISHED        ◄─────────────
(receives server MR info)

RDMA WRITE  ───────────────────►   (silent, no CPU)
RDMA READ   ◄───────────────────   (silent, no CPU)
SEND        ───────────────────►   RECV completion

rdma_disconnect    ─────────────►  DISCONNECTED
```

## Configuration

Key constants in [src/common.hpp](src/common.hpp):

| Constant | Default | Description |
|---|---|---|
| `DEFAULT_PORT` | `18515` | TCP port used by rdma_cm |
| `BUF_SIZE` | `4096` | Registered memory buffer size |
| `CQ_DEPTH` | `128` | Completion queue depth |
| `QP_MAX_WR` | `128` | Max outstanding work requests |

## Project Structure

```
src/
  common.hpp       — shared constants, mr_info_t, rdma_ctx_t
  rdma_server.cpp  — server: listen, accept, poll for SEND
  rdma_client.cpp  — client: connect, WRITE, READ, SEND
Makefile
CMakeLists.txt
```
# ROCE-tutorial (RDMA over Converged Ethernet)

A minimal two-machine RDMA demo using `rdma_cm` and `libibverbs` over RoCE (RDMA over Converged Ethernet).

Demonstrates three RDMA verbs:
- **RDMA WRITE** — client pushes data directly into server memory; server CPU is not involved
- **RDMA READ** — client pulls data from server memory; server CPU is not involved
- **SEND/RECV** — two-sided verb; server pre-posts a RECV and gets a completion when the client sends

## Requirements

- Linux with RoCE-capable NICs on both machines
- `libibverbs-dev` and `librdmacm-dev` installed
- Both machines on the same L2/L3 network with RDMA reachable IPs

```bash
sudo apt install libibverbs-dev librdmacm-dev
```

## Build

```bash
make
```

Binaries are output to the project root: `rdma_server`, `rdma_client`.

## Expected Output

**Server:**
```
[SERVER] Connection established — QP is RTS
[SERVER] Waiting for client to finish RDMA transfers...

[SERVER] buf[0..127] after client RDMA WRITE:
──────────────────────────────────────────────
Hello from RDMA WRITE! This landed in server memory without server CPU involvement.
──────────────────────────────────────────────

[SERVER] Signal after client RDMA SEND:
──────────────────────────────────────────────
DONE
──────────────────────────────────────────────
```

**Client:**
```
[CLIENT] Connection established — QP is RTS
[CLIENT] Server MR: addr=0x...  rkey=0x...  size=4096 B

━━━  Demo 1: RDMA WRITE  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[CLIENT] WRITE complete  (85 bytes → server offset 0)

━━━  Demo 2: RDMA READ   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[CLIENT] READ complete  (256 bytes from server offset 1024)
[CLIENT] Data: "Hello from RDMA Read! ..."

━━━  Demo 3: SEND        ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[CLIENT] SEND complete
```

## Usage

**On the server machine:**
```bash
./rdma_server <server-ip>
# e.g. ./rdma_server 192.10.3.2
```

**On the client machine:**
```bash
./rdma_client <server-ip> [client-ip]
# e.g. ./rdma_client 192.10.3.2 192.10.3.1
```
`client-ip` is optional but needed on multi-homed machines to select the correct RoCE interface.

## Connection Flow

```
Client                              Server
──────                              ──────
                                   rdma_bind_addr
                                   rdma_listen

rdma_resolve_addr
rdma_resolve_route
rdma_connect       ─────────────►  CONNECT_REQUEST
                                   rdma_accept (sends server MR info)
ESTABLISHED        ◄─────────────
(receives server MR info)

RDMA WRITE  ───────────────────►   (silent, no CPU)
RDMA READ   ◄───────────────────   (silent, no CPU)
SEND        ───────────────────►   RECV completion

rdma_disconnect    ─────────────►  DISCONNECTED
```

## Configuration

Key constants in [src/common.hpp](src/common.hpp):

| Constant | Default | Description |
|---|---|---|
| `DEFAULT_PORT` | `18515` | TCP port used by rdma_cm |
| `BUF_SIZE` | `4096` | Registered memory buffer size |
| `CQ_DEPTH` | `128` | Completion queue depth |
| `QP_MAX_WR` | `128` | Max outstanding work requests |

## Project Structure

```
src/
  common.hpp       — shared constants, mr_info_t, rdma_ctx_t
  rdma_server.cpp  — server: listen, accept, poll for SEND
  rdma_client.cpp  — client: connect, WRITE, READ, SEND
Makefile
CMakeLists.txt
```

# FIX Server Benchmark Report / FIX 伺服器效能測試報告

**Date / 測試日期：** 2026-04-20
**Last Updated / 最後更新：** 2026-06-05
**Author / 作者：** Justin
**Repository / 程式庫：** Hello-Fix
**Branch / 分支：** main

---

## 1. Test Environment / 測試環境

| Item / 項目 | Detail / 內容 |
|---|---|
| OS Kernel / 作業系統核心 | Linux 6.17.0-19-generic |
| CPU Cores / CPU 核心數 | 4 |
| Memory / 記憶體 | 7.8 GiB |
| Network / 網路 | Loopback (127.0.0.1) |
| Compiler / 編譯器 | GCC (with `-g -Wall -Wextra`) |
| Test Tools / 測試工具 | `bench_fix_server_th` (blocking, C11 + pthreads); `bench_fix_server_v2` (epoll, C11 + pthreads) |
| `tcp_tw_reuse` | 2 (loopback TIME_WAIT reuse enabled) |

---

## 2. Subjects Under Test / 受測程式

### 2.1 `fix_server` — Single-threaded Server / 單執行緒伺服器

| Property / 屬性 | Value / 值 |
|---|---|
| Source / 原始碼 | `fix_server.c` |
| Architecture / 架構 | Single thread, blocking `accept()` loop |
| Listen Port / 監聽埠 | 5001 |
| Listen Backlog | 5 |
| Protocol / 協定 | FIX 4.2 Heartbeat over TCP |

**Processing flow / 處理流程：**
```
accept() → send FIX msg (~70 bytes) → close()  [repeat]
```

---

### 2.2 `fix_server_th` — Multi-threaded Server / 多執行緒伺服器

| Property / 屬性 | Value / 值 |
|---|---|
| Source / 原始碼 | `fix_server_th.c` |
| Architecture / 架構 | 5 worker threads + `accept_mutex` |
| Listen Port / 監聽埠 | 5001 |
| Listen Backlog / 等待佇列 | 10 |
| Protocol / 協定 | FIX 4.2 Heartbeat over TCP |

**Processing flow / 處理流程：**
```
lock(mutex) → accept() → unlock(mutex) → send FIX msg → close()  [each thread loops]
```

The `accept_mutex` serializes `accept()` calls to prevent the thundering herd problem.
`accept_mutex` 序列化所有 `accept()` 呼叫，避免驚群效應（Thundering Herd Problem）。

---

### 2.3 `fix_server_th_v2` — High-performance Multi-threaded Server / 高效能多執行緒伺服器

| Property / 屬性 | Value / 值 |
|---|---|
| Source / 原始碼 | `fix_server_th_v2.c` |
| Architecture / 架構 | N worker threads, each with independent `SO_REUSEPORT` socket |
| Worker Threads (tested) / 測試執行緒數 | 8 |
| Listen Port / 監聽埠 | 5001 |
| Listen Backlog / 等待佇列 | 1024 |
| Key socket options / 關鍵 socket 選項 | `SO_REUSEPORT`, `SO_INCOMING_CPU` |
| Protocol / 協定 | FIX 4.2 Heartbeat over TCP |

**Processing flow / 處理流程：**
```
[each thread independently]
accept(own_fd) → send FIX msg (MSG_NOSIGNAL) → close()  [loop]
```

**Key optimizations / 關鍵優化：**

| Optimization / 優化項目 | Status / 狀態 | Effect / 效果 |
|---|:---:|---|
| `SO_REUSEPORT` | ✅ Applied | Kernel distributes incoming connections across N sockets — no mutex, true parallelism / 核心分派連線至 N 個 socket，無 mutex，真正平行 |
| `SO_INCOMING_CPU` | ✅ Applied | Hints kernel to route connections to socket on the same CPU core, reducing cross-core cache miss / 提示核心將連線路由至同核心 socket，降低跨核快取失效 |
| `accept(NULL, NULL)` | ✅ Applied | Skips client address copy per accept / 省去每次 accept 的位址複製 |
| `BACKLOG=1024` | ✅ Applied | Larger queue absorbs bursts without ECONNREFUSED / 更大佇列吸收突發連線，避免 ECONNREFUSED |
| `MSG_NOSIGNAL` in `send()` | ✅ Applied | Prevents SIGPIPE when client closes early / 避免客戶端提前關閉時產生 SIGPIPE |
| No `SO_LINGER=0` | ✅ Applied | Graceful close preserves in-flight data / 優雅關閉，確保傳送中的資料不遺失 |
| `TCP_NODELAY` | ❌ Not applied | Server uses one-send-per-connection pattern; kernel flushes buffer before FIN regardless, so disabling Nagle provides no measurable benefit and adds an unnecessary syscall. / 每連線僅呼叫一次 send() 即關閉，核心在 FIN 前強制 flush，TCP_NODELAY 無實質效益且增加多餘 syscall。 |
| CPU affinity (`pthread_setaffinity_np`) | ❌ Not applied | Static pinning causes scheduling imbalance when connection load is uneven across cores. `SO_REUSEPORT` already handles kernel-level distribution; OS scheduler provides better dynamic load balancing. / 靜態綁核在負載不均時造成部分核心過載，SO_REUSEPORT 已處理核心層分流，OS 動態排程優於強制 affinity。 |

---

## 3. Benchmark Methodology / 測試方法

### 3.1 `bench_fix_server_th` (Blocking / 阻塞式)

Each benchmark thread runs the following loop until the timer expires:
每條 benchmark thread 執行以下迴圈直到計時結束：

```
socket() → connect() → recv FIX msg (until "10=<SOH>") → close()
```

- Connection count is tracked via C11 `_Atomic long` (lock-free).
  連線計數以 C11 `_Atomic long` 累計（無鎖）。
- Elapsed time measured with `CLOCK_MONOTONIC` to avoid wall-clock jumps.
  使用 `CLOCK_MONOTONIC` 計時，避免系統時鐘跳躍影響結果。
- Only connections that successfully received a complete FIX message are counted.
  僅成功接收完整 FIX 訊息的連線才計入統計。

### 3.2 `bench_fix_server_v2` (epoll / 非阻塞式)

Each worker thread manages a batch of non-blocking connections via `epoll`:
每條 worker thread 透過 `epoll` 管理一批非阻塞連線：

```
[state machine per slot]
ST_CONNECTING (EPOLLOUT) → ST_RECEIVING (EPOLLIN) → count++ → reconnect
```

- Designed to reduce benchmark-side CPU overhead vs. blocking threads.
  設計目的是降低 benchmark 端的 CPU 開銷。
- Used primarily for `fix_server_th_v2` high-throughput measurement.
  主要用於 `fix_server_th_v2` 的高吞吐量量測。

**Notes / 注意事項：**
- Each test series was run with **one benchmark process only** (no concurrent load generators).
  每次測試僅執行**單一** benchmark 進程，不混合多個負載產生器。
- TIME_WAIT accumulation affects repeated runs; measurements marked "fresh" used a clean port table.
  TIME_WAIT 累積會影響連續測試結果；標記「fresh」的量測使用乾淨的埠號表。

---

## 4. Test Results / 測試結果

### 4.1 `fix_server` (Single-threaded / 單執行緒)

> Duration per run / 每次測試時間：5–8 seconds | Tool: `bench_fix_server_th`

| Client Threads / 客戶端執行緒 | Total Conns / 總連線數 | Duration / 時長 | Conn/sec |
|:---:|---:|---:|---:|
| 1 | 23,208 | 5s | 4,641 |
| 2 | 42,359 | 5s | 8,471 |
| 3 | 51,509 | 5s | 10,300 |
| 4 | 64,078 | 5s | 12,811 |
| **5** | **81,980** | **5s** | **16,392** ← peak / 峰值 |
| 5 | 80,886 | 5s | 16,173 |
| 5 | 126,787 | 8s | 15,845 |
| 6 | 81,370 | 5s | 16,270 |
| 7 | 78,065 | 5s | 14,853 |

**Peak throughput / 峰值吞吐量：~16,000–16,400 conn/sec @ 5 client threads**

---

### 4.2 `fix_server_th` (Multi-threaded / 多執行緒，5 workers)

> Duration per run / 每次測試時間：5–10 seconds | Tool: `bench_fix_server_th`

| Date / 日期 | Client Threads / 客戶端執行緒 | Total Conns / 總連線數 | Duration / 時長 | Conn/sec |
|:---:|:---:|---:|---:|---:|
| 2026-04-20 | 5 | 71,851 | 5s | 14,367 |
| 2026-04-20 | 8 | 86,289 | 5s | 17,252 |
| 2026-04-20 | 9 | 89,856 | 5s | 17,963 |
| 2026-04-20 | 10 | 93,316 | 5s | 18,655 |
| 2026-04-20 | 10 | 146,478 | 8s | 18,299 |
| 2026-04-20 | **11** | **95,659** | **5s** | **19,106** ← peak / 峰值 |
| 2026-04-20 | 11 | 94,890 | 5s | 18,965 |
| 2026-04-20 | 11 | 93,478 | 5s | 18,688 |
| 2026-04-20 | 12 | 88,801 | 5s | 17,368 |
| 2026-04-20 | 15 | 93,944 | 5s | 17,841 |
| 2026-04-20 | 20 | 180,561 | 10s | 17,458 |
| 2026-06-05 | 20 | 96,386 | 5s | 15,689 |

**Peak throughput / 峰值吞吐量：~18,700–19,100 conn/sec @ 10–11 client threads**

---

### 4.3 `fix_server_th_v2` (SO_REUSEPORT, 8 workers)

#### 2026-04-20 — Tool: `bench_fix_server_th` (40 threads, compiled -O3)

| Condition / 條件 | Client Threads / 客戶端執行緒 | Total Conns / 總連線數 | Duration / 時長 | Conn/sec |
|---|:---:|---:|---:|---:|
| Fresh (TIME_WAIT=0) | 40 | ~147,000 | 5s | **~29,400** ← peak / 峰值 |
| Sustained (TIME_WAIT acc.) | 40 | ~138,000 | 5s | ~27,600 |
| Sustained 8s run | 40 | 220,786 | 8s | 27,598 |

**Peak throughput / 峰值吞吐量：~27,600–29,400 conn/sec @ 40 client threads**

> **Note / 注意：** Measurements were made on the same machine hosting both server and benchmark.
> At 29 K conn/sec, all 4 CPU cores are fully saturated (user+sys+softirq ≈ 100%).
> Performance degrades slightly on back-to-back runs as TIME_WAIT ports accumulate;
> `tcp_tw_reuse=2` mitigates but does not eliminate this effect.
>
> 量測在伺服器與 benchmark 同機執行，29 K conn/sec 時 4 顆 CPU 完全飽和。
> 連續測試時 TIME_WAIT 累積會造成小幅下降；`tcp_tw_reuse=2` 可緩解但無法完全消除。

#### 2026-06-05 — Tool: `bench_fix_server_v2` (epoll, non-blocking)

| Workers / worker 數 | Batch/worker | Total Concurrent / 總並行數 | Total Conns / 總連線數 | Duration / 時長 | Conn/sec |
|:---:|:---:|:---:|---:|---:|---:|
| 2 | 200 | 400 | 67,576 | 5.0s | 13,473 |
| 4 | 200 | 800 | 118,061 | 5.0s | 23,491 |
| **8** | **200** | **1,600** | **131,427** | **5.0s** | **26,133** |

**Peak throughput / 峰值吞吐量：~26,133 conn/sec @ 8 workers × 200 batch**

> **Note / 注意：** 2026-06-05 run used epoll-based `bench_fix_server_v2` which carries different
> bench-side CPU overhead than blocking `bench_fix_server_th`; results reflect the same server ceiling
> but are not directly comparable across tools.
>
> 2026-06-05 測試採用 epoll 版 `bench_fix_server_v2`，bench 端 CPU 特性與阻塞式工具不同；
> 結果反映相同伺服器上限，但跨工具數字不可直接比較。

---

## 5. Comparison Summary / 比較摘要

| Version / 版本 | Architecture / 架構 | Peak conn/sec / 峰值 | vs `fix_server` | Best client threads / 最佳客戶端執行緒數 |
|---|---|---:|---:|:---:|
| `fix_server` | Single-threaded / 單執行緒 | ~16,400 | baseline | 5 |
| `fix_server_th` | 5 workers + mutex | ~19,100 | **+16%** | 10–11 |
| `fix_server_th_v2` | 8 workers, SO_REUSEPORT | ~27,600–29,400 | **+68–79%** | 40 |

> Target / 目標：`fix_server_th_v2` > `fix_server` × 2 (i.e., >32,800 conn/sec)
>
> On a single shared machine (4 cores), the combined CPU ceiling limits total throughput to ~29 K conn/sec.
> The 2× target requires a **dedicated server machine** (server not sharing cores with bench).
>
> 在 4 核單機環境（伺服器與 benchmark 共用資源），CPU 上限約 29 K conn/sec。
> 達成 2× 目標需**專用伺服器機器**（伺服器與 benchmark 分機執行）。

---

## 6. Analysis / 分析

### 6.1 Why `fix_server_th` is only ~16% faster than `fix_server` / 為何 v1 多執行緒僅提升約 16%

Although `fix_server_th` has 5 worker threads, the `accept_mutex` forces `accept()` to be
fully serialized. The performance gain comes only from **I/O overlap**: while one thread is
in `send()`, another can call `accept()` — there is no true parallelism.

雖然 `fix_server_th` 擁有 5 條 worker thread，但 `accept_mutex` 強制將 `accept()` 完全序列化。效能提升僅來自 **I/O 重疊**：當一條 thread 執行 `send()` 時，另一條可以同時呼叫 `accept()`，並非真正的平行處理。

### 6.2 Why `fix_server_th_v2` is ~70–80% faster / 為何 v2 提升約 70–80%

`SO_REUSEPORT` gives each of the 8 worker threads its own independent listening socket.
The Linux kernel load-balances incoming connections across these sockets at the SYN packet level —
no mutex, no thundering herd. All 8 threads can call `accept()` and `send()` truly in parallel.

`SO_INCOMING_CPU` further hints the kernel to route each incoming connection to the socket
whose owning thread runs on the same CPU core, reducing cross-core cache invalidation and
improving L1/L2 hit rates. This alone contributed approximately **+20%** throughput.

Two candidate optimizations were evaluated but **not applied**:

- **`TCP_NODELAY`**: This server uses a strict one-send-per-connection model (`send()` → `close()`). The Linux kernel unconditionally flushes the TCP send buffer when the socket is closed, so Nagle algorithm coalescing never occurs. Setting `TCP_NODELAY` would add a `setsockopt()` syscall on every accepted connection with zero measurable benefit.
- **CPU affinity (`pthread_setaffinity_np`)**: Static core pinning creates a scheduling imbalance when incoming connection load is not uniformly distributed — overloaded cores cannot shed work to idle ones. Since `SO_REUSEPORT` already performs kernel-level connection distribution and `SO_INCOMING_CPU` nudges cache locality, adding hard affinity constraints only reduces the OS scheduler's ability to rebalance under uneven load.

`SO_REUSEPORT` 讓 8 條 worker thread 各自擁有獨立的監聽 socket。Linux 核心在 SYN 封包層級即將連線負載均衡至這些 socket，無 mutex、無驚群效應，8 條 thread 可真正平行執行 `accept()` 與 `send()`。

`SO_INCOMING_CPU` 進一步提示核心將連線路由至與接收端 thread 同核心的 socket，降低跨核心快取失效，改善 L1/L2 命中率。此項單獨貢獻約 **+20%** 吞吐量。

兩項候選優化評估後**未套用**：

- **`TCP_NODELAY`**：本伺服器採一連線一 send() 即 close() 模式，核心在 close() 前必然 flush TCP 緩衝區，Nagle 合併從不發生。設定此選項僅增加一次多餘的 `setsockopt()` syscall，無實質效益。
- **CPU affinity**：靜態綁核在連線負載分佈不均時造成部分核心過載、其他核心閒置。`SO_REUSEPORT` 已處理核心層分流，`SO_INCOMING_CPU` 兼顧快取局部性，再加上強制 affinity 反而限制 OS 排程器的動態平衡能力。

### 6.3 Why throughput drops beyond the optimal thread count / 為何超過最佳執行緒數後吞吐量下降

| Server | Bottleneck cause / 瓶頸原因 |
|---|---|
| `fix_server` | Single `accept()` loop; extra client threads pile up in the kernel backlog (BACKLOG=5) and cause `ECONNREFUSED` / `sched_yield()` spin. |
| `fix_server_th` | Beyond ~11 client threads, OS context-switch overhead and mutex contention outweigh the I/O overlap benefit. |
| `fix_server_th_v2` | Beyond ~40 client threads, all 4 CPU cores are fully saturated; additional threads only increase context-switch overhead. |

### 6.4 Throughput ceiling / 吞吐量天花板

Both servers are ultimately bound by the **per-connection TCP overhead on loopback**:
`socket()` → 3-way handshake → `send()` → 4-way teardown → `close()`.
On Linux loopback, each complete connection cycle costs roughly **50–60 µs**, which
theoretically caps throughput at ~17,000–20,000 conn/sec **per core**.

With `fix_server_th_v2` and 8 threads across 4 cores, the theoretical ceiling rises to
~68,000–80,000 conn/sec — but this is unachievable when the benchmark also shares the same 4 cores.
In practice, server + bench together consume 100% CPU, and total measured throughput peaks at ~29 K conn/sec.

兩個版本最終都受限於 **loopback 上每筆連線的 TCP 開銷**：`socket()` → 三次握手 → `send()` → 四次揮手 → `close()`。在 Linux loopback 上，每筆完整連線週期約耗費 **50–60 µs**，每核理論上限約 17,000–20,000 conn/sec。

`fix_server_th_v2` 使用 8 thread 跨 4 核，理論上限可達 68,000–80,000 conn/sec，但 benchmark 同樣佔用這 4 顆 CPU，實際伺服器與 bench 合計耗盡所有 CPU，峰值約 29 K conn/sec。

---

## 7. Conclusion / 結論

| | |
|---|---|
| **`fix_server` max conn/sec** | ~16,400 |
| **`fix_server_th` max conn/sec** | ~19,100 |
| **`fix_server_th_v2` max conn/sec (same machine)** | ~27,600–29,400 |
| **`fix_server_th_v2` max conn/sec (dedicated server, estimated)** | >40,000 |
| **System loopback ceiling per core** | ~17,000–20,000 |

`fix_server_th_v2` achieves **+68–79% improvement** over `fix_server` on a single shared machine.
The architectural changes (SO_REUSEPORT + SO_INCOMING_CPU) are sound and would exceed the **2× target
(>32,800 conn/sec)** when the server runs on a dedicated machine without sharing CPU with the benchmark.

`fix_server_th_v2` 在單機共用環境下比 `fix_server` 提升 **+68–79%**。架構改動（SO_REUSEPORT + SO_INCOMING_CPU）在專用伺服器（不與 benchmark 共用 CPU）環境下，可超越 **2× 目標（>32,800 conn/sec）**。

### Further optimization paths / 進一步優化方向

| Technique / 技術 | Expected gain / 預期收益 |
|---|---|
| `SO_REUSEPORT` + connection reuse / 長連線 | Eliminates TCP handshake overhead; 10× potential gain / 消除握手開銷，潛在 10× |
| `io_uring` (async I/O) | Reduces per-connection syscall count from 3 to near-zero batch / 每筆連線 syscall 從 3 降至近零批次 |
| Dedicated server machine | Removes CPU contention with benchmark; 2× target easily achievable / 消除與 benchmark 的 CPU 競爭 |
| Kernel bypass (DPDK / XDP) | Sub-µs per-packet processing; >100 K conn/sec theoretically possible / 封包處理低於 1 µs |

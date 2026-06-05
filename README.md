# Hello-Fix

一個以 C 語言實作 FIX Protocol 的學習專案，從單執行緒伺服器出發，透過 **Claude AI** 協助分析瓶頸、設計優化方案，最終以 `SO_REUSEPORT` 架構將吞吐量提升近 **70–80%**。

---

## 版本演進

```
fix_server          →     fix_server_th          →     fix_server_th_v2
 (單執行緒)               (5 workers + mutex)           (N workers, SO_REUSEPORT)
 ~16,400 conn/sec          ~19,100 conn/sec               ~26,000–29,400 conn/sec
```

### v1 — `fix_server`（單執行緒）

最初版本：一條主執行緒跑 `while(1)` 迴圈，`accept()` → `send()` → `close()`。
瓶頸在於所有操作完全序列化，無法利用多核。

### v2 — `fix_server_th`（多執行緒 + accept mutex）

加入 5 條 pthread worker thread。但因為 `accept()` 需要用 `accept_mutex` 序列化（避免 thundering herd），實際上只有 send/close 可重疊，並非真正平行。相較 v1 僅提升約 **16%**。

### v3 — `fix_server_th_v2`（SO_REUSEPORT，當前版本）

由 **Claude** 分析 v2 的 mutex 瓶頸後，設計此版本：

- 每條 worker thread 各自建立獨立的 listening socket，全部 bind 同一 port
- 核心以 `SO_REUSEPORT` 在 SYN 封包層直接分派連線，完全消除 mutex
- 搭配 `SO_INCOMING_CPU` 提示核心將連線導向同核心的 socket，降低 L1/L2 cache miss
- 相較 v1 提升 **+68–79%**，4 核單機環境峰值達 ~26,000–29,400 conn/sec

---

## 檔案說明

| 檔案 | 說明 |
|------|------|
| `fix_server.c` | v1：單執行緒 while-loop 伺服器，發送 FIX 4.2 Heartbeat，支援 daemon 模式 |
| `fix_server_th.c` | v2：5 條 worker thread + accept_mutex |
| `fix_server_th_v2.c` | v3：N workers + SO_REUSEPORT（當前最高效能版本） |
| `fix_client.c` | 客戶端，接收並驗證 CheckSum，印出 FIX 訊息內容 |
| `test_unit.c` | `calculate_checksum()` 的單元測試，共 6 個測試案例 |
| `bench_fix_server_th.c` | 阻塞式連線速率基準測試（C11 + pthreads） |
| `bench_fix_server_v2.c` | epoll 非阻塞高吞吐量基準測試，對應 fix_server_th_v2 |
| `Makefile` | 建置所有執行檔並支援單元測試 |
| `benchmark_report.md` | 三個版本的完整效能測試報告 |

---

## 建置

```bash
make            # 建置所有執行檔
make test       # 建置並執行單元測試
make bench      # 僅建置 bench_fix_server_th
make bench_v2   # 僅建置 bench_fix_server_v2
make clean      # 移除所有執行檔與目的檔
```

---

## 使用方式

### fix_server（v1，單執行緒）

```bash
./fix_server              # 前景執行
./fix_server -d           # Daemon 模式（PID → /tmp/fix_server.pid）
```

### fix_server_th（v2，多執行緒）

```bash
./fix_server_th           # 前景執行（5 workers）
./fix_server_th -d        # Daemon 模式（PID → /tmp/fix_server_th.pid）
```

### fix_server_th_v2（v3，高效能，當前版本）

```bash
./fix_server_th_v2        # 前景執行（thread 數 = CPU 邏輯核心數）
./fix_server_th_v2 8      # 指定 8 條 worker thread
./fix_server_th_v2 -d     # Daemon 模式（PID → /tmp/fix_server_th_v2.pid）
./fix_server_th_v2 -d 8   # Daemon + 指定 thread 數
```

### fix_client

```bash
./fix_client              # 另開終端機連線至伺服器（port 5001）
```

### 基準測試

```bash
# v1/v2 伺服器適用（阻塞式，20 threads，5 秒）
./bench_fix_server_th 5 20

# v3 伺服器適用（epoll，8 workers × 200 conn，5 秒）
./bench_fix_server_v2 5 8 200
```

---

## 架構說明

### fix_server_th（v2）

```
main()
 ├─ socket / bind / listen（單一 fd）
 ├─ daemon()（若 -d）
 └─ pthread_create ×5
       └─ worker_thread()
             ├─ lock(accept_mutex)
             ├─ accept()
             ├─ unlock(accept_mutex)
             ├─ send FIX message（partial-send 迴圈）
             └─ close(client_fd)
```

| 項目 | 說明 |
|------|------|
| Thread 數量 | `NUM_THREADS 5`（編譯期常數） |
| Accept 保護 | `pthread_mutex_t accept_mutex`，防止 thundering herd |
| Send 保護 | partial-send 迴圈，確保完整送出 |

### fix_server_th_v2（v3，當前版本）

```
main()
 ├─ 為每條 thread 各自建立 socket
 │    └─ SO_REUSEADDR + SO_REUSEPORT + SO_INCOMING_CPU
 │    └─ bind / listen（BACKLOG=1024）
 ├─ daemon()（若 -d）
 ├─ sigaction(SIGTERM/SIGINT → g_stop=1)
 └─ pthread_create ×N
       └─ worker_thread()
             ├─ accept(own_fd)        ← 無 mutex
             ├─ send FIX message（partial-send + MSG_NOSIGNAL）
             └─ close(client_fd)
```

| 項目 | 說明 |
|------|------|
| Thread 數量 | 預設 `sysconf(_SC_NPROCESSORS_ONLN)`，上限 64，可由命令列覆寫 |
| Accept 保護 | 無需 mutex；核心以 `SO_REUSEPORT` hash 分派連線至各 thread 專屬 socket |
| 套用優化 | `SO_REUSEPORT`、`SO_INCOMING_CPU`、`MSG_NOSIGNAL`、`BACKLOG=1024` |
| 未套用 | `TCP_NODELAY`（單次 send 即 close，Nagle 無作用）、CPU affinity（靜態綁核導致排程不平衡） |
| 優雅關機 | `volatile sig_atomic_t g_stop`；SIGTERM/SIGINT 觸發 |

---

## 效能比較

> 測試環境：Linux 6.17，4 核心，伺服器與 benchmark 同機執行（loopback 127.0.0.1）。
> 最後測試日期：2026-06-05。詳細數據見 [`benchmark_report.md`](benchmark_report.md)。

| 版本 | 架構 | 峰值 conn/sec | vs v1 | 最佳 bench 設定 |
|------|------|---:|:---:|---|
| `fix_server` | 單執行緒 | ~16,400 | baseline | 5 threads |
| `fix_server_th` | 5 workers + mutex | ~19,100 | **+16%** | 10–11 threads |
| `fix_server_th_v2` | N workers, SO_REUSEPORT | ~26,000–29,400 | **+68–79%** | 8 workers × 200 (epoll) |

> **天花板說明：** 在 4 核單機環境下，伺服器與 benchmark 共用所有 CPU，
> 26–29 K conn/sec 時已達 CPU 100%。若使用獨立伺服器機器，預估可超越 **2× 目標（>32,800 conn/sec）**。

---

## Claude AI 優化歷程

本專案以 **Claude** 作為效能顧問，主要貢獻如下：

| 階段 | Claude 的角色 |
|------|--------------|
| v1 → v2 | 說明 thundering herd 問題，建議引入 pthread + accept_mutex |
| v2 → v3 | 分析 mutex 仍是序列化瓶頸，提出 `SO_REUSEPORT` 架構，讓核心取代應用層鎖 |
| v3 優化 | 評估 `SO_INCOMING_CPU`（+20% 效益）、排除 `TCP_NODELAY` 與 CPU affinity 的誤用 |
| 測試工具 | 設計 epoll 版 `bench_fix_server_v2`，突破阻塞式 bench 的 CPU 瓶頸 |
| 報告 | 撰寫 `benchmark_report.md`，分析瓶頸成因與吞吐量天花板 |

---

## FIX 訊息格式

伺服器發送一則 FIX 4.2 Heartbeat（`35=0`）：

```
8=FIX.4.2 | 9=51 | 35=0 | 49=200201 | 56=TWSE | 34=593 | 52=20090803-13:37:47 | 10=233
```

| Tag | 欄位 | 值 |
|-----|------|----|
| 8 | BeginString | FIX.4.2 |
| 9 | BodyLength | 51 |
| 35 | MsgType | 0（Heartbeat） |
| 49 | SenderCompID | 200201 |
| 56 | TargetCompID | TWSE |
| 34 | MsgSeqNum | 593 |
| 52 | SendingTime | 20090803-13:37:47 |
| 10 | CheckSum | 233 |

欄位之間以 SOH（`0x01`）分隔。CheckSum = tag 10 之前所有位元組的總和 mod 256。

---

## 單元測試

```bash
make test
```

| 測試 | 輸入 | 預期結果 |
|------|------|----------|
| TC-01 | 真實 FIX 訊息前綴 | 233 |
| TC-02 | 空字串 | 0 |
| TC-03 | 單一字元 `'A'`（65） | 65 |
| TC-04 | 單一位元組 `0xFF` | 255 |
| TC-05 | `0xFF` + `0x01` 溢位 | 0 |
| TC-06 | `0x80` + `0x7F` | 255 |

---

## 環境

- 預設 port：`5001`
- 編譯器：gcc，旗標 `-g -Wall -Wextra`
- pthread：`-lpthread`（fix_server_th、fix_server_th_v2、bench_fix_server_th、bench_fix_server_v2）

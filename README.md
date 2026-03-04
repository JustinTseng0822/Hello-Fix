# Hello-Fix

一個以 C 語言實作 FIX Protocol 的學習專案。

## 檔案說明

| 檔案 | 說明 |
|------|------|
| `fix_server.c` | TCP while(1) - loop 伺服器 ，發送 FIX 4.2 Heartbeat，支援 Daemon 模式 |
| `fix_server_th.c` | 多執行緒版本，預設啟動 5 條 worker thread，使用 pthread |
| `fix_client.c` | 客戶端，接收並驗證 CheckSum，印出 FIX 訊息內容 |
| `test_unit.c` | `calculate_checksum()` 的單元測試，共 6 個測試案例 |
| `Makefile` | 建置所有執行檔並支援單元測試 |

## 建置

```bash
make          # 建置 fix_client、fix_server、fix_server_th、test_unit
make test     # 建置並執行單元測試
make clean    # 移除所有執行檔與目的檔
```

## 使用方式

### fix_server（while(1) - loop）

```bash
# 前景模式
./fix_server

# Daemon 模式（背景執行，PID 寫入 /tmp/fix_server.pid）
./fix_server -d
cat /tmp/fix_server.pid
```

### fix_server_th（多執行緒，pthread）

```bash
# 前景模式（啟動 5 條 worker thread）
./fix_server_th

# Daemon 模式（PID 寫入 /tmp/fix_server_th.pid）
./fix_server_th -d
cat /tmp/fix_server_th.pid
```

### fix_client

```bash
# 另開終端機連線至伺服器
./fix_client
```

## fix_server_th 架構

```
main()
 ├─ socket / bind / listen
 ├─ daemon()（若 -d）
 └─ pthread_create x5
       └─ worker_thread()
             ├─ lock accept_mutex
             ├─ accept()
             ├─ unlock accept_mutex
             ├─ send FIX message（partial-send 迴圈）
             └─ close client_fd
```

| 項目 | 說明 |
|------|------|
| Thread 數量 | `NUM_THREADS 5`（編譯期常數） |
| Accept 保護 | `pthread_mutex_t accept_mutex` 避免 thundering herd |
| Send 保護 | partial-send 迴圈，確保完整送出 |
| Daemon PID | 寫入 `/tmp/fix_server_th.pid` |

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

## 連接埠

預設：`5001`

## 單元測試案例

| 測試 | 輸入 | 預期結果 |
|------|------|----------|
| TC-01 | 真實 FIX 訊息前綴 | 233 |
| TC-02 | 空字串 | 0 |
| TC-03 | 單一字元 `'A'`（65） | 65 |
| TC-04 | 單一位元組 `0xFF` | 255 |
| TC-05 | `0xFF` + `0x01` 溢位 | 0 |
| TC-06 | `0x80` + `0x7F` | 255 |

## 編譯環境

- 編譯器：gcc
- 旗標：`-g -Wall -Wextra`
- pthread：`-lpthread`（僅 fix_server_th）

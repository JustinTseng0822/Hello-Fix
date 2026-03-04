# Hello-Fix

一個以 C 語言實作 [FIX Protocol] 的學習專案。

## 檔案說明

| 檔案 | 說明 |
|------|------|
| `fix_server.c` | TCP 伺服器，負責發送 FIX 4.2 Heartbeat 訊息 |
| `fix_client.c` | 客戶端，負責接收、驗證 checksum 並印出 FIX 訊息 |
| `test_unit.c` | `calculate_checksum()` -> 測試 checksum 計算函數的單元測試 |
| `Makefile` | 建置 `fix_client`、`fix_server` 及 `test_unit` |

## 建置

```bash
make          # 建置 fix_client 和 fix_server
make test     # 建置並執行單元測試
make clean    # 移除所有執行檔與目的檔
```

## 使用方式

### 前景模式

```bash
# 終端機 1
./fix_server

# 終端機 2
./fix_client
```

### Daemon 模式

```bash
./fix_server -d       # 在背景執行，並印出 PID
./fix_client          # 連線至 daemon
```

## FIX 訊息

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
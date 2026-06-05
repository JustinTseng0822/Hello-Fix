# Backlog

## fix_server_th_v2.c — Code Review 修正項目

> 來源：RD2 Code Review（2026-04-20）
> 狀態說明：`[ ]` 待處理 ／ `[x]` 已完成

---

### Major

- [x] **M-1｜strtol 缺 endptr 檢查**（第 224 行）
  輸入 `"4abc"` 或 `""` 會被靜默接受為合法值。應傳入 `&endptr` 並驗證 `*endptr == '\0'`。
  ✅ 已修正（2026-06-05）：`fix_server_th_v2.c:224`、`bench_fix_server_th.c:166,176`

- [ ] **M-2｜g_stop 跨 thread 可見性不足**（第 92 行）
  `volatile sig_atomic_t` 不提供 C11 memory ordering，在 ARM 等弱序架構有可見性延遲風險。應改用 `_Atomic int`（`<stdatomic.h>`）搭配 `memory_order_relaxed`。

- [ ] **M-3｜PID 檔 symlink attack**（第 327–331 行）
  `fopen("/tmp/fix_server_th_v2.pid", "w")` 可被 symlink 攻擊截斷任意檔案。應改用 `open()` + `O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW`。

- [ ] **M-4｜accept() 錯誤分類不足，致命錯誤造成 tight loop**（第 153–154 行）
  `EBADF`/`EINVAL` 等致命錯誤與 `EMFILE` 等資源不足錯誤一律 `continue`，前者會造成 thread 緊密迴圈耗盡 CPU。應對 `EBADF`/`EINVAL` 執行 `break`。

- [ ] **M-5｜pthread_create 部分失敗時 blocking thread 無法被喚醒**（第 366–373 行）
  設 `g_stop = 1` 無法中斷阻塞在 `accept()` 的 thread，需要 signal 才能讓其觀察到 `g_stop`。應在設 `g_stop = 1` 後對已建立的 thread 發送 `pthread_kill(threads[i], SIGTERM)`。

---

### Minor

- [ ] **m-1｜sigaction 回傳值未檢查**（第 345–346 行）
  失敗時 signal handler 不會被安裝，程式無法優雅關機。應加 `if (sigaction(...) < 0) { perror(...); }` 處理。

- [ ] **m-2｜send() 失敗完全靜默**（第 165–171 行）
  熱路徑避免 printf 是正確設計，但除錯困難。可加 per-thread error counter，於 thread 結束後統一輸出。

- [ ] **m-3｜goto cleanup_fds 結構脆弱**（第 394 行）
  正常執行流與錯誤流共用 label，未來插入新資源容易導致 double-free 或 leak。建議改用結構化 cleanup 函式，或對已 free 的指標清零（置 `NULL`）。

- [ ] **m-4｜main() 流程注解第 7 點與實際行為矛盾**（第 191 行）
  注解仍寫「CPU affinity：pin thread i → core (i % num_cores)」，實際未套用。應改為「CPU affinity：未套用，見設計說明第 5 點」。

- [ ] **m-5｜DEFAULT_THREADS 命名語意偏差**（第 77 行）
  此常數是 `sysconf` 失敗時的 fallback，非真正預設值。建議重新命名為 `FALLBACK_THREADS`。

- [ ] **m-6｜worker_arg_t.thread_id 未在 worker_thread() 中使用**（第 114 行）
  affinity 移除後此欄位無任何引用，應移除或加 `(void)warg->thread_id` 抑制潛在警告。

---

### Info

- [ ] **i-1｜sched.h include 但未使用**（第 69 行）
  CPU affinity 未套用後 `<sched.h>` 無任何引用，可移除或加注解說明保留原因。

- [ ] **i-2｜partial-send 迴圈說明不足**（第 163–172 行）
  在目前訊息長度（~80 bytes）下實際只跑一次，可補充注解說明迴圈為正確性保護而存在。

- [ ] **i-3｜Makefile 未指定 -std**（`Makefile` 第 9–10 行）
  未明確指定 C 標準，建議加入 `-std=gnu11` 或 `-std=gnu17` 並說明原因。

---

## 資安掃描修正項目

> 來源：RD2 Security Review（2026-06-05）
> 涵蓋範圍：`fix_server.c`、`fix_server_th.c`、`fix_server_th_v2.c`、`fix_client.c`、`bench_fix_server_th.c`、`bench_fix_server_v2.c`

---

### High

- [x] **H-3｜無 send timeout，可被 DoS**
  惡意客戶端連線後不讀取資料，`send()` 永久阻塞。單執行緒版 `fix_server.c` 一條連線即可癱瘓服務。
  ✅ 已修正（2026-06-05）：三個伺服器 `accept()` 後加入 `SO_SNDTIMEO = 5s`。

- [ ] **H-1｜INADDR_ANY 無來源限制**（`fix_server.c:69`、`fix_server_th.c:194`、`fix_server_th_v2.c:267`）
  三個伺服器監聽所有網路介面，任何可到達 port 5001 的主機均可連線，無 IP 過濾或認證機制。
  若非需對外服務，應改為指定內部介面 IP 或由防火牆層過濾。

- [ ] **H-2｜`goto cleanup_fds` 跳過 `free()` 的結構脆弱性**（`fix_server_th_v2.c:353-357`）
  `threads`/`args` 分配後，未來若在其後插入新的 `goto cleanup_fds`，將導致記憶體洩漏。
  建議將 `free(threads)` 與 `free(args)` 移入 `cleanup_fds:` 段落統一處理（同 Code Review m-3）。

---

### Medium

- [ ] **M-sec-1｜`server_addr` / `client_addr` 未 `memset` 清零**（`fix_server.c:37-38`、`fix_client.c:30`）
  `sin_zero` padding 欄位保留 stack garbage，可能在 logging 或嚴格 socket 實作中洩漏 stack 內容。
  應在賦值前加 `memset(&addr, 0, sizeof(addr))`。

- [ ] **M-sec-2｜整數乘法潛在溢位**（`bench_fix_server_v2.c:571`）
  `int total_conns = num_workers * batch_per_worker`，上限乘積 2,560,000 目前安全，但型別不防禦未來擴充。
  應改為 `long total_conns = (long)num_workers * batch_per_worker`。

- [ ] **M-sec-3｜`getsockopt` 回傳值未檢查**（`bench_fix_server_v2.c:372-373`）
  失敗時 `so_err` 保持 0，程式錯誤判斷連線成功。應檢查 `getsockopt` 回傳值，`< 0` 時視為連線失敗。

- [ ] **M-sec-4｜PID 檔寫入失敗靜默繼續**（`fix_server.c:107-112`、`fix_server_th.c:232-237`、`fix_server_th_v2.c:327-331`）
  `fopen` 失敗時程式不回報任何錯誤，daemon 管理腳本無法取得 PID，可能導致多個 daemon 實例並存。
  應在 `fopen` 失敗時輸出錯誤或終止程序。

- [ ] **M-sec-5｜signal handler 在 `daemon()` 後才設定，存在競態窗口**（`fix_server_th_v2.c:318-346`）
  `daemon()` 呼叫後、handler 設定前若收到 SIGTERM，程序使用預設行為終止且不做 cleanup。
  應將 `sigaction` 移至 thread 建立前完成。

---

### Low

- [x] **L-3｜`send()` 缺少 `MSG_NOSIGNAL`**（`fix_server.c:142`、`fix_server_th.c:119`）
  客戶端中斷連線時發出 SIGPIPE，預設行為終止整個程序。
  ✅ 已修正（2026-06-05）：兩個檔案的 `send()` 加入 `MSG_NOSIGNAL`。

- [ ] **L-1｜`pthread_mutex_unlock` 失敗後其餘 thread 永久死鎖**（`fix_server_th.c:90-96`）
  unlock 失敗後 `break` 退出，mutex 進入 undefined state，其他 thread 的 `lock` 將永遠阻塞。
  應觸發全域停止旗標或程式重啟，而非靜默退出。

- [ ] **L-4｜Makefile `rm` 變數定義非標準**（`Makefile:1`）
  `rm=/bin/rm -f` 將選項塞入變數名稱，非標準做法，跨平台可能解析失敗。
  應改為 `RM = /bin/rm` 並在使用時寫 `$(RM) -f`。

- [ ] **L-5｜port 號硬編碼**（`fix_server.c:11`、`fix_server_th.c:13`、`fix_server_th_v2.c:75`）
  Port 5001 無法透過命令列或環境變數變更，部署彈性不足。
  建議新增 `-p port` 選項或讀取環境變數 `FIX_PORT`。

# Backlog

## fix_server_th_v2.c — Code Review 修正項目

> 來源：RD2 Code Review（2026-04-20）
> 狀態說明：`[ ]` 待處理 ／ `[x]` 已完成

---

### Major

- [ ] **M-1｜strtol 缺 endptr 檢查**（第 224 行）
  輸入 `"4abc"` 或 `""` 會被靜默接受為合法值。應傳入 `&endptr` 並驗證 `*endptr == '\0'`。

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

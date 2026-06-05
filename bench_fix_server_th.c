/*
 * bench_fix_server_th.c
 *
 * 功能：對 fix_server_th（127.0.0.1:5001）進行連線速率基準測試。
 *
 * 測試邏輯：
 *   - 啟動 num_threads 條 benchmark thread，每條 thread 不斷循環：
 *       connect -> recv FIX message（直到 "10=" 後的 SOH 0x01）-> close
 *   - 使用 C11 _Atomic long 累計成功連線總數
 *   - 主程式等待 duration_sec 秒後，設定停止旗標，等所有 thread 結束，
 *     計算並輸出 conn/sec
 *
 * 編譯方式（由 Makefile 管理）：
 *   gcc -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -o bench_fix_server_th bench_fix_server_th.c -lpthread
 *
 * 注意：_POSIX_C_SOURCE 與 _DEFAULT_SOURCE 由 Makefile 的 -D 旗標傳入，
 *       原始檔此處不重複定義，避免產生 "redefined" 警告。
 *
 * 用法：
 *   ./bench_fix_server_th [duration_sec] [num_threads]
 *   預設：duration=5, threads=20
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

/* ---- 設定常數 ---- */
#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 5001
#define BUF_SIZE    4096

/* ---- 全域共享狀態 ---- */

/* 停止旗標：主程式計時結束後設為 1，通知所有 thread 退出 */
static _Atomic int g_stop = 0;

/* 成功連線計數器（atomic，免加鎖） */
static _Atomic long g_conn_count = 0;

/* ---- thread 參數結構 ---- */
typedef struct {
    int thread_id;      /* 用於偵錯輸出（目前未使用，保留供擴充） */
} thread_arg_t;

/*
 * recv_fix_message()
 *
 * 從已連線的 socket fd 接收完整 FIX 訊息。
 * 判斷終止條件：在緩衝區中找到 "10=" 後緊接著一個 SOH（0x01）byte，
 * 表示 CheckSum 欄位已完整接收。
 *
 * 回傳：
 *   >= 0  成功收到的總位元組數
 *   -1    recv 錯誤或緩衝區已滿
 */
static ssize_t recv_fix_message(int fd, char *buf, size_t bufsz)
{
    ssize_t total = 0;

    while (total < (ssize_t)(bufsz - 1)) {
        ssize_t n = recv(fd, buf + total,
                         (bufsz - 1) - (size_t)total, 0);
        if (n < 0) {
            /* recv 失敗 */
            return -1;
        }
        if (n == 0) {
            /* 對端提前關閉連線 */
            break;
        }
        total += n;
        /* 補 '\0' 確保 strstr/strchr 不越界 */
        buf[total] = '\0';

        /*
         * FIX 結束判斷：找到 "10=" 欄位（CheckSum tag），
         * 再確認其後至少有一個 SOH（0x01）。
         */
        char *cs_field = strstr(buf, "10=");
        if (cs_field != NULL) {
            char *soh_after_cs = strchr(cs_field + 3, 0x01);
            if (soh_after_cs != NULL) {
                /* CheckSum 欄位含尾端 SOH 已完整接收 */
                break;
            }
        }
    }

    return total;
}

/*
 * bench_thread()
 *
 * 每條 benchmark thread 的主函式。
 * 迴圈：connect -> recv FIX msg -> close，直到 g_stop 被設為非零。
 */
static void *bench_thread(void *arg)
{
    (void)arg;  /* 目前未使用 thread_arg_t 內容 */

    char buf[BUF_SIZE];
    struct sockaddr_in server_addr;

    /* 預先填好 server 位址，每次迴圈重用 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "[bench_thread] inet_pton failed\n");
        return NULL;
    }

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {

        /* 1. 建立 socket */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            /* socket 建立失敗，短暫讓出 CPU 再重試 */
            sched_yield();
            continue;
        }

        /* 2. 連線到伺服器 */
        if (connect(fd, (struct sockaddr *)&server_addr,
                    sizeof(server_addr)) < 0) {
            /* 連線失敗，關閉 fd 後繼續下一輪（不退出 thread） */
            close(fd);
            sched_yield();
            continue;
        }

        /* 3. 接收完整 FIX 訊息 */
        ssize_t received = recv_fix_message(fd, buf, sizeof(buf));

        /* 4. 關閉連線 */
        close(fd);

        /* 5. 只有成功收到資料才算一次有效連線 */
        if (received > 0) {
            atomic_fetch_add_explicit(&g_conn_count, 1L,
                                      memory_order_relaxed);
        }
    }

    return NULL;
}

/* ---- 主程式 ---- */
int main(int argc, char *argv[])
{
    int duration_sec = 5;
    int num_threads  = 20;

    /* 解析命令列參數 */
    if (argc >= 2) {
        char *endptr;
        long v = strtol(argv[1], &endptr, 10);
        if (endptr == argv[1] || *endptr != '\0' || v <= 0 || v > 3600) {
            fprintf(stderr, "Invalid duration_sec: %s (must be 1-3600)\n",
                    argv[1]);
            return EXIT_FAILURE;
        }
        duration_sec = (int)v;
    }
    if (argc >= 3) {
        char *endptr;
        long v = strtol(argv[2], &endptr, 10);
        if (endptr == argv[2] || *endptr != '\0' || v <= 0 || v > 10000) {
            fprintf(stderr, "Invalid num_threads: %s (must be 1-10000)\n",
                    argv[2]);
            return EXIT_FAILURE;
        }
        num_threads = (int)v;
    }

    printf("Benchmarking fix_server_th (%s:%d)\n", SERVER_IP, SERVER_PORT);
    printf("Threads: %d | Duration: %ds\n", num_threads, duration_sec);
    fflush(stdout);

    /* 建立 thread 陣列與參數陣列 */
    pthread_t *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    thread_arg_t *args = malloc((size_t)num_threads * sizeof(thread_arg_t));
    if (!threads || !args) {
        fprintf(stderr, "malloc failed\n");
        free(threads);
        free(args);
        return EXIT_FAILURE;
    }

    /* 初始化 atomic 變數 */
    atomic_store(&g_stop, 0);
    atomic_store(&g_conn_count, 0L);

    /* 記錄實際開始時間（用 CLOCK_MONOTONIC 避免系統時間跳躍） */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* 啟動所有 benchmark thread */
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        int ret = pthread_create(&threads[i], NULL, bench_thread, &args[i]);
        if (ret != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(ret));
            /* 標記停止，等待已啟動的 thread */
            atomic_store(&g_stop, 1);
            for (int j = 0; j < i; j++)
                pthread_join(threads[j], NULL);
            free(threads);
            free(args);
            return EXIT_FAILURE;
        }
    }

    /* 主程式等待指定秒數 */
    sleep((unsigned int)duration_sec);

    /* 通知所有 thread 停止 */
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);

    /* 等待所有 thread 結束 */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    /* 記錄結束時間，計算實際經過時間 */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (double)(ts_end.tv_sec  - ts_start.tv_sec) +
                     (double)(ts_end.tv_nsec - ts_start.tv_nsec) * 1e-9;

    long total_conn = atomic_load(&g_conn_count);
    double conn_per_sec = (elapsed > 0.0) ? (double)total_conn / elapsed : 0.0;

    /* 輸出結果 */
    printf("Total connections: %ld\n", total_conn);
    printf("Connections/sec : %.1f\n", conn_per_sec);

    free(threads);
    free(args);
    return EXIT_SUCCESS;
}

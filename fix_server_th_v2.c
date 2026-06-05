/*
 * fix_server_th_v2.c
 *
 * 高效能 FIX 4.2 伺服器 — 目標吞吐量 >32,800 conn/sec (>2x fix_server_th)
 *
 * 核心最佳化設計：
 *
 *  1. SO_REUSEPORT — 每條 worker thread 各自建立獨立 socket 並 bind 同一
 *     port。核心會將新連線 hash 分散到各 socket，完全消除共享 accept()
 *     所需的 mutex，避免 thundering herd。
 *
 *  2. 無 mutex accept — 每條 thread 在自己的 server_fd 上直接呼叫
 *     accept()，不需鎖定任何共享資料結構。
 *
 *  3. TCP_NODELAY — 【未套用】本伺服器採「一連線一回應即關閉」模式，
 *     每條連線僅呼叫一次 send() 即 close()。Nagle 演算法只在同一連線
 *     連續發出多個小封包時才會造成延遲合併；單次 send 後立即關閉的情境
 *     下，核心會在 FIN 之前強制 flush 緩衝區，TCP_NODELAY 對吞吐量與
 *     延遲均無實質幫助，故不設定以減少無效 syscall。
 *
 *  4. BACKLOG=1024 — 加大 listen 佇列，避免連線在 SYN backlog 滿時被丟棄。
 *
 *  5. CPU affinity — 【未套用】靜態 pinning（thread i → core i % N）在
 *     連線流量分佈不均時會造成部分核心過載、其他核心閒置，反而降低整體
 *     吞吐量。SO_REUSEPORT 已由核心依連線負載動態分流，OS 排程器可進一步
 *     在核心間自由遷移 thread；兩者疊加下，強制 affinity 帶來的 cache
 *     locality 收益無法彌補排程不平衡的損失，故不啟用。
 *
 *  6. MSG_NOSIGNAL — send() 時傳入此旗標，避免對端關閉連線時產生
 *     SIGPIPE 訊號導致 process 意外終止。
 *
 *  7. 熱路徑無 printf — accept/send/close 主迴圈中不呼叫 printf，
 *     避免 stdio 鎖爭用成為瓶頸。
 *
 *  8. 動態 thread 數量 — 預設為 sysconf(_SC_NPROCESSORS_ONLN)（本機
 *     邏輯核心數），上限 64；可由命令列第一個位置參數覆寫。
 *
 *  9. 優雅關機 — 以 volatile sig_atomic_t g_stop 作停止旗標；
 *     SIGTERM / SIGINT 皆設為相同 handler。
 *
 * 10. Daemon 模式 — -d 旗標啟用，PID 寫入 /tmp/fix_server_th_v2.pid。
 *
 * 編譯（由 Makefile 管理）：
 *   gcc -Wall -Wextra -D_GNU_SOURCE \
 *       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -o fix_server_th_v2 fix_server_th_v2.c -lpthread
 *
 * 用法：
 *   ./fix_server_th_v2 [-d] [num_threads]
 *   -d            daemon 模式（背景執行，PID 寫入 /tmp/fix_server_th_v2.pid）
 *   num_threads   worker thread 數量（預設：CPU 邏輯核心數，最多 64）
 *
 * 注意：_GNU_SOURCE 定義在原始檔頂端，不由 Makefile -D 傳入，
 *       以確保 pthread_setaffinity_np 與 CPU_SET 等 GNU 擴充可見。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/types.h>

/* -------------------------------------------------------------------------
 * 常數定義
 * ------------------------------------------------------------------------- */
#define PORT            5001   /* 監聽埠號 */
#define BACKLOG         1024   /* listen() 等待佇列長度 */
#define DEFAULT_THREADS 4      /* 預設 thread 數量（命令列未指定時使用） */
#define MAX_THREADS     64     /* thread 數量上限 */

/* -------------------------------------------------------------------------
 * FIX 4.2 Heartbeat 訊息（欄位以 SOH 0x01 分隔）
 * 與其他伺服器版本完全一致，宣告為 static const 避免跨 TU 污染。
 * ------------------------------------------------------------------------- */
static const char fix_msg[] =
    "8=FIX.4.2\x01"
    "9=51\x01"
    "35=0\x01"
    "49=200201\x01"
    "56=TWSE\x01"
    "34=593\x01"
    "52=20090803-13:37:47\x01"
    "10=233\x01";

/* -------------------------------------------------------------------------
 * 全域停止旗標
 * signal handler 將此變數設為 1，worker thread 在每輪迴圈頂端檢查。
 * ------------------------------------------------------------------------- */
static volatile sig_atomic_t g_stop = 0;

/* -------------------------------------------------------------------------
 * signal_handler() — SIGTERM / SIGINT 共用 handler
 * 僅設定 g_stop，其餘邏輯由 worker thread 自行處理。
 * ------------------------------------------------------------------------- */
static void signal_handler(int signo)
{
    (void)signo;   /* 不需區分訊號種類，直接設停止旗標 */
    g_stop = 1;
}

/* -------------------------------------------------------------------------
 * worker_arg_t — 傳給每條 worker thread 的參數結構
 * ------------------------------------------------------------------------- */
typedef struct {
    int thread_id;   /* thread 編號（0-based），用於 CPU affinity 計算 */
    int server_fd;   /* 此 thread 專屬的 listening socket fd */
} worker_arg_t;

/* -------------------------------------------------------------------------
 * worker_thread() — 每條 pthread 執行的入口函式
 *
 * 流程（無 mutex，直接 accept 自有 server_fd）：
 *   while (!g_stop)
 *     accept()       — 取得 client_fd
 *     TCP_NODELAY    — 停用 Nagle，立即送出資料
 *     partial-send   — 以迴圈送出完整 fix_msg（MSG_NOSIGNAL 避免 SIGPIPE）
 *     close()        — 關閉 client_fd
 *
 * 當 accept() 被訊號中斷（EINTR）且 g_stop 已被設定時，跳出迴圈結束 thread。
 * 其他非致命錯誤（EMFILE、ENFILE 等）略過繼續執行。
 * ------------------------------------------------------------------------- */
static void *worker_thread(void *arg)
{
    worker_arg_t       *warg       = (worker_arg_t *)arg;
    const int           server_fd  = warg->server_fd;
    const size_t        msg_len    = sizeof(fix_msg) - 1;  /* 不含 '\0' */

    while (!g_stop) {

        /* ----------------------------------------------------------------
         * accept() — 在此 thread 專屬的 server_fd 上等待新連線。
         * 不需持有任何 mutex，SO_REUSEPORT 保證核心已替每個 fd 分流。
         * ---------------------------------------------------------------- */
        /* NULL,NULL：不拷貝 client 位址，省略 socklen_t 寫入 */
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) {
                /* 被訊號中斷：若已要求停止則退出，否則重試 */
                if (g_stop)
                    break;
                continue;
            }
            /* 其他非致命錯誤（如 EMFILE）：略過，繼續等待下一條連線 */
            continue;
        }

        /* ----------------------------------------------------------------
         * partial-send 迴圈：
         *   send() 不保證一次送出全部資料，必須以迴圈補送剩餘部分。
         *   MSG_NOSIGNAL：避免對端關閉連線時發出 SIGPIPE 終止 process。
         *   send() 回傳 <= 0 時視為連線中斷，跳出迴圈。
         * ---------------------------------------------------------------- */
        size_t total_sent = 0;
        while (total_sent < msg_len) {
            ssize_t sent = send(client_fd,
                                fix_msg + total_sent,
                                msg_len - total_sent,
                                MSG_NOSIGNAL);
            if (sent <= 0)
                break;
            total_sent += (size_t)sent;
        }

        close(client_fd);
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * main()
 *
 * 流程：
 *   1. 解析命令列（-d 旗標、可選的位置參數 num_threads）
 *   2. 決定 thread 數量（sysconf 或命令列，上限 MAX_THREADS）
 *   3. 為每條 thread 建立獨立 socket：
 *        SO_REUSEADDR + SO_REUSEPORT → bind → listen
 *   4. 若 -d：daemon(0,0)、寫入 PID 檔
 *   5. 註冊 SIGTERM / SIGINT handler
 *   6. pthread_create 所有 worker thread
 *   7. CPU affinity：pin thread i 到 core (i % num_cores)
 *   8. pthread_join 等待所有 thread 結束
 *   9. 關閉所有 server_fd
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt_char;

    /* ------------------------------------------------------------------
     * 命令列解析：-d（daemon）
     * ------------------------------------------------------------------ */
    while ((opt_char = getopt(argc, argv, "d")) != -1) {
        switch (opt_char) {
            case 'd':
                daemon_mode = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-d] [num_threads]\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    /* ------------------------------------------------------------------
     * 決定 worker thread 數量：
     *   優先使用命令列位置參數（第一個非選項參數）；
     *   未指定時使用 sysconf(_SC_NPROCESSORS_ONLN)；
     *   最終值夾在 [1, MAX_THREADS] 之間。
     * ------------------------------------------------------------------ */
    int num_threads;

    if (optind < argc) {
        /* 命令列有指定 thread 數量 */
        long v = strtol(argv[optind], NULL, 10);
        if (v <= 0 || v > MAX_THREADS) {
            fprintf(stderr,
                    "Invalid num_threads: %s (must be 1-%d)\n",
                    argv[optind], MAX_THREADS);
            return EXIT_FAILURE;
        }
        num_threads = (int)v;
    } else {
        /* 使用 CPU 邏輯核心數，並夾住上下限 */
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus <= 0)
            cpus = DEFAULT_THREADS;
        if (cpus > MAX_THREADS)
            cpus = MAX_THREADS;
        num_threads = (int)cpus;
    }

    /* 取得本機邏輯核心數，供 CPU affinity 計算使用 */
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores <= 0)
        num_cores = 1;

    printf("fix_server_th_v2: port=%d, threads=%d, backlog=%d\n",
           PORT, num_threads, BACKLOG);
    fflush(stdout);

    /* ------------------------------------------------------------------
     * 為每條 thread 建立獨立的 listening socket（SO_REUSEPORT）。
     * server_fds[] 陣列在 cleanup 階段統一關閉。
     * ------------------------------------------------------------------ */
    int *server_fds = malloc((size_t)num_threads * sizeof(int));
    if (!server_fds) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    /* 初始化為 -1，方便 cleanup 判斷是否需要 close */
    for (int i = 0; i < num_threads; i++)
        server_fds[i] = -1;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    for (int i = 0; i < num_threads; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            goto cleanup_fds;
        }

        /* SO_REUSEADDR：重啟時立即重用 port，避免 TIME_WAIT 阻塞 */
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEADDR");
            close(fd);
            goto cleanup_fds;
        }

        /* SO_REUSEPORT：允許多個 socket bind 相同 port，
         * 核心自動將新連線分散（per-socket accept，無 mutex）。 */
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
            perror("setsockopt SO_REUSEPORT");
            close(fd);
            goto cleanup_fds;
        }

        /* SO_INCOMING_CPU：提示核心將此 socket 的連線優先分配給
         * 指定 CPU，減少跨核心 cache miss（非致命，失敗略過）。 */
        int cpu_hint = (int)(i % (size_t)num_cores);
        setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU,
                   &cpu_hint, sizeof(cpu_hint));

        if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("bind");
            close(fd);
            goto cleanup_fds;
        }

        if (listen(fd, BACKLOG) < 0) {
            perror("listen");
            close(fd);
            goto cleanup_fds;
        }

        server_fds[i] = fd;
    }

    /* ------------------------------------------------------------------
     * Daemon 模式：daemon() 呼叫前先印出提示訊息並 flush，
     * 因為 daemon() 之後 stdout 已被重導向 /dev/null。
     * ------------------------------------------------------------------ */
    if (daemon_mode) {
        printf("Running as daemon (PID -> /tmp/fix_server_th_v2.pid)\n");
        fflush(stdout);

        if (daemon(0, 0) < 0) {
            perror("daemon");
            goto cleanup_fds;
        }

        FILE *pid_fp = fopen("/tmp/fix_server_th_v2.pid", "w");
        if (pid_fp) {
            fprintf(pid_fp, "%d\n", (int)getpid());
            fclose(pid_fp);
        }
    }

    /* ------------------------------------------------------------------
     * 註冊訊號 handler（SIGTERM / SIGINT → 設定 g_stop）。
     * 使用 sigaction 確保 SA_RESTART 行為明確。
     * ------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    /* 不設 SA_RESTART：讓 accept() 在訊號到達時以 EINTR 返回，
     * worker thread 才能及時檢查 g_stop 並退出。 */
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* ------------------------------------------------------------------
     * 建立 worker thread 及傳遞參數
     * ------------------------------------------------------------------ */
    pthread_t    *threads = malloc((size_t)num_threads * sizeof(pthread_t));
    worker_arg_t *args    = malloc((size_t)num_threads * sizeof(worker_arg_t));
    if (!threads || !args) {
        perror("malloc");
        free(threads);
        free(args);
        goto cleanup_fds;
    }

    int threads_created = 0;

    for (int i = 0; i < num_threads; i++) {
        args[i].thread_id = i;
        args[i].server_fd = server_fds[i];

        int ret = pthread_create(&threads[i], NULL, worker_thread, &args[i]);
        if (ret != 0) {
            fprintf(stderr, "pthread_create [%d] failed: %s\n",
                    i, strerror(ret));
            /* 通知已啟動的 thread 停止 */
            g_stop = 1;
            break;
        }
        threads_created++;

        /* CPU affinity 未套用：見檔頭設計說明第 5 點。
         * 靜態 pinning 在流量不均時反而造成排程不平衡，
         * 由 OS 排程器動態分配優於強制綁核。 */
    }

    /* ------------------------------------------------------------------
     * 等待所有已建立的 worker thread 結束。
     * 正常情況下 thread 會持續執行直到 g_stop 被訊號 handler 設為 1。
     * ------------------------------------------------------------------ */
    for (int i = 0; i < threads_created; i++)
        pthread_join(threads[i], NULL);

    free(threads);
    free(args);

    /* ------------------------------------------------------------------
     * Cleanup：關閉所有 server socket
     * ------------------------------------------------------------------ */
cleanup_fds:
    for (int i = 0; i < num_threads; i++) {
        if (server_fds[i] >= 0)
            close(server_fds[i]);
    }
    free(server_fds);

    return EXIT_SUCCESS;
}

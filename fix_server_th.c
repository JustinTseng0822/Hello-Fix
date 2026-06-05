#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * 常數定義
 * ----------------------------------------------------------------------- */
#define PORT        5001   /* 監聽埠號，與 fix_server.c 保持一致 */
#define BACKLOG     10     /* listen() 等待佇列長度；多 thread 時略加大 */
#define NUM_THREADS 5      /* 預設 worker thread 數量 */

/* -----------------------------------------------------------------------
 * 全域共享變數
 *   server_fd   — 由 main() 建立後供所有 thread 共用
 *   accept_mutex — 保護 accept()，同一時間只允許一條 thread 呼叫
 *   daemon_mode  — 由命令列解析，之後只讀，無需 mutex
 * ----------------------------------------------------------------------- */
static int             server_fd    = -1;
static pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             daemon_mode  = 0;

/* -----------------------------------------------------------------------
 * FIX 4.2 Heartbeat 訊息（欄位以 SOH 0x01 分隔）
 * 與 fix_server.c 完全相同，宣告為 static const 避免跨 TU 污染
 * ----------------------------------------------------------------------- */
static const char fix_msg[] =
    "8=FIX.4.2\x01"
    "9=51\x01"
    "35=0\x01"
    "49=200201\x01"
    "56=TWSE\x01"
    "34=593\x01"
    "52=20090803-13:37:47\x01"
    "10=233\x01";

/* -----------------------------------------------------------------------
 * worker_thread() — 每條 pthread 執行的入口函式
 *
 * 架構（accept mutex 模式）：
 *   1. lock   accept_mutex
 *   2. accept() — 取得 client_fd
 *   3. unlock accept_mutex
 *   4. 以 partial-send 迴圈送出整份 FIX message
 *   5. close  client_fd
 *   6. 回到步驟 1，繼續等待下一條連線
 *
 * 使用 accept mutex 而非 per-connection thread 的理由：
 *   - 避免驚群效應（thundering herd）：多 thread 同時被喚醒爭搶連線
 *   - 保持固定 thread 數量，資源可預測
 * ----------------------------------------------------------------------- */
static void *worker_thread(void *arg)
{
    /* arg 僅用於識別 thread 編號，不須在執行期修改 */
    int thread_id = *((int *)arg);

    /* fix_msg 長度（不含結尾 '\0'）只需計算一次 */
    const size_t msg_len = sizeof(fix_msg) - 1;

    if (!daemon_mode)
    {
        printf("[Thread %d] started\n", thread_id);
    }

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t          client_addr_len = sizeof(client_addr);
        int                client_fd;

        /* ------------------------------------------------------------------
         * accept mutex 模式：同一時間只有一條 thread 可以呼叫 accept()，
         * 避免多條 thread 同時阻塞在 accept() 時造成Thundering Herd Problem。
         * accept() 返回後立刻釋放 mutex，讓其他 thread 可以繼續接受新連線。
         * ------------------------------------------------------------------ */
        if (pthread_mutex_lock(&accept_mutex) != 0)
        {
            perror("pthread_mutex_lock");
            break;
        }

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);

        if (pthread_mutex_unlock(&accept_mutex) != 0)
        {
            perror("pthread_mutex_unlock");
            if (client_fd >= 0)
                close(client_fd);
            break;
        }

        /* accept() 失敗：記錄錯誤後繼續等待，不終止 thread */
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        /* H-3: 設定 send timeout，防止惡意客戶端不讀資料造成 send() 永久阻塞 */
        struct timeval snd_tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

        if (!daemon_mode)
        {
            printf("[Thread %d] client connected\n", thread_id);
        }

        /* ------------------------------------------------------------------
         * partial-send 迴圈：
         *   send() 不保證一次送出全部資料，必須以迴圈持續補送剩餘部分。
         *   send() 回傳 < 0 表示錯誤，跳出迴圈並記錄。
         * ------------------------------------------------------------------ */
        size_t total_sent = 0;

        while (total_sent < msg_len)
        {
            ssize_t sent = send(client_fd,
                                fix_msg + total_sent,
                                msg_len  - total_sent,
                                MSG_NOSIGNAL);  /* L-3: 避免 SIGPIPE 終止程序 */
            if (sent < 0)
            {
                perror("send");
                break;
            }
            total_sent += (size_t)sent;
        }

        close(client_fd);

        if (!daemon_mode)
        {
            printf("[Thread %d] client disconnected\n", thread_id);
        }
    }

    /* 正常情況下不會到達此處（無窮迴圈），僅在 mutex 操作失敗時返回 */
    return NULL;
}

/* -----------------------------------------------------------------------
 * main() 流程：
 *   1. 解析 -d 旗標
 *   2. 建立 server socket、bind、listen
 *   3. 若 -d 旗標：呼叫 daemon()、寫入 PID 檔
 *   4. spawn NUM_THREADS 條 worker thread
 *   5. pthread_join() 等待所有 thread（實際為無窮等待）
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int opt_char;

    /* ------------------------------------------------------------------
     * 命令列解析：只接受 -d（daemon mode）
     * ------------------------------------------------------------------ */
    while ((opt_char = getopt(argc, argv, "d")) != -1)
    {
        switch (opt_char)
        {
            case 'd':
                daemon_mode = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    /* ------------------------------------------------------------------
     * 建立 TCP server socket
     * ------------------------------------------------------------------ */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* SO_REUSEADDR：允許重啟時立即重用同一埠號，避免 TIME_WAIT 阻塞 */
    int reuse_opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_opt, sizeof(reuse_opt)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, BACKLOG) < 0)
    {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("FIX threaded server listening on port %d (%d worker threads)...\n",
           PORT, NUM_THREADS);

    /* ------------------------------------------------------------------
     * Daemon mode：
     *   daemon(0, 0) — chdir("/") 且將 stdin/stdout/stderr 重導向 /dev/null
     *   提示訊息必須在 daemon() 呼叫前印出，因為之後 stdout 已被關閉。
     * ------------------------------------------------------------------ */
    if (daemon_mode)
    {
        printf("Running as daemon (PID will be written to /tmp/fix_server_th.pid)\n");
        fflush(stdout);

        if (daemon(0, 0) < 0)
        {
            perror("daemon");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        /* daemon() 返回後，目前 process 即為 daemon，寫入 PID 檔 */
        FILE *pid_fp = fopen("/tmp/fix_server_th.pid", "w");
        if (pid_fp)
        {
            fprintf(pid_fp, "%d\n", (int)getpid());
            fclose(pid_fp);
        }
    }

    /* ------------------------------------------------------------------
     * 建立 NUM_THREADS 條 worker thread
     *   thread_ids[] 存放各 thread 的編號（1-based），傳給 worker_thread()
     *   供日誌識別使用。陣列必須在 join 完成前保持有效（存放於 stack）。
     * ------------------------------------------------------------------ */
    pthread_t threads[NUM_THREADS];
    int       thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++)
    {
        thread_ids[i] = i + 1; /* 1-based 編號，方便閱讀日誌 */

        if (pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]) != 0)
        {
            perror("pthread_create");
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        if (!daemon_mode)
        {
            printf("Worker thread %d created\n", thread_ids[i]);
        }
    }

    /* ------------------------------------------------------------------
     * 等待所有 worker thread 結束。
     * worker_thread() 設計為無窮迴圈，因此 pthread_join() 實際上永遠阻塞，
     * 此處作為 main() 的保護措施，防止主執行緒提前退出導致 process 結束。
     * ------------------------------------------------------------------ */
    for (int i = 0; i < NUM_THREADS; i++)
    {
        if (pthread_join(threads[i], NULL) != 0)
        {
            perror("pthread_join");
        }
    }

    /* 正常情況下不會到達此處 */
    close(server_fd);
    pthread_mutex_destroy(&accept_mutex);
    return 0;
}

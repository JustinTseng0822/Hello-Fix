/*
 * bench_fix_server_v2.c
 *
 * 功能：使用 epoll 事件迴圈對 fix_server_th_v2（127.0.0.1:5001）
 *       進行高吞吐量連線速率基準測試。
 *
 * 設計動機：
 *   舊版 bench_fix_server_th 採用每條 thread 做一個阻塞式 connect/recv/close
 *   迴圈，在 4 核心機器上到 64 threads 就遇到 CPU 瓶頸，無法繼續提升負載。
 *   本工具改用 N worker threads，每條 thread 以 epoll 同時管理 BATCH_PER_WORKER
 *   個非阻塞 TCP 連線，可以更有效率地飽和伺服器。
 *
 * 架構：
 *   - NUM_WORKERS worker threads（預設 2，保留 2 核給伺服器）
 *   - 每條 worker thread 管理 BATCH_PER_WORKER 個連線（預設 200）
 *   - 總並行連線數：NUM_WORKERS × BATCH_PER_WORKER = 400
 *
 * 每條 worker thread 的事件迴圈：
 *   1. 建立 BATCH_PER_WORKER 個非阻塞 TCP socket
 *   2. 對每個 socket 呼叫非阻塞 connect()（會回傳 EINPROGRESS）
 *   3. 將每個 fd 加入 epoll，監聽 EPOLLOUT（連線完成）
 *   4. epoll_wait() 事件處理：
 *        ST_CONNECTING（等待連線完成）：
 *          - EPOLLOUT 表示連線完成
 *          - 改監聽 EPOLLIN，狀態切換為 ST_RECEIVING
 *        ST_RECEIVING（等待 FIX 資料）：
 *          - EPOLLIN 表示有資料可讀
 *          - recv() 直到找到 "10=" 後的 SOH（0x01）為止
 *          - 成功後累加 atomic 計數器，close() 舊 fd
 *          - 建立新 socket，非阻塞 connect()，加入 epoll → ST_CONNECTING
 *   5. 主程式計時結束後設 g_stop，worker thread 離開迴圈並清理所有 fd
 *
 * 編譯方式（由 Makefile 管理）：
 *   gcc -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *       -o bench_fix_server_v2 bench_fix_server_v2.c -lpthread
 *
 * 用法：
 *   ./bench_fix_server_v2 [duration_sec] [num_workers] [batch_per_worker]
 *   預設：duration=5, workers=2, batch=200
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <time.h>

/* ---- 設定常數 ---- */
#define SERVER_IP        "127.0.0.1"
#define SERVER_PORT      5001
#define BUF_SIZE         4096
#define MAX_EPOLL_EVENTS 256    /* 每次 epoll_wait 最多回傳的事件數 */

/* ---- 連線狀態機 ---- */
typedef enum {
    ST_CONNECTING = 0,  /* connect() 已呼叫，等待 EPOLLOUT（連線完成） */
    ST_RECEIVING  = 1   /* 已連線，等待 EPOLLIN（FIX 資料） */
} conn_state_t;

/* ---- 每個 fd 的連線資訊 ---- */
typedef struct {
    int          fd;            /* socket fd（-1 表示槽位空閒） */
    conn_state_t state;         /* 目前連線狀態 */
    char         buf[BUF_SIZE]; /* 接收緩衝區 */
    ssize_t      buf_len;       /* 緩衝區中已接收的位元組數 */
} conn_t;

/* ---- 全域共享狀態 ---- */

/* 停止旗標：主程式計時結束後設為 1，通知所有 worker thread 退出 */
static _Atomic int  g_stop       = 0;

/* 成功完成（收到完整 FIX 訊息）的連線計數器 */
static _Atomic long g_conn_count = 0;

/* ---- worker thread 參數結構 ---- */
typedef struct {
    int worker_id;       /* worker 編號（用於偵錯） */
    int batch_size;      /* 此 worker 管理的並行連線數 */
} worker_arg_t;

/* ---- 輔助函式 ---- */

/*
 * make_nonblocking()
 *
 * 將 fd 設為非阻塞模式。
 * 回傳 0 成功，-1 失敗。
 */
static int make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * new_connect()
 *
 * 建立一個新的非阻塞 TCP socket 並對 SERVER_IP:SERVER_PORT 發起連線。
 *
 * 成功時：
 *   - 回傳 socket fd
 *   - *out_inprogress 設為 1 表示連線仍在進行（EINPROGRESS）
 *   - *out_inprogress 設為 0 表示連線立即成功（罕見但合法）
 * 失敗時回傳 -1。
 */
static int new_connect(const struct sockaddr_in *server_addr,
                       int *out_inprogress)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    if (make_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    int ret = connect(fd, (const struct sockaddr *)server_addr,
                      sizeof(*server_addr));
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            /* 非阻塞連線正常情況：連線仍在進行 */
            *out_inprogress = 1;
            return fd;
        }
        /* 其他錯誤：真正的失敗 */
        close(fd);
        return -1;
    }

    /* 連線立即完成（loopback 上偶爾發生） */
    *out_inprogress = 0;
    return fd;
}

/*
 * epoll_add()
 *
 * 將 fd 加入 epoll 實例，監聽指定事件，並以 conn 作為 user data。
 * 回傳 0 成功，-1 失敗。
 */
static int epoll_add(int epfd, int fd, uint32_t events, conn_t *conn)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = conn;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

/*
 * epoll_mod()
 *
 * 修改 epoll 對 fd 的監聽事件。
 * 回傳 0 成功，-1 失敗。
 */
static int epoll_mod(int epfd, int fd, uint32_t events, conn_t *conn)
{
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = conn;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

/*
 * check_fix_complete()
 *
 * 在緩衝區 buf（長度 len）中搜尋 FIX 訊息結尾標記：
 * "10=" 欄位後緊接著一個 SOH（0x01）byte。
 *
 * 回傳 1 表示訊息完整，0 表示尚未完整。
 */
static int check_fix_complete(const char *buf, ssize_t len)
{
    /* 確保有 '\0' 可以給 strstr 使用；呼叫端必須在 buf[len] 已放 '\0' */
    const char *cs = strstr(buf, "10=");
    if (cs == NULL)
        return 0;
    /* 在 "10=" 之後找 SOH */
    const char *p = cs + 3;
    const char *end = buf + len;
    while (p < end) {
        if ((unsigned char)*p == 0x01)
            return 1;
        p++;
    }
    return 0;
}

/*
 * conn_reset()
 *
 * 重置 conn 槽位狀態（不動 fd，fd 由呼叫端管理）。
 */
static void conn_reset(conn_t *c)
{
    c->fd      = -1;
    c->state   = ST_CONNECTING;
    c->buf_len = 0;
    c->buf[0]  = '\0';
}

/*
 * start_new_conn()
 *
 * 對槽位 c 建立新的非阻塞連線並加入 epoll。
 * 成功回傳 0，失敗回傳 -1。
 */
static int start_new_conn(conn_t *c, int epfd,
                          const struct sockaddr_in *server_addr)
{
    int inprogress = 0;
    int fd = new_connect(server_addr, &inprogress);
    if (fd < 0)
        return -1;

    c->fd      = fd;
    c->buf_len = 0;
    c->buf[0]  = '\0';

    if (inprogress) {
        /* 連線進行中：等 EPOLLOUT 通知連線完成 */
        c->state = ST_CONNECTING;
        if (epoll_add(epfd, fd, EPOLLOUT, c) < 0) {
            close(fd);
            c->fd = -1;
            return -1;
        }
    } else {
        /* 連線立即成功：直接監聽 EPOLLIN */
        c->state = ST_RECEIVING;
        if (epoll_add(epfd, fd, EPOLLIN, c) < 0) {
            close(fd);
            c->fd = -1;
            return -1;
        }
    }

    return 0;
}

/* ---- worker thread 主函式 ---- */

/*
 * worker_thread()
 *
 * 每條 worker thread 維護一個 epoll 實例，同時管理 batch_size 個連線。
 * 事件迴圈持續到 g_stop 被設為非零且所有連線已清理為止。
 */
static void *worker_thread(void *arg)
{
    worker_arg_t *warg = (worker_arg_t *)arg;
    int batch_size = warg->batch_size;

    /* 建立伺服器位址（每條 thread 獨立建立，避免共用狀態） */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "[worker %d] inet_pton failed\n", warg->worker_id);
        return NULL;
    }

    /* 建立 epoll 實例 */
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return NULL;
    }

    /* 配置連線槽位陣列 */
    conn_t *conns = calloc((size_t)batch_size, sizeof(conn_t));
    if (conns == NULL) {
        fprintf(stderr, "[worker %d] calloc failed\n", warg->worker_id);
        close(epfd);
        return NULL;
    }

    /* 初始化所有槽位並發起初始連線 */
    int active = 0; /* 目前活躍（在 epoll 中）的連線數 */
    for (int i = 0; i < batch_size; i++) {
        conn_reset(&conns[i]);
        if (start_new_conn(&conns[i], epfd, &server_addr) == 0) {
            active++;
        }
        /* 若 start_new_conn 失敗，略過此槽位；後續迴圈不會再嘗試補充，
         * 但仍能繼續測試（只是並行度略低）。
         * 實際上 loopback connect 幾乎不會在初始階段失敗。*/
    }

    /* epoll 事件緩衝區 */
    struct epoll_event events[MAX_EPOLL_EVENTS];

    /*
     * 主事件迴圈
     *
     * 條件：g_stop 未被設定，或仍有活躍連線需要清理。
     * 當 g_stop 被設定後，不再建立新連線，只等現有連線處理完畢後離開。
     */
    while (active > 0 ||
           !atomic_load_explicit(&g_stop, memory_order_relaxed)) {

        /* 若停止旗標已設且沒有活躍連線，直接離開 */
        if (atomic_load_explicit(&g_stop, memory_order_relaxed) &&
            active == 0)
            break;

        /* dead slot retry：連線失敗的槽位在每輪事件迴圈中重試，
         * 確保在低 BACKLOG 伺服器（如 fix_server BACKLOG=5）下
         * 暫時 ECONNREFUSED 的槽位能持續嘗試，不永久停擺。 */
        if (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
            for (int i = 0; i < batch_size; i++) {
                if (conns[i].fd < 0) {
                    if (start_new_conn(&conns[i], epfd, &server_addr) == 0)
                        active++;
                }
            }
        }

        /* epoll_wait 逾時 50ms，讓迴圈能定期檢查 g_stop */
        int nev = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 50);
        if (nev < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nev; i++) {
            conn_t *c = (conn_t *)events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (c->state == ST_CONNECTING) {
                /*
                 * ST_CONNECTING：連線完成通知（EPOLLOUT）
                 *
                 * 需確認 SO_ERROR 無誤，才算真正連線成功。
                 * 若有錯誤（例如 connection refused）則關閉並
                 * 在非停止狀態下重新發起連線。
                 */
                if (ev & (EPOLLERR | EPOLLHUP)) {
                    /* 連線失敗 */
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    active--;

                    if (!atomic_load_explicit(&g_stop,
                                             memory_order_relaxed)) {
                        conn_reset(c);
                        if (start_new_conn(c, epfd, &server_addr) == 0)
                            active++;
                    } else {
                        c->fd = -1;
                    }
                    continue;
                }

                if (ev & EPOLLOUT) {
                    /* 確認連線是否真正成功 */
                    int so_err = 0;
                    socklen_t so_len = sizeof(so_err);
                    getsockopt(c->fd, SOL_SOCKET, SO_ERROR,
                               &so_err, &so_len);

                    if (so_err != 0) {
                        /* 連線失敗（例如 ECONNREFUSED） */
                        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                        close(c->fd);
                        active--;

                        if (!atomic_load_explicit(&g_stop,
                                                 memory_order_relaxed)) {
                            conn_reset(c);
                            if (start_new_conn(c, epfd, &server_addr) == 0)
                                active++;
                        } else {
                            c->fd = -1;
                        }
                        continue;
                    }

                    /* 連線成功：改監聽 EPOLLIN 等待 FIX 資料 */
                    c->state = ST_RECEIVING;
                    epoll_mod(epfd, c->fd, EPOLLIN, c);
                }

            } else { /* ST_RECEIVING */
                /*
                 * ST_RECEIVING：有資料可讀（EPOLLIN）或錯誤
                 */
                if (ev & (EPOLLERR | EPOLLHUP)) {
                    /* 連線異常中斷，不計入成功 */
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    active--;

                    if (!atomic_load_explicit(&g_stop,
                                             memory_order_relaxed)) {
                        conn_reset(c);
                        if (start_new_conn(c, epfd, &server_addr) == 0)
                            active++;
                    } else {
                        c->fd = -1;
                    }
                    continue;
                }

                /*
                 * ST_RECEIVING：EPOLLIN 優先於 EPOLLERR 處理。
                 * 原因：伺服器送出資料後以正常 FIN 關閉，client 可能同時
                 * 收到 EPOLLIN 與 EPOLLHUP，必須先讀完資料才判斷錯誤。
                 */
                if (ev & EPOLLIN) {
                    int done = 0;
                    int error = 0;

                    while (!done) {
                        ssize_t space = (ssize_t)(BUF_SIZE - 1) - c->buf_len;
                        if (space <= 0) {
                            error = 1;
                            break;
                        }

                        ssize_t n = recv(c->fd,
                                         c->buf + c->buf_len,
                                         (size_t)space, 0);
                        if (n < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;
                            error = 1;
                            break;
                        }
                        if (n == 0) {
                            /* FIN received：訊息若已完整則計入成功 */
                            break;
                        }

                        c->buf_len += n;
                        c->buf[c->buf_len] = '\0';

                        if (check_fix_complete(c->buf, c->buf_len)) {
                            done = 1;
                        }
                    }

                    if (done) {
                        atomic_fetch_add_explicit(&g_conn_count, 1L,
                                                  memory_order_relaxed);

                        /*
                         * 以 SO_LINGER=0 關閉（送 RST）：
                         *   伺服器此時在 FIN_WAIT_2 等待 client FIN，
                         *   RST 使伺服器直接進入 CLOSED，雙方均無 TIME_WAIT。
                         */
                        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
                        setsockopt(c->fd, SOL_SOCKET, SO_LINGER,
                                   &lg, sizeof(lg));

                        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                        close(c->fd);
                        active--;

                        if (!atomic_load_explicit(&g_stop,
                                                 memory_order_relaxed)) {
                            conn_reset(c);
                            if (start_new_conn(c, epfd, &server_addr) == 0)
                                active++;
                        } else {
                            c->fd = -1;
                        }

                    } else if (error) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                        close(c->fd);
                        active--;

                        if (!atomic_load_explicit(&g_stop,
                                                 memory_order_relaxed)) {
                            conn_reset(c);
                            if (start_new_conn(c, epfd, &server_addr) == 0)
                                active++;
                        } else {
                            c->fd = -1;
                        }
                    }
                    /* else: 資料不完整，等下次 EPOLLIN */

                } else if (ev & (EPOLLERR | EPOLLHUP)) {
                    /* 無資料可讀，連線異常中斷 */
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    active--;

                    if (!atomic_load_explicit(&g_stop,
                                             memory_order_relaxed)) {
                        conn_reset(c);
                        if (start_new_conn(c, epfd, &server_addr) == 0)
                            active++;
                    } else {
                        c->fd = -1;
                    }
                }
            }
        } /* end for each event */
    } /* end while */

    /* 清理：關閉仍在 epoll 中的 fd */
    for (int i = 0; i < batch_size; i++) {
        if (conns[i].fd >= 0) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, conns[i].fd, NULL);
            close(conns[i].fd);
            conns[i].fd = -1;
        }
    }

    free(conns);
    close(epfd);
    return NULL;
}

/* ---- 主程式 ---- */
int main(int argc, char *argv[])
{
    int duration_sec      = 5;
    int num_workers       = 2;
    int batch_per_worker  = 200;

    /* 解析命令列參數 */
    if (argc >= 2) {
        long v = strtol(argv[1], NULL, 10);
        if (v > 0 && v <= 3600) {
            duration_sec = (int)v;
        } else {
            fprintf(stderr, "Invalid duration_sec: %s (must be 1-3600)\n",
                    argv[1]);
            return EXIT_FAILURE;
        }
    }
    if (argc >= 3) {
        long v = strtol(argv[2], NULL, 10);
        if (v > 0 && v <= 256) {
            num_workers = (int)v;
        } else {
            fprintf(stderr,
                    "Invalid num_workers: %s (must be 1-256)\n", argv[2]);
            return EXIT_FAILURE;
        }
    }
    if (argc >= 4) {
        long v = strtol(argv[3], NULL, 10);
        if (v > 0 && v <= 10000) {
            batch_per_worker = (int)v;
        } else {
            fprintf(stderr,
                    "Invalid batch_per_worker: %s (must be 1-10000)\n",
                    argv[3]);
            return EXIT_FAILURE;
        }
    }

    int total_conns = num_workers * batch_per_worker;

    printf("Benchmarking fix_server_th_v2 (%s:%d) with epoll\n",
           SERVER_IP, SERVER_PORT);
    printf("Workers: %d | Batch/worker: %d | Total concurrent: %d | "
           "Duration: %ds\n",
           num_workers, batch_per_worker, total_conns, duration_sec);
    fflush(stdout);

    /* 配置 worker thread 陣列與參數陣列 */
    pthread_t    *threads = malloc((size_t)num_workers * sizeof(pthread_t));
    worker_arg_t *args    = malloc((size_t)num_workers * sizeof(worker_arg_t));
    if (!threads || !args) {
        fprintf(stderr, "malloc failed\n");
        free(threads);
        free(args);
        return EXIT_FAILURE;
    }

    /* 初始化全域 atomic 變數 */
    atomic_store(&g_stop, 0);
    atomic_store(&g_conn_count, 0L);

    /* 記錄實際開始時間（CLOCK_MONOTONIC 避免系統時間跳躍） */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* 啟動所有 worker thread */
    for (int i = 0; i < num_workers; i++) {
        args[i].worker_id  = i;
        args[i].batch_size = batch_per_worker;
        int ret = pthread_create(&threads[i], NULL, worker_thread, &args[i]);
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

    /* 通知所有 worker thread 停止 */
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);

    /* 等待所有 worker thread 結束 */
    for (int i = 0; i < num_workers; i++)
        pthread_join(threads[i], NULL);

    /* 記錄結束時間，計算實際經過時間 */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (double)(ts_end.tv_sec  - ts_start.tv_sec) +
                     (double)(ts_end.tv_nsec - ts_start.tv_nsec) * 1e-9;

    long   total_conn   = atomic_load(&g_conn_count);
    double conn_per_sec = (elapsed > 0.0)
                          ? (double)total_conn / elapsed
                          : 0.0;

    /* 輸出結果 */
    printf("Total connections : %ld\n", total_conn);
    printf("Elapsed           : %.3f sec\n", elapsed);
    printf("Connections/sec   : %.1f\n", conn_per_sec);

    free(threads);
    free(args);
    return EXIT_SUCCESS;
}

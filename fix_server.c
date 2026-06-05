#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#define PORT 5001
#define BACKLOG 5


int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt_char;

    /* Parse command-line options. Only -d (daemon mode) is recognised. */
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

    int server_fd, client_fd;

    /* [fix] 分開宣告 server_addr 與 client_addr，避免 accept() 中混用同一變數 */
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    /* FIX message with SOH (0x01) */
    const char fix_msg[] =
        "8=FIX.4.2\x01"
        "9=51\x01"
        "35=0\x01"
        "49=200201\x01"
        "56=TWSE\x01"
        "34=593\x01"
        "52=20090803-13:37:47\x01"
        "10=233\x01";

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) 
	{
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    /* [fix] 檢查 setsockopt() 回傳值 */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) 
	{
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

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

   printf("FIX server listening on port %d...\n", PORT);
   if (daemon_mode)
	{
        /*
         * 使用 daemon(0, 0) 一步完成 daemonize：
         *   第一個參數 0 — 呼叫 chdir("/")，釋放對掛載點的參照
         *   第二個參數 0 — 將 stdin/stdout/stderr 重導向至 /dev/null
         *
         * daemon() 在成功後，目前的 process 本身即成為背景 daemon；
         * 此後 stdout 已被重導向，因此提示訊息必須在呼叫前印出。
         */
        printf("Running as daemon\n");
		printf("cat /tmp/fix_server.pid to get daemon pid\n");
        fflush(stdout);
        if (daemon(0, 0) < 0)
		{
            perror("daemon");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
        /* daemon() 返回後，目前 process 即為 daemon，getpid() 取得其 PID */
        FILE *pid_fp = fopen("/tmp/fix_server.pid", "w");
        if (pid_fp)
		{
            fprintf(pid_fp, "%d\n", (int)getpid());
            fclose(pid_fp);
        }    		
    }	

    while (1) 
	{
        /* 使用 client_addr / client_addr_len，不再與 server_addr 混用 */
        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_addr,
                           &client_addr_len);
        if (client_fd < 0)
		{
            perror("accept");
            continue;
        }

        /* H-3: 設定 send timeout，防止惡意客戶端不讀資料造成 send() 永久阻塞 */
        struct timeval snd_tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

        /* daemon 模式下 stdout 已重導向 /dev/null，只在非 daemon 模式列印 */
        if (!daemon_mode) 
		{
            printf("Client connected\n");
        }

        /*
         * partial send 處理：
         * 以迴圈確保所有資料都送出，並檢查 send() 每次的回傳值。
         */
        size_t msg_len   = sizeof(fix_msg) - 1; /* 不含結尾 '\0' */
        size_t total_sent = 0;

        while (total_sent < msg_len) 
		{
            ssize_t sent = send(client_fd,
                                fix_msg + total_sent,
                                msg_len - total_sent,
                                MSG_NOSIGNAL);  /* L-3: 避免 SIGPIPE 終止程序 */
            if (sent < 0) {
                perror("send");
                break;
            }
            total_sent += (size_t)sent;
        }

        close(client_fd);

        /* daemon 模式下不列印，理由同上 */
        if (!daemon_mode) 
		{
            printf("Client disconnected\n");
        }
    }

    close(server_fd);
    return 0;
}

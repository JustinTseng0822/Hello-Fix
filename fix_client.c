#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SERVER_IP "127.0.0.1"
#define PORT 5001
#define BUF_SIZE 2048

/*
 * len 參數改為 size_t，回傳型別改為 unsigned int
 * 避免有號/無號混用與潛在的負值傳入問題
 */
unsigned int calculate_checksum(const char *msg, size_t len)
{
    unsigned int sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += (unsigned char)msg[i];

    return sum % 256;
}

/* main() 改為標準的 int main(void) */
int main(void)
{
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUF_SIZE];

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) 
	{
        perror("socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);

    /* 檢查 inet_pton() 回傳值 */
    int pton_ret = inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    if (pton_ret == 0) 
	{
        fprintf(stderr, "inet_pton: invalid address \"%s\"\n", SERVER_IP);
        close(sock);
        exit(EXIT_FAILURE);
    } 
	else 
	if (pton_ret < 0) 
	{
        perror("inet_pton");
        close(sock);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
	{
        perror("connect");
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server\n");

    /*
     * partial recv 修復：
     * 以迴圈持續讀取，直到在緩衝區中找到 "10=" 後的第一個 SOH 為止，
     * 代表 FIX CheckSum 欄位已完整接收。
     * 每次最多讀 BUF_SIZE-1 個位元組，並在結尾補 '\0' 防止 buffer over-read。
     */
    ssize_t total = 0;

    while (total < (ssize_t)(BUF_SIZE - 1)) 
	{
        /* 傳入 BUF_SIZE-1 保留一個位元組給 '\0' */
        ssize_t n = recv(sock, buffer + total, (size_t)(BUF_SIZE - 1) - (size_t)total, 0);
        if (n < 0) 
		{
            perror("recv");
            close(sock);
            return 1;
        }
        if (n == 0) 
		{
            /* 對端關閉連線 */
            break;
        }
        total += n;
        /* 每次收到資料後立刻補 '\0'，防止後續 strstr 越界 */
        buffer[total] = '\0';

        /* 找到 "10=" 欄位後，再確認其後的第一個 SOH 已到達，則訊息完整 */
        char *checksum_field = strstr(buffer, "10=");
        if (checksum_field != NULL) 
		{
            /* SOH (0x01) 是 FIX 欄位分隔符，CheckSum 值後緊跟一個 SOH */
            char *soh_after_cs = strchr(checksum_field + 3, 0x01);
            if (soh_after_cs != NULL) 
			{
                /* CheckSum 欄位（含尾端 SOH）已完整接收，結束讀取迴圈 */
                break;
            }
        }
    }

    if (total <= 0) 
	{
        printf("Receive error\n");
        close(sock);
        return 1;
    }

    /* 找到 "10=" 欄位 */
    char *checksum_field = strstr(buffer, "10=");
    if (!checksum_field) 
	{
        printf("Invalid FIX message: No CheckSum field\n");
        close(sock);
        return 1;
    }

    /* 計算 checksum 涵蓋範圍：從訊息開頭到 "10=" 之前 */
    size_t checksum_pos = (size_t)(checksum_field - buffer);

    unsigned int calculated = calculate_checksum(buffer, checksum_pos);

    /*
     *  atoi() 換成 strtol()，加上範圍驗證（FIX CheckSum 合法值 0-255）
     *  並以 endptr 判斷是否有非數字字元
     */
    char *endptr = NULL;
    errno = 0;
    long received_long = strtol(checksum_field + 3, &endptr, 10);
    if (errno != 0 || endptr == checksum_field + 3) 
	{
        fprintf(stderr, "Invalid checksum value in message\n");
        close(sock);
        return 1;
    }
    if (received_long < 0 || received_long > 255) 
	{
        fprintf(stderr, "Checksum value out of range: %ld\n", received_long);
        close(sock);
        return 1;
    }
    unsigned int received = (unsigned int)received_long;

    if (calculated != received) 
	{
        printf("Checksum ERROR!\n");
        printf("Calculated: %03u\n", calculated);
        printf("Received  : %03u\n", received);
        close(sock);
        return 1;
    }

    printf("Checksum OK (%03u)\n", calculated);

    /* 印出 message，將 SOH 可視化為 <SOH> */
    printf("Received FIX message:\n");
    for (ssize_t i = 0; i < total; i++) 
	{
        if (buffer[i] == 0x01)
            printf("<SOH>");
        else
            putchar(buffer[i]);
    }
    printf("\n");

    close(sock);
    return 0;
}

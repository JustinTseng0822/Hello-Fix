rm=/bin/rm -f
CC= gcc
DEFS=
INCLUDES=  -I.
LIBS=

DEFINES= $(INCLUDES) $(DEFS) -DSYS_UNIX=1
# [fix] 加入 -Wall -Wextra -std=c11 以啟用完整警告並指定 C11 標準
#CFLAGS= -g -Wall -Wextra -std=c11 $(DEFINES)
CFLAGS= -g -Wall -Wextra $(DEFINES)

# --- fix_client ---
CLIENT_PROGNAME= fix_client
CLIENT_SRCS= fix_client.c
CLIENT_OBJS= fix_client.o

# --- fix_server ---
SERVER_PROGNAME= fix_server
SERVER_SRCS= fix_server.c
SERVER_OBJS= fix_server.o

# --- test_unit ---
TEST_PROGNAME= test_unit
TEST_SRCS= test_unit.c
TEST_OBJS= test_unit.o

# --- fix_server_th (pthread 多 thread 版本) ---
SERVER_TH_PROGNAME= fix_server_th
SERVER_TH_SRCS= fix_server_th.c
SERVER_TH_OBJS= fix_server_th.o
# pthread 需要額外連結 -lpthread
SERVER_TH_LIBS= -lpthread

# --- bench_fix_server_th (fix_server_th 連線速率基準測試) ---
BENCH_PROGNAME= bench_fix_server_th
BENCH_SRCS= bench_fix_server_th.c
BENCH_OBJS= bench_fix_server_th.o
BENCH_LIBS= -lpthread

# --- fix_server_th_v2 (SO_REUSEPORT 高效能多 thread 版本) ---
SERVER_TH_V2_PROGNAME= fix_server_th_v2
SERVER_TH_V2_SRCS= fix_server_th_v2.c
SERVER_TH_V2_OBJS= fix_server_th_v2.o
SERVER_TH_V2_LIBS= -lpthread

# --- bench_fix_server_v2 (epoll 高吞吐量基準測試，對應 fix_server_th_v2) ---
BENCH_V2_PROGNAME= bench_fix_server_v2
BENCH_V2_SRCS= bench_fix_server_v2.c
BENCH_V2_OBJS= bench_fix_server_v2.o
BENCH_V2_LIBS= -lpthread

# [fix] 加入 .PHONY 宣告，避免 all/clean/test/bench 與同名檔案衝突
.PHONY: all clean test bench bench_v2

.c.o:
	$(rm) $@
	$(CC) $(CFLAGS) -c $*.c

all: $(CLIENT_PROGNAME) $(SERVER_PROGNAME) $(TEST_PROGNAME) $(SERVER_TH_PROGNAME) $(BENCH_PROGNAME) $(SERVER_TH_V2_PROGNAME) $(BENCH_V2_PROGNAME)

$(CLIENT_PROGNAME): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $(CLIENT_PROGNAME) $(CLIENT_OBJS) $(LIBS)

$(SERVER_PROGNAME): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $(SERVER_PROGNAME) $(SERVER_OBJS) $(LIBS)

$(TEST_PROGNAME): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $(TEST_PROGNAME) $(TEST_OBJS) $(LIBS)

# fix_server_th：需加 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE 以啟用
#   pthread 與 daemon() 的 POSIX/BSD 介面，並連結 -lpthread
$(SERVER_TH_PROGNAME): $(SERVER_TH_OBJS)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -o $(SERVER_TH_PROGNAME) $(SERVER_TH_OBJS) $(SERVER_TH_LIBS)

fix_server_th.o: fix_server_th.c
	$(rm) fix_server_th.o
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -c fix_server_th.c

# bench_fix_server_th：連線速率基準測試，需 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
#   以啟用 pthread、clock_gettime 及 sched_yield 等 POSIX 介面，並連結 -lpthread
$(BENCH_PROGNAME): $(BENCH_OBJS)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -o $(BENCH_PROGNAME) $(BENCH_OBJS) $(BENCH_LIBS)

bench_fix_server_th.o: bench_fix_server_th.c
	$(rm) bench_fix_server_th.o
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -c bench_fix_server_th.c

# fix_server_th_v2：SO_REUSEPORT 高效能版，需加 -D_POSIX_C_SOURCE=200809L
#   -D_DEFAULT_SOURCE 以啟用 daemon() 及 pthread 介面，並連結 -lpthread。
#   注意：_GNU_SOURCE 已定義在原始檔頂端（pthread_setaffinity_np / CPU_SET）。
$(SERVER_TH_V2_PROGNAME): $(SERVER_TH_V2_OBJS)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -o $(SERVER_TH_V2_PROGNAME) $(SERVER_TH_V2_OBJS) $(SERVER_TH_V2_LIBS)

fix_server_th_v2.o: fix_server_th_v2.c
	$(rm) fix_server_th_v2.o
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -c fix_server_th_v2.c

# bench_fix_server_v2：epoll 高吞吐量基準測試，需 -D_POSIX_C_SOURCE=200809L
#   -D_DEFAULT_SOURCE 以啟用 pthread、clock_gettime 等 POSIX 介面，並連結 -lpthread
$(BENCH_V2_PROGNAME): $(BENCH_V2_OBJS)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -o $(BENCH_V2_PROGNAME) $(BENCH_V2_OBJS) $(BENCH_V2_LIBS)

bench_fix_server_v2.o: bench_fix_server_v2.c
	$(rm) bench_fix_server_v2.o
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
	      -c bench_fix_server_v2.c

# bench target：只編譯舊版阻塞式基準測試
bench: $(BENCH_PROGNAME)

# bench_v2 target：只編譯 epoll 基準測試
bench_v2: $(BENCH_V2_PROGNAME)

# test target：編譯並立即執行單元測試
test: $(TEST_PROGNAME)
	./$(TEST_PROGNAME)

clean:
	$(rm) $(CLIENT_OBJS) $(SERVER_OBJS) $(CLIENT_PROGNAME) $(SERVER_PROGNAME) \
	      $(TEST_OBJS) $(TEST_PROGNAME) \
	      $(SERVER_TH_OBJS) $(SERVER_TH_PROGNAME) \
	      $(BENCH_OBJS) $(BENCH_PROGNAME) \
	      $(SERVER_TH_V2_OBJS) $(SERVER_TH_V2_PROGNAME) \
	      $(BENCH_V2_OBJS) $(BENCH_V2_PROGNAME) \
	      core *~

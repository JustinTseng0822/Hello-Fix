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

# [fix] 加入 .PHONY 宣告，避免 all/clean/test 與同名檔案衝突
.PHONY: all clean test

.c.o:
	$(rm) $@
	$(CC) $(CFLAGS) -c $*.c

all: $(CLIENT_PROGNAME) $(SERVER_PROGNAME) $(TEST_PROGNAME) $(SERVER_TH_PROGNAME)

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

# test target：編譯並立即執行單元測試
test: $(TEST_PROGNAME)
	./$(TEST_PROGNAME)

clean:
	$(rm) $(CLIENT_OBJS) $(SERVER_OBJS) $(CLIENT_PROGNAME) $(SERVER_PROGNAME) \
	      $(TEST_OBJS) $(TEST_PROGNAME) \
	      $(SERVER_TH_OBJS) $(SERVER_TH_PROGNAME) core *~

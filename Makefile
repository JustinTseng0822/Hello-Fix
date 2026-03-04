rm=/bin/rm -f
CC= gcc
DEFS=
INCLUDES=  -I.
LIBS=

DEFINES= $(INCLUDES) $(DEFS) -DSYS_UNIX=1
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

# 加入 .PHONY 宣告，避免 all/clean/test 與同名檔案衝突
.PHONY: all clean test

.c.o:
	$(rm) $@
	$(CC) $(CFLAGS) -c $*.c

all: $(CLIENT_PROGNAME) $(SERVER_PROGNAME)

$(CLIENT_PROGNAME): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $(CLIENT_PROGNAME) $(CLIENT_OBJS) $(LIBS)

$(SERVER_PROGNAME): $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $(SERVER_PROGNAME) $(SERVER_OBJS) $(LIBS)

$(TEST_PROGNAME): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $(TEST_PROGNAME) $(TEST_OBJS) $(LIBS)

# test target：編譯並立即執行單元測試
test: $(TEST_PROGNAME)
	./$(TEST_PROGNAME)

clean:
	$(rm) $(CLIENT_OBJS) $(SERVER_OBJS) $(CLIENT_PROGNAME) $(SERVER_PROGNAME) \
	      $(TEST_OBJS) $(TEST_PROGNAME) core *~

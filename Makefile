# make        → build/server, build/backend
# make test   → tests/test_*.c를 각각 빌드 · 실행 (하나라도 실패하면 종료 코드 1)
# make asan   → ASan + UBSan으로 서버와 테스트를 빌드하고 테스트까지 실행
# make clean

CFLAGS     := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread -I. -MMD -MP
LDFLAGS    := -pthread
OPT        := -O2
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

BUILD      := build
COMMON_SRC := common/buf.c common/net.c common/http_parser.c common/http_response.c
# main을 뺀 server 모듈. 테스트도 이것과 함께 링크한다
LIB_SRC    := $(COMMON_SRC) server/mime.c server/static_handler.c server/config.c server/router.c \
              server/proxy.c server/thread_pool.c server/conn.c server/standby.c \
              server/balance.c server/gate.c
SERVER_SRC := server/main.c $(LIB_SRC)
# 백엔드는 common/만 쓴다 (server/ 모듈을 링크하지 않음)
BACKEND_SRC := backend/main.c $(COMMON_SRC)
TEST_SRC   := $(wildcard tests/test_*.c)

SERVER_OBJ := $(SERVER_SRC:%.c=$(BUILD)/obj/%.o)
BACKEND_OBJ := $(BACKEND_SRC:%.c=$(BUILD)/obj/%.o)
COMMON_OBJ := $(LIB_SRC:%.c=$(BUILD)/obj/%.o)
TESTS      := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%)

ASAN_OBJ        := $(SERVER_SRC:%.c=$(BUILD)/asan/obj/%.o)
ASAN_BACKEND_OBJ := $(BACKEND_SRC:%.c=$(BUILD)/asan/obj/%.o)
ASAN_COMMON_OBJ := $(LIB_SRC:%.c=$(BUILD)/asan/obj/%.o)
ASAN_TESTS      := $(TEST_SRC:tests/%.c=$(BUILD)/asan/tests/%)

.PHONY: all asan test clean
# 테스트 .o를 중간 파일로 보고 지우면 매번 다시 컴파일하므로 남겨 둔다
.SECONDARY:

all: $(BUILD)/server $(BUILD)/backend

$(BUILD)/server: $(SERVER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/backend: $(BACKEND_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/tests/%: $(BUILD)/obj/tests/%.o $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OPT) -c $< -o $@

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

asan: $(BUILD)/asan/server $(BUILD)/asan/backend $(ASAN_TESTS)
	@for t in $(ASAN_TESTS); do echo "== $$t"; $$t || exit 1; done

$(BUILD)/asan/server: $(ASAN_OBJ)
	$(CC) $(LDFLAGS) -fsanitize=address,undefined -o $@ $^

$(BUILD)/asan/backend: $(ASAN_BACKEND_OBJ)
	$(CC) $(LDFLAGS) -fsanitize=address,undefined -o $@ $^

$(BUILD)/asan/tests/%: $(BUILD)/asan/obj/tests/%.o $(ASAN_COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -fsanitize=address,undefined -o $@ $^

$(BUILD)/asan/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(SERVER_OBJ:.o=.d) $(ASAN_OBJ:.o=.d) $(BACKEND_OBJ:.o=.d) $(ASAN_BACKEND_OBJ:.o=.d) \
         $(TEST_SRC:%.c=$(BUILD)/obj/%.d) \
         $(TEST_SRC:%.c=$(BUILD)/asan/obj/%.d)

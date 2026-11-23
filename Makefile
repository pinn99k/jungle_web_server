# make        → build/server
# make asan   → build/asan/server (ASan + UBSan. 스텝 2부터는 테스트까지 실행)
# make test   → 단위 테스트 (스텝 2부터)
# make clean

CFLAGS     := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread -I. -MMD -MP
LDFLAGS    := -pthread
OPT        := -O2
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

BUILD      := build
SERVER_SRC := server/main.c common/buf.c common/net.c

SERVER_OBJ := $(SERVER_SRC:%.c=$(BUILD)/obj/%.o)
ASAN_OBJ   := $(SERVER_SRC:%.c=$(BUILD)/asan/obj/%.o)

.PHONY: all asan test clean

all: $(BUILD)/server

$(BUILD)/server: $(SERVER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OPT) -c $< -o $@

asan: $(BUILD)/asan/server

$(BUILD)/asan/server: $(ASAN_OBJ)
	$(CC) $(LDFLAGS) -fsanitize=address,undefined -o $@ $^

$(BUILD)/asan/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -c $< -o $@

test:
	@echo "단위 테스트는 스텝 2부터"

clean:
	rm -rf $(BUILD)

-include $(SERVER_OBJ:.o=.d) $(ASAN_OBJ:.o=.d)

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -libverbs

BIN := bin/rdma_check
SRC := rdma_check.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf bin

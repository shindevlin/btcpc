CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -g -fsanitize=address,undefined \
          -I. -I.. -I../protocol -I../crypto -D_POSIX_C_SOURCE=199309L
# TweetNaCl uses intentional signed-integer wrapping for field arithmetic;
# compile it without UBSan to avoid false positives.
CFLAGS_NACL = -std=c11 -g -I. -I.. -I../protocol -I../crypto -D_POSIX_C_SOURCE=199309L

SRCS_PROTO  = ../protocol/btcpc_protocol.c
SRCS_EXP    = ../protocol/btcpc_protocol.c

.PHONY: test clean

test: test_protocol test_expansion
	@echo ""
	@./test_protocol
	@./test_expansion
	@echo "All test suites passed."

tweetnacl.o: ../crypto/tweetnacl.c
	$(CC) $(CFLAGS_NACL) -c -o $@ $<

ed25519.o: ../crypto/ed25519.c
	$(CC) $(CFLAGS_NACL) -c -o $@ $<

test_protocol: test_protocol.c $(SRCS_PROTO) tweetnacl.o ed25519.o
	$(CC) $(CFLAGS) -o $@ test_protocol.c $(SRCS_PROTO) tweetnacl.o ed25519.o

test_expansion: test_expansion.c $(SRCS_EXP) tweetnacl.o ed25519.o
	$(CC) $(CFLAGS) -o $@ test_expansion.c $(SRCS_EXP) tweetnacl.o ed25519.o

clean:
	rm -f test_protocol test_expansion tweetnacl.o ed25519.o

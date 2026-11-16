# Host test for the ignition bench box. Compiles the exact sources that go to the
# FK743 (si5351.c vendored from afiskon/stm32-si5351 MIT, ignition_core.c) against
# the virtual-clock stubs in main_host.c.

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -I.

ign_host_test: si5351.c ignition_core.c main_host.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test: ign_host_test
	./ign_host_test

clean:
	rm -f ign_host_test

.PHONY: test clean

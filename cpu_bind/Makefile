# cpu_bind/Makefile — 独立 CPU 绑定模块编译 & 测试

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -g -D_DEFAULT_SOURCE -I..

.PHONY: all test clean

all: test_cpu_bind

cpu_bind.o: cpu_bind.c cpu_bind.h topology.h ../vec.h
	$(CC) $(CFLAGS) -c cpu_bind.c -o cpu_bind.o

test_cpu_bind: test_cpu_bind.c cpu_bind.o cpu_bind.h topology.h ../vec.h
	$(CC) $(CFLAGS) -o test_cpu_bind test_cpu_bind.c cpu_bind.o ../vec.o

test: test_cpu_bind
	./test_cpu_bind

clean:
	rm -f *.o test_cpu_bind

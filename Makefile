PREFIX?=/usr/local
PREFIX_LOCAL=~
GLIBCFLAGS=-D_XOPEN_SOURCE=500 -D__STRICT_ANSI__
CPPFLAGS+=$(GLIBCFLAGS)
CFLAGS?=-pedantic -ansi -Wall -g -O0 -std=c11
OBJECTS=main.o \
	server.o \
	server_start.o \
	client.o \
	msgdump.o \
	jobs.o \
	execute.o \
	msg.o \
	mail.o \
	error.o \
	signals.o \
	list.o \
	print.o \
	info.o \
	env.o \
	tail.o\
	gpu.o
TARGET=ts
INSTALL=install -c

GIT_REPO=$(shell git rev-parse --is-inside-work-tree)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $(TARGET) $^ -L$(CUDA_HOME)/lib64 -L$(CUDA_HOME)/lib64/stubs -I$(CUDA_HOME)/include -lpthread -lcudart -lcublas -fopenmp -lnvidia-ml

%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Dependencies
main.o: main.c main.h
ifeq ($(GIT_REPO), true)
	GIT_VERSION=$$(echo $$(git describe --dirty --always --tags) | tr - +); \
	$(CC) $(CFLAGS) $(CPPFLAGS) -DTS_VERSION=$${GIT_VERSION} -c $< -o $@
endif
server_start.o: server_start.c main.h
server.o: server.c main.h
client.o: client.c main.h
msgdump.o: msgdump.c main.h
jobs.o: jobs.c main.h
execute.o: execute.c main.h
msg.o: msg.c main.h
mail.o: mail.c main.h
error.o: error.c main.h
signals.o: signals.c main.h
list.o: list.c main.h
tail.o: tail.c main.h
gpu.o: gpu.c main.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -L$(CUDA_HOME)/lib64 -I$(CUDA_HOME)/include -lpthread -c $< -o $@

clean:
	rm -f *.o $(TARGET)

install: $(TARGET)
	$(INSTALL) -d $(PREFIX)/bin
	$(INSTALL) ts $(PREFIX)/bin
	$(INSTALL) -d $(PREFIX)/share/man/man1
	$(INSTALL) -m 644 $(TARGET).1 $(PREFIX)/share/man/man1

install-local: $(TARGET)
	$(INSTALL) -d $(PREFIX_LOCAL)/bin
	$(INSTALL) ts $(PREFIX_LOCAL)/bin
	$(INSTALL) -d $(PREFIX_LOCAL)/.local/share/man/man1
	$(INSTALL) -m 644 $(TARGET).1 $(PREFIX_LOCAL)/.local/share/man/man1

.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(PREFIX)/share/man/man1/$(TARGET).1

uninstall-local:
	rm -f $(PREFIX_LOCAL)/bin/$(TARGET)
	rm -f $(PREFIX_LOCAL)/.local/share/man/man1/$(TARGET).1

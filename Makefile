PREFIX?=/usr/local
PREFIX_LOCAL=~
GLIBCFLAGS=-D_XOPEN_SOURCE=500
CPPFLAGS+=$(GLIBCFLAGS)
CFLAGS?=-pedantic -Wall -g -O2 -std=c11
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
	tail.o \
	cjson/cJSON.o
TARGET=ts
INSTALL=install -c

GIT_REPO=$(shell git rev-parse --is-inside-work-tree)

# The GPU build needs only NVML (part of the NVIDIA driver), not the CUDA compute
# runtime -- so we link -lnvidia-ml only (no -lcudart/-lcublas). CUDA_HOME, if set,
# is used purely as a search hint for nvml.h and libnvidia-ml.
ifdef CUDA_HOME
CUDA_INCLUDE=-I$(CUDA_HOME)/include
CUDA_LIBDIR=-L$(CUDA_HOME)/lib64 -L$(CUDA_HOME)/lib64/stubs
endif

all: OBJECTS+= gpu.o
all: LDFLAGS+=$(CUDA_LIBDIR) $(CUDA_INCLUDE)
all: LDLIBS+=-lnvidia-ml
all: gpu.o

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $(TARGET)

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
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CUDA_INCLUDE) -c $< -o $@
cjson/cJSON.o: cjson/cJSON.c cjson/cJSON.h

cpu: CFLAGS+=-DCPU

all cpu: $(TARGET)

all cpu:
ifeq ($(GIT_REPO), true)
	GIT_VERSION=$$(echo $$(git describe --dirty --always --tags) | tr - +); \
	$(CC) $(CFLAGS) -DTS_VERSION=$${GIT_VERSION} man.c -o makeman
endif

clean:
	rm -f *.o cjson/*.o $(TARGET) makeman ts.1

install: $(TARGET)
	$(INSTALL) -d $(PREFIX)/bin
	$(INSTALL) ts $(PREFIX)/bin
	$(INSTALL) -d $(PREFIX)/share/man/man1
	./makeman
	$(INSTALL) -m 644 $(TARGET).1 $(PREFIX)/share/man/man1

install-local: $(TARGET)
	$(INSTALL) -d $(PREFIX_LOCAL)/bin
	$(INSTALL) ts $(PREFIX_LOCAL)/bin
	$(INSTALL) -d $(PREFIX_LOCAL)/.local/share/man/man1
	./makeman
	$(INSTALL) -m 644 $(TARGET).1 $(PREFIX_LOCAL)/.local/share/man/man1

.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(PREFIX)/share/man/man1/$(TARGET).1

uninstall-local:
	rm -f $(PREFIX_LOCAL)/bin/$(TARGET)
	rm -f $(PREFIX_LOCAL)/.local/share/man/man1/$(TARGET).1

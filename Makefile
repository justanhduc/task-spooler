
PREFIX?=/usr/local
PREFIX_LOCAL=~
GLIBCFLAGS=#-D_XOPEN_SOURCE=500 -D__STRICT_ANSI__
CPPFLAGS+=$(GLIBCFLAGS) -D_DEFAULT_SOURCE
CFLAGS?=-pedantic -ansi -Wall -g -std=gnu11 -fcommon -Wno-format-truncation # -DSOUND
ifdef CGROUP_V2
CFLAGS += -DCGROUP_V2
endif
ifdef TS_CPU_BIND
CFLAGS += -DTS_CPU_BIND
OBJECTS=main.o \
	server.o \
	server_start.o \
	client.o \
	msgdump.o \
	jobs.o \
	notify.o \
	job_ops.o \
	server_user.o \
	server_env.o \
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
	user.o \
	cJSON.o \
	sqlite.o \
	runtime_limit.o \
	cgroups.o \
	vec.o \
	utils.o \
	cpu_bind.o
else
OBJECTS=main.o \
	server.o \
	server_start.o \
	client.o \
	msgdump.o \
	jobs.o \
	notify.o \
	job_ops.o \
	server_user.o \
	server_env.o \
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
	user.o \
	cJSON.o \
	sqlite.o \
	runtime_limit.o \
	cgroups.o \
	vec.o \
	utils.o
endif

TARGET=ts
INSTALL=install -c

GIT_REPO=$(shell git rev-parse --is-inside-work-tree)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $(TARGET) $^ -lsqlite3

%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Dependencies
main.o: main.c main.h version.h defaults.h user.h utils.h runtime_limit.h error.h server_start.h client.h server.h signals.h
ifeq ($(GIT_REPO), true)
	GIT_VERSION=$$(echo $$(git describe --dirty --always --tags) | tr - +); \
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
endif
user.o: user.c user.h main.h defaults.h jobs.h
server.o: server.c server.h main.h msg.h jobs.h user.h vec.h defaults.h error.h notify.h job_ops.h server_user.h server_env.h server_start.h sqlite.h cgroups.h signals.h runtime_limit.h info.h print.h
server_start.o: server_start.c server_start.h server.h main.h error.h user.h runtime_limit.h sqlite.h
client.o: client.c client.h main.h msg.h server_start.h error.h utils.h env.h execute.h tail.h
msgdump.o: msgdump.c msgdump.h main.h msg.h
jobs.o: jobs.c jobs.h main.h msg.h user.h vec.h defaults.h sqlite.h runtime_limit.h cgroups.h info.h list.h utils.h notify.h mail.h execute.h error.h
notify.o: notify.c notify.h jobs.h main.h msg.h vec.h error.h
job_ops.o: job_ops.c job_ops.h main.h msg.h jobs.h user.h vec.h list.h info.h print.h runtime_limit.h error.h server_user.h utils.h
server_user.o: server_user.c server_user.h main.h msg.h jobs.h user.h vec.h runtime_limit.h sqlite.h utils.h list.h cgroups.h
server_env.o: server_env.c server_env.h main.h msg.h jobs.h error.h
execute.o: execute.c execute.h main.h msg.h jobs.h signals.h mail.h error.h client.h runtime_limit.h
msg.o: msg.c msg.h main.h msgdump.h error.h
mail.o: mail.c mail.h main.h msg.h error.h signals.h print.h
error.o: error.c error.h main.h msg.h server.h server_start.h sqlite.h notify.h msgdump.h
signals.o: signals.c signals.h main.h msg.h
list.o: list.c list.h jobs.h main.h msg.h user.h runtime_limit.h cgroups.h error.h
print.o: print.c print.h main.h msg.h error.h
info.o: info.c info.h jobs.h main.h msg.h error.h runtime_limit.h
env.o: env.c env.h main.h msg.h error.h signals.h
tail.o: tail.c tail.h main.h msg.h error.h client.h
cJSON.o : cjson/cJSON.c cjson/cJSON.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
sqlite.o: sqlite.c sqlite.h defaults.h main.h msg.h jobs.h utils.h error.h
runtime_limit.o: runtime_limit.c runtime_limit.h main.h msg.h jobs.h defaults.h error.h utils.h
cpu_bind.o: cpu_bind.c cpu_bind.h topology.h vec.h
cgroups.o: cgroups.c cgroups.h main.h msg.h jobs.h error.h list.h user.h
vec.o: vec.c vec.h
utils.o: utils.c utils.h error.h

clean:
	rm -f *.o $(TARGET)
	-killall ts
	-rm -f ts

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

mpi_pi: tools/mpi_pi.c
	mpicc -O2 -fopenmp -o tools/mpi_pi tools/mpi_pi.c

.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)
	rm -f $(PREFIX)/share/man/man1/$(TARGET).1

uninstall-local:
	rm -f $(PREFIX_LOCAL)/bin/$(TARGET)
	rm -f $(PREFIX_LOCAL)/.local/share/man/man1/$(TARGET).1

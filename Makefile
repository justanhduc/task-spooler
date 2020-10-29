PREFIX?=/usr/local
GLIBCFLAGS=-D_XOPEN_SOURCE=500 -D__STRICT_ANSI__
CPPFLAGS+=$(GLIBCFLAGS)
CFLAGS?=-pedantic -ansi -Wall -g -O0
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
	tail.o
INSTALL=install -c

all: ts

tsretry: tsretry.c

ts: $(OBJECTS)
	$(CC) $(LDFLAGS) -o ts $^

# Test our 'tail' implementation.
ttail: tail.o ttail.o
	$(CC) $(LDFLAGS) -o ttail $^


.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<

# Dependencies
main.o: main.c main.h
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
ttail.o: ttail.c main.h

clean:
	rm -f *.o ts

install: ts
	$(INSTALL) -d $(PREFIX)/bin
	$(INSTALL) ts $(PREFIX)/bin
	$(INSTALL) -d $(PREFIX)/share/man/man1
	$(INSTALL) -m 644 ts.1 $(PREFIX)/share/man/man1

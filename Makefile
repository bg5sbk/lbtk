CC=gcc
CFLAGS = -pipe -std=c99
CFLAGS+= -Wall -Wextra
CFLAGS+= -O2 -s
CFLAGS+= -D_GNU_SOURCE
CFLAGS+= -D_XOPEN_SOURCE=700
#CFLAGS+= -DHAVE_ASSERT=1
#CFLAGS+= -DHAVE_DEBUG=1
INCDIRS= -I $(LOCAL)/include
LIBNN= -lnanomsg
LIBDIRS= -L $(LOCAL)/lib


OBJ=
OBJ+= gen
OBJ+= proxy-tcp-splice
OBJ+= proxy-tcp
OBJ+= echo-tcp-splice
OBJ+= echo-tcp
OBJ+= refresh-static
OBJ+= refresh-file
OBJ+= refresh-etcd

.PHONY: all clean install
all: $(OBJ)
clean:
	-/bin/rm -f $(OBJ)
install: all
	/usr/bin/install -m 0755 $(OBJ) $(LOCAL)/bin/

gen: Makefile gen.go
	go get github.com/gdamore/mangos
	go build gen.go

proxy-tcp-splice: Makefile proxy-tcp-splice.c utils.h utils.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$+) $(LIBDIRS) $(INCDIRS) $(LIBNN)
proxy-tcp: Makefile proxy-tcp.go
	go get github.com/gdamore/mangos
	go build proxy-tcp.go

echo-tcp-splice: Makefile echo-tcp-splice.c utils.h utils.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$+) $(LIBDIRS) $(INCDIRS)
echo-tcp: Makefile echo-tcp.go
	go build echo-tcp.go

refresh-file: Makefile refresh-file.go
	go get github.com/jfsmig/exp/inotify
	go build refresh-file.go
refresh-static: Makefile refresh-static.go
	go build refresh-static.go
refresh-etcd: Makefile refresh-etcd.go
	go get github.com/coreos/etcd
	go build refresh-etcd.go


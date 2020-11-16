CC=gcc
CFLAGS=-O2 -Wall
REMOVERS?=0
ifeq ($(REMOVERS),1)
CFLAGS+=-DREMOVERS
endif

SRCC=jcp.c
SRCC+=jcp_handler.c
SRCH=dumpver.h flashstub.h romdump6.h turbow.h univbin.h e2pget.h e2pput.h
SRCH+=jcp_handler.h
OBJS=$(SRCC:.c=.o) 

all: .depend jcp 

jcp: $(OBJS) $(SRCH)
	gcc -o jcp $(OBJS) -lusb

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *~ $(OBJS) jcp

.depend: $(SRCC)
	$(CC) -MM $(SRCC) > .depend

-include .depend

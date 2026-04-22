CC      = gcc
CFLAGS  = -Wall -Wextra -g
TARGETS = oss worker

.PHONY: all clean
all: $(TARGETS)

oss: oss.o
	$(CC) $(CFLAGS) -o oss oss.o

worker: worker.o
	$(CC) $(CFLAGS) -o worker worker.o

.c.o:
	$(CC) $(CFLAGS) -c $<

oss.o:    oss.c shared.h
worker.o: worker.c shared.h

clean:
	rm -f $(TARGETS) *.o oss.log

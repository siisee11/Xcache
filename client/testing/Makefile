.DEFAULT_GOAL := all

CC=g++
LDFLAGS=
LIBS=
CFLAGS=-std=c++11 -w

EXECUTABLE=read_file write_file

all: $(EXECUTABLE)

read_file:
	$(CC) $(CFLAGS) -g read_file.c -o read_file $(LIBS)

write_file:
	$(CC) $(CFLAGS) -g write_file.c -o write_file $(LIBS)

clean:
	rm -rf *.o $(EXECUTABLE)

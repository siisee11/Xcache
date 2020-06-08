.SUFFIXES : .c .o

CC = gcc
CFLAGS = -g -I. 
WARNING = -Wall -Wextra -Wconversion -pedantic -std=gnu11
INC = 
LIBS = -lurcu

OBJECTS = main.o hotring.o benchmark.o test.o

TARGET = a.out

all : $(TARGET)

run : $(TARGET)
	./$(TARGET) -vvv

gdb : $(TARGET)
	gdb $(TARGET)

$(TARGET) : $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean :
	rm -rf $(OBJECTS) $(TARGET) core

new : 
	$(MAKE) clean 
	$(MAKE) 


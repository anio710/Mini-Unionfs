CC = gcc
CFLAGS = -Wall
LIBS = -lfuse3

TARGET = mini_unionfs

all: $(TARGET)

$(TARGET): mini_unionfs.c
	$(CC) $(CFLAGS) mini_unionfs.c -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

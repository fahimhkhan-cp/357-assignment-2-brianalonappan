CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11
TARGET = fs_emulator
SRC = fs_emulator.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) fs_run


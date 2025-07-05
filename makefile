CC = gcc
CFLAGS = -Wall -g $(shell pkg-config --cflags xft freetype2)
LDFLAGS = $(shell pkg-config --libs xft freetype2) -lX11 -lm
SRCS = calculator.c simple_gui.c

OBJS = $(SRCS:.c=.o)

TARGET = calculator

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c simple_gui.h gc.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

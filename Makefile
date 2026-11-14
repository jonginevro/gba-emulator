# Simple build: make -j8
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-common -fno-strict-aliasing
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := gba

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(SDL_LIBS) -lm

%.o: %.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean

CC=gcc
FLAGS=`pkg-config --cflags --libs libdrm`
FLAGS+=-Wall -O0 -g
FLAGS+=-D_FILE_OFFSET_BITS=64
DEPS=modeset.h
OBJ=modeset.o app.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(FLAGS)

all: $(OBJ)
	gcc -o app $^ $(FLAGS)

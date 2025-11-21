# Compila el cliente FTP concurrente

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 $(D_FLAGS)

# Para habilitar struct hostent->h_addr
D_FLAGS = -D_DEFAULT_SOURCE

TARGET = clienteFTPcon

OBJS = \
    AnacichaD-clienteFTP.o \
    connectsock.o \
    connectTCP.o \
    errexit.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

AnacichaD-clienteFTPcon.o: AnacichaD-clienteFTP.c
	$(CC) $(CFLAGS) -c AnacichaD-clienteFTP.c

connectsock.o: connectsock.c
	$(CC) $(CFLAGS) -c connectsock.c

connectTCP.o: connectTCP.c
	$(CC) $(CFLAGS) -c connectTCP.c

errexit.o: errexit.c
	$(CC) $(CFLAGS) -c errexit.c

clean:
	rm -f *.o $(TARGET)

#
#


CC=gcc
CFLAGS= -lrdmacm -libverbs -pthread

# Default rules 
all : semeru_cpu_user.c
	$(CC) $(CFLAGS)   semeru_cpu_user.c -o semeru_cpu_user.o

clean: 
	rm semeru_cpu_user.o

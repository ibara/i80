# z80 Makefile

PROG =	z80
OBJS =	z80.o

all: ${OBJS}
	${CC} ${LDFLAGS} -o ${PROG} ${OBJS}

clean:
	rm -f ${PROG} ${OBJS}

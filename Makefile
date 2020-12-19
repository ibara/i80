# i80 Makefile

PROG =	i80
OBJS =	i80.o

all: ${OBJS}
	${CC} ${LDFLAGS} -o ${PROG} ${OBJS}

clean:
	rm -f ${PROG} ${OBJS}

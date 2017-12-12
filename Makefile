CC ?= cc
FLAGS=`pkg-config --cflags glib-2.0 --cflags gstreamer-1.0`
LIBS=`pkg-config --libs glib-2.0 --libs gstreamer-1.0`

OBJS = gobject-list.o

all: libgobject-list.so
.PHONY: all clean
clean:
	rm -f libgobject-list.so $(OBJS)

%.o: %.c
	$(CC) -fPIC -rdynamic -g -c -Wall -Wextra ${FLAGS} ${BUILD_OPTIONS} $<

libgobject-list.so: $(OBJS)
	$(CC) -shared -Wl,-soname,$@ -o $@ $^ -lc -ldl ${LIBS}

TARGET=httpserver
OBJS=httpserver.o vthread.o

CFLAGS+=-DTEST
ifeq ($(CC),mingw32-gcc)
WIN32:=1
endif
ifeq ($(CC),cl.exe)
WIN32:=1
endif

ifeq ($(WIN32),1)
LDFLAGS+=-lws2_32
else
LDFLAGS+=-lpthread
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	$(RM) $(OBJS) $(TARGET)
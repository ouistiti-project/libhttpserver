TARGET=httpserver
OBJS=httpserver.o vthread.o

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

$(TARGET):CFLAGS+=-DTEST
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

lib:CFLAGS+=-fPIC 
lib:lib-static lib-dynamic

lib-static: lib$(TARGET).a
lib-dynamic: lib$(TARGET).so

lib$(TARGET).a:$(OBJS)
	$(AR) rcs $@ $^

lib$(TARGET).so:$(OBJS)
	gcc -shared -Wl,-soname,$@.1 -o $@  $^

clean:
	$(RM) $(OBJS) $(TARGET) lib$(TARGET).so lib$(TARGET).a

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
SLIBEXT=lib
DLIBEXT=dll
else
LDFLAGS+=-lpthread
SLIBEXT=a
DLIBEXT=so
endif

all: $(TARGET)

$(TARGET):CFLAGS+=-DTEST -g -Wall
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

lib:CFLAGS+=-fPIC 
lib:lib-static lib-dynamic

lib-static: lib$(TARGET).$(SLIBEXT)
lib-dynamic: lib$(TARGET).$(DLIBEXT)

lib$(TARGET).$(SLIBEXT):$(OBJS)
	$(AR) rcs $@ $^

lib$(TARGET).$(DLIBEXT):$(OBJS)
	$(CC) -shared -Wl,-soname,$@.1 -o $@  $^ $(LDFLAGS)

clean:
	$(RM) $(OBJS) $(TARGET) lib$(TARGET).$(DLIBEXT) lib$(TARGET).$(SLIBEXT)

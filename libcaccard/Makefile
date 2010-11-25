include ../Makefile.objs
include ../config-host.mak
include ../config-all-devices.mak
include $(SRC_PATH)/rules.mak

CFLAGS+=-fPIC

libcaccard.so: $(libcaccard-y)
	gcc -shared $(libcaccard_libs) -o $@ $^

vscclient: $(libcaccard-y) vscclient.o
	gcc $(libcaccard_libs) -o $@ $^

all: libcaccard.so vscclient

clean:
	rm -f *.o */*.o *.d */*.d *.a */*.a *~ */*~ libcaccard.so


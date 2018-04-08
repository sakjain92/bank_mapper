LCC=gcc
LCFLAGS=-Werror -Wall -O1 -g3
KOBJECT=kam
OBJECT=bank_test

all: $(OBJECT) $(KOBJECT)

*_test: *_test.c
	$(LCC) $(LCFLAGS) -o $@ $?


obj-m += $(KOBJECT).o

$(KOBJECT):
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f $(OBJECT)

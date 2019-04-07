VERSION=1.0.2

CFLAGS = -g -O2 -Wall -Wextra

CADD  = `pkg-config --cflags fuse gumbo libcurl` -DVERSION=\"$(VERSION)\"
LDADD = `pkg-config --libs fuse gumbo libcurl`
COBJS = main.o network.o fuse_local.o link.o

prefix ?= /usr/local

all: httpdirfs

%.o: src/%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $(CADD) -o $@ $<

httpdirfs: $(COBJS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LDADD) -o $@ $^

install:
	install -m 755 -D httpdirfs \
		$(DESTDIR)$(prefix)/bin/httpdirfs
	install -m 644 -D doc/man/httpdirfs.1 \
		$(DESTDIR)$(prefix)/share/man/man1/httpdirfs.1

doc:
	doxygen Doxyfile

clean:
	-rm -f *.o
	-rm -f httpdirfs
	-rm -rf doc/html

distclean: clean

uninstall:
	-rm -f $(DESTDIR)$(prefix)/bin/httpdirfs
	-rm -f $(DESTDIR)$(prefix)/share/man/man1/httpdirfs.1

.PHONY: all doc install clean distclean uninstall

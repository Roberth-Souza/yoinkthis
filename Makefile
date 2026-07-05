PREFIX ?= /usr/local
PKGS := gtk+-3.0 gtk-layer-shell-0

CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS))

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
HDR := $(wildcard src/*.h)

yoinkthis: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): $(HDR) src/style_css.h

src/style_css.h: style.css
	{ printf 'static const char DEFAULT_CSS[] =\n'; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' style.css; \
	  printf ';\n'; } > $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

install: yoinkthis
	install -Dm755 yoinkthis $(DESTDIR)$(PREFIX)/bin/yoinkthis

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/yoinkthis

clean:
	rm -f yoinkthis src/*.o src/style_css.h

.PHONY: install uninstall clean

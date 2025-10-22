
# Poenostavljen Makefile za baRadio
CC = gcc
SRC = src/main.c
TARGET = baradio
PKGS = gtk+-3.0 ayatana-appindicator3-0.1 sqlite3 gstreamer-1.0
CFLAGS = $(shell pkg-config --cflags $(PKGS))
LDFLAGS = $(shell pkg-config --libs $(PKGS))

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)


install:
	mkdir -p /usr/local/bin
	mkdir -p /usr/local/share/icons/hicolor/scalable/apps
	mkdir -p /usr/local/share/applications
	cp baradio /usr/local/bin/
	cp icons/baradio.svg /usr/local/share/icons/hicolor/scalable/apps/baradio.svg
	cp baradio.desktop /usr/local/share/applications/baradio.desktop

.PHONY: all clean install
uninstall:
	rm -f /usr/local/bin/baradio
	rm -f /usr/local/share/icons/hicolor/scalable/apps/baradio.svg
	rm -f /usr/local/share/applications/baradio.desktop
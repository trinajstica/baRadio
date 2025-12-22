
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
	mkdir -p /usr/local/share/icons/hicolor/16x16/apps
	mkdir -p /usr/local/share/icons/hicolor/32x32/apps
	mkdir -p /usr/local/share/icons/hicolor/48x48/apps
	mkdir -p /usr/local/share/icons/hicolor/64x64/apps
	mkdir -p /usr/local/share/pixmaps
	mkdir -p /usr/local/share/applications
	cp baradio /usr/local/bin/
	# Copy SVG next to executable so program can find it (AppImage/local installs)
	cp icons/baradio.svg /usr/local/bin/baradio.svg
	cp icons/baradio.svg /usr/local/share/icons/hicolor/scalable/apps/baradio.svg
	# If rsvg-convert is available, create PNG fallbacks for popular sizes
	if command -v rsvg-convert >/dev/null 2>&1; then \
		rsvg-convert -w 16 -h 16 icons/baradio.svg -o /usr/local/share/icons/hicolor/16x16/apps/baradio.png; \
		rsvg-convert -w 32 -h 32 icons/baradio.svg -o /usr/local/share/icons/hicolor/32x32/apps/baradio.png; \
		rsvg-convert -w 48 -h 48 icons/baradio.svg -o /usr/local/share/icons/hicolor/48x48/apps/baradio.png; \
		rsvg-convert -w 64 -h 64 icons/baradio.svg -o /usr/local/share/icons/hicolor/64x64/apps/baradio.png; \
		rsvg-convert -w 48 -h 48 icons/baradio.svg -o /usr/local/share/pixmaps/baradio.png; \
		rsvg-convert -w 48 -h 48 icons/baradio.svg -o /usr/local/bin/baradio.png; \
	else \
		cp icons/baradio.svg /usr/local/share/pixmaps/baradio.svg; \
	fi
	cp baradio.desktop /usr/local/share/applications/baradio.desktop
	# Update icon cache if available
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f /usr/local/share/icons/hicolor || true; \
	fi

.PHONY: all clean install
uninstall:
	rm -f /usr/local/bin/baradio
	rm -f /usr/local/share/icons/hicolor/scalable/apps/baradio.svg
	rm -f /usr/local/bin/baradio.svg
	rm -f /usr/local/bin/baradio.png
	rm -f /usr/local/share/icons/hicolor/16x16/apps/baradio.png
	rm -f /usr/local/share/icons/hicolor/32x32/apps/baradio.png
	rm -f /usr/local/share/icons/hicolor/48x48/apps/baradio.png
	rm -f /usr/local/share/icons/hicolor/64x64/apps/baradio.png
	rm -f /usr/local/share/pixmaps/baradio.png
	rm -f /usr/local/share/pixmaps/baradio.svg
	rm -f /usr/local/share/applications/baradio.desktop
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -f /usr/local/share/icons/hicolor || true; \
	fi
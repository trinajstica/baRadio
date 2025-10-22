baRadio
======

Preprost GTK3 radijski predvajalnik (baRadio v2).

Gradnja
-------

Potrebne knjižnice (primer za Debian/Ubuntu):

- libgtk-3-dev
- libayatana-appindicator3-dev
- libsqlite3-dev
- libgstreamer1.0-dev

Za gradnjo v projektu:

```bash
make
```

Zagon
-----

Po uspešni gradnji:

```bash
./baradio
```

Namestitev
----------

```bash
sudo make install
```
# baRadio

Enostaven radio predvajalnik za Linux z GTK3 vmesnikom in podporo za tray ikono.

## Funkcionalnost
- Predvajanje spletnih radijskih postaj
- Upravljanje seznama postaj (dodajanje, urejanje, brisanje)
- Prikaz trenutne skladbe (če jo postaja oddaja)
- Tray ikona z osnovnimi kontrolami
- Podpora za multimedijske tipke (play/pause, stop, next, previous)
- MPRIS D-Bus integracija za sistemske medijske kontrole
- Iskanje postaj po imenu
- Shranjevanje zadnje predvajane postaje

## Zahteve za uporabo

Programu za delovanje potrebujete naslednje knjižnice:

- **GTK 3** - grafični vmesnik
- **GStreamer 1.0** - predvajanje avdio tokov
- **SQLite 3** - baza podatkov za postaje
- **Ayatana AppIndicator 3** - tray ikona

### Namestitev odvisnosti

**Ubuntu/Debian/Mint:**
```sh
sudo apt install libgtk-3-0 libayatana-appindicator3-1 libsqlite3-0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
```

**Fedora/RHEL/CentOS:**
```sh
sudo dnf install gtk3 libayatana-appindicator-gtk3 sqlite gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free
```

**Arch Linux/Manjaro:**
```sh
sudo pacman -S gtk3 libayatana-appindicator sqlite gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad
```

**Solus:**
```sh
sudo eopkg install libgtk-3 libayatana-appindicator sqlite3 gstreamer-1.0-plugins-base gstreamer-1.0-plugins-good gstreamer-1.0-plugins-bad
```

## Zahteve za razvoj

Za kompilacijo programa iz izvorne kode potrebujete:


### Namestitev razvojnih odvisnosti (Solus)

Za prevajanje in razvoj aplikacije na Solus Linux namestite vse potrebne knjižnice in orodja z enim ukazom:

```sh
sudo eopkg install -c system.devel libgtk-3-devel libayatana-appindicator-devel sqlite3-devel gstreamer-1.0-devel gstreamer-1.0-plugins-base-devel
```

To namesti:
- vse osnovne razvojne pakete (gcc, make, pkgconfig ...)
- razvojne datoteke za GTK3, Ayatana AppIndicator, SQLite3, GStreamer

Po namestitvi lahko aplikacijo prevedete z ukazom `make`.

### Namestitev razvojnih orodij

**Ubuntu/Debian/Mint:**
```sh
sudo apt install build-essential pkg-config libgtk-3-dev libayatana-appindicator3-dev libsqlite3-dev libgstreamer1.0-dev
```

**Fedora/RHEL/CentOS:**
```sh
sudo dnf install gcc make pkg-config gtk3-devel libayatana-appindicator-gtk3-devel sqlite-devel gstreamer1-devel
```

**Arch Linux/Manjaro:**
```sh
sudo pacman -S base-devel pkg-config gtk3 libayatana-appindicator sqlite gstreamer
```

**Solus:**
```sh
sudo eopkg install -c system.devel
sudo eopkg install libgtk-3-devel libayatana-appindicator-devel sqlite3-devel gstreamer-1.0-devel
```

## Gradnja

```sh
make
```

## Namestitev

```sh
sudo make install
```

Program se namesti v `/usr/local/bin/`, ikona v `/usr/local/share/icons/hicolor/scalable/apps/` in `.desktop` datoteka v `/usr/local/share/applications/`.

## Odstranitev

```sh
sudo make uninstall
```

## Zagon

```sh
baradio
```

Ali poiščite "baRadio" v svojem aplikacijskem meniju.

## Flatpak

Za gradnjo Flatpak paketa:

```sh
./build-flatpak.sh
```

Nato lahko program zaženete z:

```sh
flatpak run si.generacija.baradio
```

## Uporaba

- **Desni klik** na seznam postaj za dodajanje, urejanje ali brisanje postaj
- **Dvojni klik** na postajo za predvajanje
- **Tipkanje** v seznamu postaj odpre iskalno polje
- **ESC** zapre iskalno polje
- **Multimedijske tipke** za kontrolo predvajanja

## Baza podatkov

Postaje in nastavitve so shranjene v SQLite bazi: `~/.config/baradio/baradio.db`

## Opombe

- Ikona za tray mora biti na voljo v sistemski temi ikon pod imenom `radio`
- Program podpira samo en tekoči primerek (single instance)
- Za predvajanje nekaterih formatov lahko potrebujete dodatne GStreamer vtičnike

## Avtor

BArko, 2025
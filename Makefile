CC=gcc

OPTIMIZACION=-g3
#OPTIMIZACION=-O3

GTK_FLAGS=`pkg-config --cflags gtk+-3.0 ayatana-appindicator3-0.1 libusb-1.0 glib-2.0 gio-2.0 libudev`
GTK_LIB=`pkg-config --libs gtk+-3.0 ayatana-appindicator3-0.1 libusb-1.0 glib-2.0 gio-2.0 libudev`

INCLUDE=-Wformat=2  $(OPTIMIZACION) -fPIC -pthread -g3 -O0 -rdynamic -Wall -Isrc/include -I/usr/include ${GTK_FLAGS}

all: clean app-obj app-bin permisos instalar

app-obj:
	$(CC) $(INCLUDE) -o obj/comun.o -c src/comun.c
	$(CC) $(INCLUDE) -o obj/teclado.o -c src/teclado.c
	$(CC) $(INCLUDE) -o obj/pantalla.o -c src/pantalla.c
	$(CC) $(INCLUDE) -o obj/monitor_bluetooth.o -c src/monitor_bluetooth.c
	$(CC) $(INCLUDE) -o obj/monitor_orientacion.o -c src/monitor_orientacion.c
	$(CC) $(INCLUDE) -o obj/monitor_teclado_usb.o -c src/monitor_teclado_usb.c
	$(CC) $(INCLUDE) -o obj/config.o -c src/config.c
	$(CC) $(INCLUDE) -o obj/main.o -c src/main.c
	$(CC) $(INCLUDE) -o obj/gui_daemon.o -c src/gui_daemon.c

app-bin:
	$(CC) $(INCLUDE) -o bin/zbd obj/comun.o obj/teclado.o obj/pantalla.o obj/monitor_bluetooth.o obj/monitor_orientacion.o obj/monitor_teclado_usb.o obj/config.o obj/main.o ${GTK_LIB}
	$(CC) $(INCLUDE) -o bin/zbd-tray obj/comun.o obj/teclado.o obj/pantalla.o obj/monitor_bluetooth.o obj/monitor_orientacion.o obj/monitor_teclado_usb.o obj/config.o obj/gui_daemon.o ${GTK_LIB}

instalar:
	mkdir -p /etc/zbd/
	cp conf/* /etc/zbd/
	cp bin/* /usr/local/bin

clean:
	rm -f obj/* bin/*

permisos:
	chmod 775 bin/*
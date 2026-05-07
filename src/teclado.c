#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>

#include "comun.h"
#include "teclado.h"

#define VENDOR_ID   0x0b05
#define PRODUCT_ID  0x1b2c

#define REPORT_ID   0x5A   // ID de reporte
#define WVALUE      0x035A // wValue para la petición SET_REPORT
#define WINDEX      4      // Número de interfaz (en el script Python se usaba 4)
#define WLENGTH     16     // Número de bytes a enviar en el paquete

int set_brillo_teclado(int nivel_brillo) {
    if (nivel_brillo < 0 || nivel_brillo > 3) {
        fprintf(stderr, "Nivel inválido. Debe ser un entero entre 0 y 3.\n");
        return EXIT_FAILURE;
    }

    /* Inicializar libusb */
    int r = libusb_init(NULL);
    if (r < 0) {
        fprintf(stderr, "Error al inicializar libusb: %s\n", libusb_strerror(r));
        return EXIT_FAILURE;
    }

    /* Buscar el dispositivo */
    libusb_device_handle *dev_handle = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID);
    if (!dev_handle) {
        fprintf(stderr, "Dispositivo no encontrado (Vendor ID: 0x%04X, Product ID: 0x%04X)\n",
                VENDOR_ID, PRODUCT_ID);
        libusb_exit(NULL);
        return EXIT_FAILURE;
    }

    /* Verificar y desanexar el driver del kernel si está activo en la interfaz WINDEX.
     * En el UX8406MA la interfaz 4 normalmente no tiene driver kernel asociado
     * (es HID Vendor-defined del Asus Dial / backlight), pero comprobamos por
     * defensa.  `kernel_was_attached` permite reanexar simétricamente al final. */
    int kernel_was_attached = 0;
    if (libusb_kernel_driver_active(dev_handle, WINDEX) == 1) {
        r = libusb_detach_kernel_driver(dev_handle, WINDEX);
        if (r < 0) {
            fprintf(stderr, "No se pudo desanexar el driver del kernel: %s\n", libusb_strerror(r));
            libusb_close(dev_handle);
            libusb_exit(NULL);
            return EXIT_FAILURE;
        }
        kernel_was_attached = 1;
    }

    /* Reclamar la interfaz: REQUERIDO antes de un control_transfer dirigido a
     * una interfaz (bmRequestType bit recipient = INTERFACE).  Sin este claim,
     * el kernel emite el warning "usbfs: did not claim interface N before use"
     * (drivers/usb/core/devio.c::checkintf).  Aunque la transferencia se
     * permite hoy, futuros kernels podrían denegarla con -EBUSY. */
    r = libusb_claim_interface(dev_handle, WINDEX);
    if (r < 0) {
        fprintf(stderr, "No se pudo reclamar la interfaz %d: %s\n", WINDEX, libusb_strerror(r));
        if (kernel_was_attached)
            libusb_attach_kernel_driver(dev_handle, WINDEX);
        libusb_close(dev_handle);
        libusb_exit(NULL);
        return EXIT_FAILURE;
    }

    /* Preparar el paquete de datos (16 bytes) */
    uint8_t data[WLENGTH];
    for (int i = 0; i < WLENGTH; i++)
        data[i] = 0x00;  // Inicializar con ceros

    data[0] = REPORT_ID; // Primer byte: ID de reporte
    data[1] = 0xBA;
    data[2] = 0xC5;
    data[3] = 0xC4;
    data[4] = (uint8_t)nivel_brillo;

    /* Parametros para libusb_control_transfer:
       bmRequestType: 0x21 (Host to Device | Class | Interface)
       bRequest:      0x09 (SET_REPORT, típico de dispositivos HID)
       wValue:        WVALUE (0x035A)
       wIndex:        WINDEX (4, la interfaz)
       data:          data[] (el buffer)
       wLength:       WLENGTH (16 bytes)
       timeout:       1000 ms
    */
    uint8_t bmRequestType = 0x21;
    uint8_t bRequest      = 0x09;
    uint16_t wValue       = WVALUE;
    uint16_t wIndex       = WINDEX;

    int transferred = libusb_control_transfer(dev_handle,
                                              bmRequestType,
                                              bRequest,
                                              wValue,
                                              wIndex,
                                              data,
                                              WLENGTH,
                                              1000 /* timeout ms */);

    /* Liberar la interfaz reclamada — siempre, incluso si la transferencia falló.
     * libusb_release_interface() es seguro de invocar antes de evaluar el
     * resultado del control_transfer; el handle sigue válido. */
    int release_r = libusb_release_interface(dev_handle, WINDEX);
    if (release_r < 0)
        fprintf(stderr, "Aviso: liberar interfaz %d: %s\n", WINDEX, libusb_strerror(release_r));

    /* Reanexar el driver del kernel SOLO si fue desanexado por nosotros.
     * En la interfaz 4 del UX8406MA no hay driver kernel, así que normalmente
     * esto no se ejecuta.  Llamarlo en el caso contrario produciría -ENOENT. */
    if (kernel_was_attached)
        libusb_attach_kernel_driver(dev_handle, WINDEX);

    /* Cleanup unificado del handle/contexto: una sola vez, antes del return. */
    libusb_close(dev_handle);
    libusb_exit(NULL);

    /* Evaluación del resultado del control_transfer tras el cleanup. */
    if (transferred < 0) {
        fprintf(stderr, "Transferencia de control falló: %s\n", libusb_strerror(transferred));
        return EXIT_FAILURE;
    }

    if (transferred != WLENGTH) {
        fprintf(stderr, "Advertencia: se enviaron solo %d bytes de %d.\n", transferred, WLENGTH);
    } else {
        printf("Nivel de brillo de teclado definido correctamente: %d.\n", nivel_brillo);
    }

    return EXIT_SUCCESS;
}

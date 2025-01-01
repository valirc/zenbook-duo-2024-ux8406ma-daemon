
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

int ejecutar_comando(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    /* Se recomienda usar un buffer lo suficientemente grande.
       Ajusta a tus necesidades. */
    char cmd_buffer[1024];

    vsnprintf(cmd_buffer, sizeof(cmd_buffer), fmt, args);
    va_end(args);

    /* Opcional: verificar si se excedió el tamaño del buffer
       (si vsnprintf() truncó la salida). Por simplicidad, omitimos
       ese check aquí. */

    /* Muestra el comando para depurar */
    printf("Ejecutando %s\n", cmd_buffer);

    /* Ejecuta el comando */
    return system(cmd_buffer);
}

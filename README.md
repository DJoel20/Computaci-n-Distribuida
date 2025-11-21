Cliente FTP Concurrente – AnacichaD

Este proyecto implementa un cliente FTP concurrente en lenguaje C, compatible con servidores FTP reales como vsftpd. El cliente utiliza un canal de control (TCP) y abre canales de datos concurrentes mediante fork(), permitiendo ejecutar varias transferencias al mismo tiempo (descargas, subidas y listados en paralelo).

Fue desarrollado como parte del curso de Computación Distribuida, empleando sockets TCP y las funciones base entregadas por el ingeniero (connectsock.c, connectTCP.c, errexit.c).
El archivo principal desarrollado por mí es:
AnacichaD-clienteFTPcon.c

Características principales
Soporte de comandos FTP básicos

USER – iniciar sesión

PASS – contraseña (oculta en pantalla)

PWD – mostrar directorio actual

MKD <dir> – crear directorio

DELE <file> – borrar archivo

RNFR / RNTO – renombrar archivos

QUIT – salir

Transferencia de archivos

RETR <file> – descargar

STOR <file> – subir archivos

REST <offset> – reanudar descargas

LIST – listado pasivo

LIST -a – listado activo (modo PORT)

Concurrencia real

Cada transferencia (RETR, STOR, LIST) se ejecuta en un proceso hijo, permitiendo:

realizar múltiples descargas simultáneas,

listar mientras se sube un archivo,

ejecutar operaciones en paralelo sin bloquear el REPL.

Seguridad mejorada

La contraseña no se imprime (se oculta con termios).

Uso obligatorio de modo binario (TYPE I) para evitar corrupción en binarios.

Soporte por defecto únicamente de modo PASV, más seguro y compatible con firewalls.


Requisitos

Linux / WSL

Servidor vsftpd (para pruebas)


Compilación con Makefile

El repositorio incluye un Makefile.
./clienteFTPcon <host> <port>
Ejemplo
./clienteFTPcon localhost 21


Experiencia personal y conclusiones

Realizar este proyecto fue un reto muy enriquecedor, ya que combina varias áreas de la computación distribuida:

Programación de sockets y protocolos reales
Aprendí cómo funciona internamente FTP: canal de control, canal de datos, códigos de respuesta y comandos RFC 959.

Concurrencia con procesos
Implementar fork() para lograr descargas paralelas fue clave. Pude comprobar con herramientas como:

ps aux | grep clienteFTPcon


que existían múltiples procesos trabajando al mismo tiempo.

Depuración y compatibilidad
El proyecto implicó depurar respuestas del servidor, parsear PASV correctamente y manejar errores reales del FTP (550, 530, 227, etc.).

Construcción de una herramienta profesional
El cliente final tiene:

login profesional,

ocultamiento de contraseña,

comandos completos FTP,

soporte mixto activo/pasivo,

uso real con vsftpd.

Este trabajo me ayudó a comprender mucho mejor los protocolos clásicos de internet, cómo funcionan realmente los servidores concurrentes y cómo se detectan y manejan errores en protocolos reales.

/* AnacichaD-clienteFTPcon.c
 * Cliente FTP concurrente simple (USER, PASS, PASV, PORT, RETR, STOR)
 * Añadidos: MKD, PWD, DELE, REST, RNFR/RNTO, LIST
 *
 *
 * Requiere: connectsock.c connectTCP.c errexit.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <termios.h>

int connectsock(const char *host, const char *service, const char *transport);
int connectTCP(const char *host, const char *service);
int errexit(const char *format, ...);

/* Utilities */
ssize_t readline(int fd, char *buf, size_t maxlen);
int get_reply(int ctrl_sock, char *reply, size_t maxlen);
int send_cmd(int ctrl_sock, const char *fmt, ...);

/* FTP specific */
int enter_pasv(int ctrl_sock, char *pasv_ip, int *pasv_port);
int create_port_and_listen(int *listen_sock, int *port_out);
int open_data_active(int listen_sock);
int download_file_active(int ctrl_sock, int listen_sock, const char *filename, long restart_offset);
int upload_file_active(int ctrl_sock, int listen_sock, const char *filename);
int download_file_passive(int ctrl_sock, const char *pasv_ip, int pasv_port, const char *filename, long restart_offset);
int upload_file_passive(int ctrl_sock, const char *pasv_ip, int pasv_port, const char *filename);
int list_passive(int ctrl_sock, const char *pasv_ip, int pasv_port, const char *arg);
int list_active(int ctrl_sock, int listen_sock, const char *arg);

/* Simple interactive loop */
void repl(int ctrl_sock, const char *local_ip);

#define BUFSIZE 8192

/* Read a line (ending with \n) from socket */
ssize_t readline(int fd, char *buf, size_t maxlen) {
    ssize_t n, rc;
    char c;
    for (n = 0; n < (ssize_t)maxlen - 1; ) {
        rc = read(fd, &c, 1);
        if (rc == 1) {
            buf[n++] = c;
            if (c == '\n') break;
        } else if (rc == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            return -1;
        }
    }
    buf[n] = '\0';
    return n;
}

/* Get reply from control channel: read until a line starting with 3-digit code and space */
int get_reply(int ctrl_sock, char *reply, size_t maxlen) {
    char line[1024];
    int code = 0;
    reply[0] = '\0';
    while (1) {
        ssize_t n = readline(ctrl_sock, line, sizeof(line));
        if (n <= 0) return -1;
        /* print server reply to stdout */
        fputs(line, stdout);
        /* copy last line into reply */
        strncpy(reply, line, maxlen - 1);
        reply[maxlen - 1] = '\0';
        /* final line has 3 digits + space */
        if (strlen(line) >= 4 &&
            isdigit((unsigned char)line[0]) &&
            isdigit((unsigned char)line[1]) &&
            isdigit((unsigned char)line[2]) &&
            line[3] == ' ') {
            code = (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
            break;
        }
    }
    return code;
}

/* send a formatted command to control socket (adds \r\n) */
int send_cmd(int ctrl_sock, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    strncat(buf, "\r\n", sizeof(buf) - strlen(buf) - 1);

    ssize_t w = write(ctrl_sock, buf, strlen(buf));
    if (w < 0) {
        perror("write");
        return -1;
    }

    /* Log enviado, ocultando password si es PASS */
    if (strncmp(buf, "PASS ", 5) == 0) {
        printf("C> PASS ********\n");
    } else {
        printf("C> %s", buf);
    }
    return 0;
}

/* Parse PASV reply: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2). */
int enter_pasv(int ctrl_sock, char *pasv_ip, int *pasv_port) {
    char reply[1024];
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code != 227) {
        fprintf(stderr, "PASV expected 227 but got %d\n", code);
        return -1;
    }
    char *p = strchr(reply, '(');
    char *q = strchr(reply, ')');
    if (!p || !q) return -1;
    char nums[256];
    size_t len = (size_t)(q - p - 1);
    if (len >= sizeof(nums)) return -1;
    strncpy(nums, p + 1, len);
    nums[len] = '\0';
    int h1,h2,h3,h4,p1,p2;
    if (sscanf(nums, "%d,%d,%d,%d,%d,%d", &h1,&h2,&h3,&h4,&p1,&p2) != 6) return -1;
    snprintf(pasv_ip, 64, "%d.%d.%d.%d", h1,h2,h3,h4);
    *pasv_port = p1*256 + p2;
    return 0;
}

/* Create an ephemeral listening socket for PORT (active mode). */
int create_port_and_listen(int *listen_sock, int *port_out) {
    int ls;
    struct sockaddr_in sa;
    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return -1; }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = 0; /* ephemeral */
    if (bind(ls, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); close(ls); return -1; }
    if (listen(ls, 1) < 0) { perror("listen"); close(ls); return -1; }
    socklen_t len = sizeof(sa);
    if (getsockname(ls, (struct sockaddr*)&sa, &len) < 0) { perror("getsockname"); close(ls); return -1; }
    *port_out = ntohs(sa.sin_port);
    *listen_sock = ls;
    return 0;
}

/* Accept incoming data connection on listen socket (active mode) */
int open_data_active(int listen_sock) {
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int ds = accept(listen_sock, (struct sockaddr*)&peer, &len);
    if (ds < 0) { perror("accept"); return -1; }
    return ds;
}

/* Download using active mode */
int download_file_active(int ctrl_sock, int listen_sock, const char *filename, long restart_offset) {
    char reply[1024];
    if (restart_offset > 0) {
        if (send_cmd(ctrl_sock, "REST %ld", restart_offset) < 0) return -1;
        int rc = get_reply(ctrl_sock, reply, sizeof(reply));
        if (rc >= 400) return -1;
    }
    if (send_cmd(ctrl_sock, "RETR %s", filename) < 0) return -1;
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) return -1;
    int data_sock = open_data_active(listen_sock);
    if (data_sock < 0) return -1;

    int fd;
    if (restart_offset > 0) {
        fd = open(filename, O_WRONLY);
        if (fd < 0) fd = open(filename, O_CREAT | O_WRONLY, 0666);
        if (fd < 0) { perror("open"); close(data_sock); return -1; }
        if (lseek(fd, restart_offset, SEEK_SET) < 0) { perror("lseek"); }
    } else {
        fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd < 0) { perror("open"); close(data_sock); return -1; }
    }
    char buf[BUFSIZE];
    ssize_t n;
    while ((n = read(data_sock, buf, sizeof(buf))) > 0) {
        write(fd, buf, n);
    }
    close(fd);
    close(data_sock);
    get_reply(ctrl_sock, reply, sizeof(reply));
    return 0;
}

/* Upload file using active mode */
int upload_file_active(int ctrl_sock, int listen_sock, const char *filename) {
    char reply[1024];
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }
    if (send_cmd(ctrl_sock, "STOR %s", filename) < 0) { close(fd); return -1; }
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) { close(fd); return -1; }
    int data_sock = open_data_active(listen_sock);
    if (data_sock < 0) { close(fd); return -1; }
    char buf[BUFSIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(data_sock, buf, n);
    }
    close(fd);
    close(data_sock);
    get_reply(ctrl_sock, reply, sizeof(reply));
    return 0;
}

/* Passive mode download */
int download_file_passive(int ctrl_sock, const char *pasv_ip, int pasv_port, const char *filename, long restart_offset) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", pasv_port);
    int data_sock = connectsock(pasv_ip, portstr, "tcp");
    if (data_sock < 0) return -1;

    char reply[1024];
    if (restart_offset > 0) {
        if (send_cmd(ctrl_sock, "REST %ld", restart_offset) < 0) { close(data_sock); return -1; }
        int rc = get_reply(ctrl_sock, reply, sizeof(reply));
        if (rc >= 400) { close(data_sock); return -1; }
    }
    if (send_cmd(ctrl_sock, "RETR %s", filename) < 0) { close(data_sock); return -1; }
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) { close(data_sock); return -1; }

    int fd;
    if (restart_offset > 0) {
        fd = open(filename, O_WRONLY);
        if (fd < 0) fd = open(filename, O_CREAT | O_WRONLY, 0666);
        if (fd < 0) { perror("open"); close(data_sock); return -1; }
        if (lseek(fd, restart_offset, SEEK_SET) < 0) { perror("lseek"); }
    } else {
        fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd < 0) { perror("open"); close(data_sock); return -1; }
    }
    char buf[BUFSIZE];
    ssize_t n;
    while ((n = read(data_sock, buf, sizeof(buf))) > 0) {
        write(fd, buf, n);
    }
    close(fd);
    close(data_sock);
    get_reply(ctrl_sock, reply, sizeof(reply));
    return 0;
}

/* Passive mode upload */
int upload_file_passive(int ctrl_sock, const char *pasv_ip, int pasv_port, const char *filename) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", pasv_port);
    int data_sock = connectsock(pasv_ip, portstr, "tcp");
    if (data_sock < 0) return -1;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { perror("open"); close(data_sock); return -1; }
    if (send_cmd(ctrl_sock, "STOR %s", filename) < 0) { close(fd); close(data_sock); return -1; }
    char reply[1024];
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) { close(fd); close(data_sock); return -1; }
    char buf[BUFSIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(data_sock, buf, n);
    }
    close(fd);
    close(data_sock);
    get_reply(ctrl_sock, reply, sizeof(reply));
    return 0;
}

/* Passive LIST */
int list_passive(int ctrl_sock, const char *pasv_ip, int pasv_port, const char *arg) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", pasv_port);
    int data_sock = connectsock(pasv_ip, portstr, "tcp");
    if (data_sock < 0) return -1;
    if (arg && strlen(arg) > 0) send_cmd(ctrl_sock, "LIST %s", arg);
    else send_cmd(ctrl_sock, "LIST");
    char reply[1024];
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) { close(data_sock); return -1; }
    char buf[BUFSIZE];
    ssize_t n;
    while ((n = read(data_sock, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(data_sock);
    get_reply(ctrl_sock, reply, sizeof(reply));
    return 0;
}

/* Active LIST */
int list_active(int ctrl_sock, int listen_sock, const char *arg) {
    if (arg && strlen(arg) > 0) send_cmd(ctrl_sock, "LIST %s", arg);
    else send_cmd(ctrl_sock, "LIST");
    char reply[1024];
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) return -1;
    int data_sock = open_data_active(listen_sock);
    if (data_sock < 0) return -1;
    char buf[BUFSIZE];
    ssize_t n;
    while ((n = read(data_sock, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(data_sock);
    get_reply(ctrl_sock, reply, sizeof(reply));
    return 0;
}

/* Obtener IP local "bonita" para PORT */
int get_first_ipv4(char *out, size_t maxlen) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in*)ifa->ifa_addr;
            const char *addr = inet_ntoa(sa->sin_addr);
            if (addr && strcmp(addr, "127.0.0.1") != 0) {
                strncpy(out, addr, maxlen-1);
                out[maxlen-1] = '\0';
                freeifaddrs(ifaddr);
                return 0;
            }
        }
    }
    freeifaddrs(ifaddr);
    return -1;
}

/* Build and send PORT command */
int send_port_command(int ctrl_sock, const char *local_ip, int port) {
    int h1,h2,h3,h4,p1,p2;
    if (sscanf(local_ip, "%d.%d.%d.%d", &h1,&h2,&h3,&h4) != 4) return -1;
    p1 = port / 256;
    p2 = port % 256;
    if (send_cmd(ctrl_sock, "PORT %d,%d,%d,%d,%d,%d", h1,h2,h3,h4,p1,p2) < 0) return -1;
    char reply[1024];
    int code = get_reply(ctrl_sock, reply, sizeof(reply));
    if (code >= 400) return -1;
    return 0;
}

/* Disable echo on terminal to hide password */
void disable_echo(struct termios *old_attr) {
    struct termios new_attr;
    tcgetattr(STDIN_FILENO, old_attr);
    new_attr = *old_attr;
    new_attr.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_attr);
}

/* Restore terminal echo */
void restore_echo(struct termios *old_attr) {
    tcsetattr(STDIN_FILENO, TCSANOW, old_attr);
}

/* REPL para comandos del usuario */
void repl(int ctrl_sock, const char *local_ip) {
    char line[1024];
    char cmd[64], arg[512];
    long restart_offset = 0; /* para REST */

    while (1) {
        printf("ftp> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        cmd[0] = '\0';
        arg[0] = '\0';

        if (sscanf(line, "%63s %511[^\n]", cmd, arg) < 1) continue;

        if (strcasecmp(cmd, "quit") == 0 || strcasecmp(cmd, "bye") == 0) {
            send_cmd(ctrl_sock, "QUIT");
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));
            break;

        } else if (strcasecmp(cmd, "user") == 0) {
            send_cmd(ctrl_sock, "USER %s", arg);
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));

        } else if (strcasecmp(cmd, "pass") == 0) {
            send_cmd(ctrl_sock, "PASS %s", arg);
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));

        } else if (strcasecmp(cmd, "pasv") == 0) {
            send_cmd(ctrl_sock, "PASV");
            char pasv_ip[64];
            int pasv_port;
            if (enter_pasv(ctrl_sock, pasv_ip, &pasv_port) == 0) {
                printf("PASV -> %s:%d\n", pasv_ip, pasv_port);
            } else {
                fprintf(stderr, "Failed to parse PASV\n");
            }

        } else if (strcasecmp(cmd, "pwd") == 0) {
            send_cmd(ctrl_sock, "PWD");
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));

        } else if (strcasecmp(cmd, "mkd") == 0) {
            send_cmd(ctrl_sock, "MKD %s", arg);
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));

        } else if (strcasecmp(cmd, "dele") == 0) {
            send_cmd(ctrl_sock, "DELE %s", arg);
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));

        } else if (strcasecmp(cmd, "rest") == 0) {
            restart_offset = atol(arg);
            printf("SET REST offset = %ld\n", restart_offset);

        } else if (strcasecmp(cmd, "rename") == 0) {
            char oldn[256], newn[256];
            if (sscanf(arg, "%255s %255s", oldn, newn) != 2) {
                fprintf(stderr, "Usage: rename <old> <new>\n");
                continue;
            }
            send_cmd(ctrl_sock, "RNFR %s", oldn);
            char rep[1024];
            int code = get_reply(ctrl_sock, rep, sizeof(rep));
            if (code >= 350 && code < 400) {
                send_cmd(ctrl_sock, "RNTO %s", newn);
                get_reply(ctrl_sock, rep, sizeof(rep));
            } else {
                fprintf(stderr, "RNFR falló (código %d)\n", code);
            }

        } else if (strcasecmp(cmd, "list") == 0 || strcasecmp(cmd, "ls") == 0) {
            if (strncmp(arg, "-a ", 3) == 0) {
                char *larg = arg + 3;
                int listen_sock, port;
                if (create_port_and_listen(&listen_sock, &port) < 0) { fprintf(stderr, "Can't listen\n"); continue; }
                if (send_port_command(ctrl_sock, local_ip, port) < 0) { fprintf(stderr, "PORT failed\n"); close(listen_sock); continue; }
                pid_t pid = fork();
                if (pid == 0) {
                    list_active(ctrl_sock, listen_sock, larg);
                    close(listen_sock);
                    _exit(0);
                } else if (pid > 0) {
                    close(listen_sock);
                    printf("Active LIST started in background (pid %d)\n", pid);
                } else perror("fork");
            } else {
                send_cmd(ctrl_sock, "PASV");
                char pasv_ip[64];
                int pasv_port;
                if (enter_pasv(ctrl_sock, pasv_ip, &pasv_port) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        list_passive(ctrl_sock, pasv_ip, pasv_port, arg);
                        _exit(0);
                    } else if (pid > 0) {
                        printf("Passive LIST started in background (pid %d)\n", pid);
                    } else perror("fork");
                } else fprintf(stderr, "PASV parse failed\n");
            }

        } else if (strcasecmp(cmd, "retr") == 0) {
            if (strncmp(arg, "-a ", 3) == 0) {
                char *fname = arg + 3;
                int listen_sock, port;
                if (create_port_and_listen(&listen_sock, &port) < 0) { fprintf(stderr, "Can't listen\n"); continue; }
                if (send_port_command(ctrl_sock, local_ip, port) < 0) { fprintf(stderr, "PORT failed\n"); close(listen_sock); continue; }
                pid_t pid = fork();
                if (pid == 0) {
                    download_file_active(ctrl_sock, listen_sock, fname, restart_offset);
                    close(listen_sock);
                    _exit(0);
                } else if (pid > 0) {
                    close(listen_sock);
                    printf("Active transfer started in background (pid %d)\n", pid);
                } else perror("fork");
                restart_offset = 0;
            } else {
                send_cmd(ctrl_sock, "PASV");
                char pasv_ip[64];
                int pasv_port;
                if (enter_pasv(ctrl_sock, pasv_ip, &pasv_port) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        download_file_passive(ctrl_sock, pasv_ip, pasv_port, arg, restart_offset);
                        _exit(0);
                    } else if (pid > 0) {
                        printf("Passive download started in background (pid %d)\n", pid);
                    } else perror("fork");
                    restart_offset = 0;
                } else {
                    fprintf(stderr, "PASV parse failed\n");
                }
            }

        } else if (strcasecmp(cmd, "stor") == 0) {
            if (strncmp(arg, "-a ", 3) == 0) {
                char *fname = arg + 3;
                int listen_sock, port;
                if (create_port_and_listen(&listen_sock, &port) < 0) { fprintf(stderr, "Can't listen\n"); continue; }
                if (send_port_command(ctrl_sock, local_ip, port) < 0) { fprintf(stderr, "PORT failed\n"); close(listen_sock); continue; }
                pid_t pid = fork();
                if (pid == 0) {
                    upload_file_active(ctrl_sock, listen_sock, fname);
                    close(listen_sock);
                    _exit(0);
                } else if (pid > 0) {
                    close(listen_sock);
                    printf("Active upload started in background (pid %d)\n", pid);
                } else perror("fork");
            } else {
                send_cmd(ctrl_sock, "PASV");
                char pasv_ip[64];
                int pasv_port;
                if (enter_pasv(ctrl_sock, pasv_ip, &pasv_port) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        upload_file_passive(ctrl_sock, pasv_ip, pasv_port, arg);
                        _exit(0);
                    } else if (pid > 0) {
                        printf("Passive upload started in background (pid %d)\n", pid);
                    } else perror("fork");
                } else {
                    fprintf(stderr, "PASV parse failed\n");
                }
            }

        } else if (strcasecmp(cmd, "port") == 0) {
            int listen_sock, port;
            if (create_port_and_listen(&listen_sock, &port) < 0) { fprintf(stderr, "Can't listen\n"); continue; }
            if (send_port_command(ctrl_sock, local_ip, port) < 0) { fprintf(stderr, "PORT failed\n"); close(listen_sock); continue; }
            printf("PORT set on %s:%d (listening). Use RETR -a <file> or STOR -a <file> to perform transfer.\n", local_ip, port);
            close(listen_sock);

        } else if (strcasecmp(cmd, "pasvinfo") == 0) {
            send_cmd(ctrl_sock, "PASV");
            char pasv_ip[64];
            int pasv_port;
            if (enter_pasv(ctrl_sock, pasv_ip, &pasv_port) == 0) {
                printf("Server PASV endpoint: %s:%d\n", pasv_ip, pasv_port);
            } else fprintf(stderr, "PASV parse failed\n");

        } else {
            /* comando crudo */
            if (strlen(arg) > 0) send_cmd(ctrl_sock, "%s %s", cmd, arg);
            else send_cmd(ctrl_sock, "%s", cmd);
            char rep[1024];
            get_reply(ctrl_sock, rep, sizeof(rep));
        }

        /* reap hijos terminados (concurrencia) */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <host> <port>\n", argv[0]);
        fprintf(stderr, "Ejemplo: %s ftp.example.com 21\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    const char *port = argv[2];

    int ctrl_sock = connectsock(host, port, "tcp");
    if (ctrl_sock < 0) {
        fprintf(stderr, "No se pudo conectar al servidor FTP\n");
        return 1;
    }

    /* welcome inicial */
    char welcome[1024];
    get_reply(ctrl_sock, welcome, sizeof(welcome));

    /* Login interactivo */
    char user[128];
    char pass[128];
    struct termios old_attr;

    printf("Usuario: ");
    fflush(stdout);
    if (!fgets(user, sizeof(user), stdin)) {
        fprintf(stderr, "Error leyendo usuario\n");
        close(ctrl_sock);
        return 1;
    }
    user[strcspn(user, "\r\n")] = '\0';

    printf("Password: ");
    fflush(stdout);
    disable_echo(&old_attr);
    if (!fgets(pass, sizeof(pass), stdin)) {
        restore_echo(&old_attr);
        fprintf(stderr, "Error leyendo password\n");
        close(ctrl_sock);
        return 1;
    }
    restore_echo(&old_attr);
    printf("\n");
    pass[strcspn(pass, "\r\n")] = '\0';

    /* Enviar USER/PASS */
    send_cmd(ctrl_sock, "USER %s", user);
    char rep[1024];
    int code = get_reply(ctrl_sock, rep, sizeof(rep));
    if (code == 331) {
        send_cmd(ctrl_sock, "PASS %s", pass);
        code = get_reply(ctrl_sock, rep, sizeof(rep));
    }

    if (code != 230) {
        fprintf(stderr, "Credenciales incorrectas (código %d)\n", code);
        close(ctrl_sock);
        return 1;
    } else {
        printf("Login exitoso como %s\n", user);
    }

    /* Modo binario para evitar problemas con REST y binarios */
    send_cmd(ctrl_sock, "TYPE I");
    get_reply(ctrl_sock, rep, sizeof(rep));

    /* IP local (para modo activo si lo usas) */
    char local_ip[64] = "127.0.0.1";
    if (get_first_ipv4(local_ip, sizeof(local_ip)) < 0) {
        /* nos quedamos con loopback */
    }
    printf("Usando IP local: %s\n", local_ip);

    repl(ctrl_sock, local_ip);

    close(ctrl_sock);
    return 0;
}

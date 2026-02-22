#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uhid.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define UHID_PATH "/dev/uhid"
#define SERVER_PORT 5555
#define MAX_CLIENTS 4

/* =========================
   HID REPORT DESCRIPTOR
   Report ID 1 = Mouse
   Report ID 2 = Keyboard
   ========================= */

static const uint8_t hid_report_desc[] = {

    /* ---------- MOUSE (Report ID 1) ---------- */
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,        //     Input (Buttons)
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,        //     Padding
    0x05, 0x01,
    0x09, 0x30,        //     X
    0x09, 0x31,        //     Y
    0x15, 0x81,        //     -127
    0x25, 0x7F,        //     +127
    0x75, 0x08,
    0x95, 0x02,
    0x81, 0x06,        //     Relative
    0xC0,
    0xC0,

    /* ---------- KEYBOARD (Report ID 2) ---------- */
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0xE0,        //   Modifiers
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,        //   Modifiers byte
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,        //   Reserved
    0x95, 0x06,        //   6 keys
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0
};

/* =========================
   UHID HELPERS
   ========================= */

static int uhid_write(int fd, struct uhid_event *ev) {
    ssize_t ret = write(fd, ev, sizeof(*ev));
    return (ret < 0) ? -1 : 0;
}

static int uhid_create(int fd) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = UHID_CREATE2;

    struct uhid_create2_req *c = &ev.u.create2;

    snprintf((char *)c->name, sizeof(c->name),
             "Virtual HID Keyboard+Mouse");

    c->bus     = BUS_USB; 
    c->vendor  = 0x1234;
    c->product = 0x5678;
    c->version = 1;
    c->country = 0;

    /* IMPORTANT: rd_data is an ARRAY, not a pointer */
    memcpy(c->rd_data, hid_report_desc, sizeof(hid_report_desc));
    c->rd_size = sizeof(hid_report_desc);

    return uhid_write(fd, &ev);
}

static void uhid_destroy(int fd) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_write(fd, &ev);
}

/* =========================
   INPUT SENDERS
   ========================= */

#define MOD_LSHIFT 0x02
#define MOD_LCTRL  0x01
#define MOD_LALT   0x04
#define MOD_LGUI   0x08

static void send_mouse(int fd, int8_t dx, int8_t dy, uint8_t buttons) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = UHID_INPUT2;

    uint8_t report[4];
    report[0] = 0x01;        // Report ID 1
    report[1] = buttons;    // Buttons
    report[2] = dx;
    report[3] = dy;

    memcpy(ev.u.input2.data, report, sizeof(report));
    ev.u.input2.size = sizeof(report);

    uhid_write(fd, &ev);
}

static void send_key(int fd, uint8_t keycode, uint8_t modifiers) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = UHID_INPUT2; // Używamy INPUT2, tak jak przy myszce

    /* 
       Struktura raportu dla klawiatury z Report ID:
       Byte 0: Report ID (0x02)
       Byte 1: Modyfikatory (Ctrl, Shift, etc.)
       Byte 2: Reserved (zazwyczaj 0x00)
       Byte 3: Keycode 1 (Twoja litera)
       Byte 4-8: Keycode 2-6 (puste)
    */
    
    uint8_t report[9]; 
    memset(report, 0, sizeof(report));

     /* --- WCIŚNIĘCIE KLAWISZA --- */
    report[0] = 0x02;     // Report ID 2 (Klawiatura)
    report[1] = modifiers;
    report[3] = keycode;  // Klawisz ląduje w 4. bajcie (index 3)

    memcpy(ev.u.input2.data, report, sizeof(report));
    ev.u.input2.size = sizeof(report);

    uhid_write(fd, &ev);

    /* --- PUSZCZENIE KLAWISZA --- */
    usleep(10000); // Krótkie opóźnienie, by system zarejestrował wciśnięcie

    memset(report, 0, sizeof(report));
    report[0] = 0x02; // Report ID musi być zawsze, nawet przy puszczaniu!
    // Reszta zerami oznacza brak wciśniętych klawiszy

    memcpy(ev.u.input2.data, report, sizeof(report));
    ev.u.input2.size = sizeof(report);

    uhid_write(fd, &ev);
}

struct key_event {
    uint8_t code;
    uint8_t modifier;
};

static struct key_event char_to_uhid(char c)
{
    struct key_event ev = {0, 0};

    /* a-z */
    if (c >= 'a' && c <= 'z') {
        ev.code = 0x04 + (c - 'a');  // HID: a=0x04
        return ev;
    }

    /* A-Z */
    if (c >= 'A' && c <= 'Z') {
        ev.code = 0x04 + (c - 'A');
        ev.modifier = MOD_LSHIFT;
        return ev;
    }

    /* 1-9 */
    if (c >= '1' && c <= '9') {
        ev.code = 0x1E + (c - '1');  // HID: 1=0x1E
        return ev;
    }

    /* 0 */
    if (c == '0') {
        ev.code = 0x27;
        return ev;
    }

    switch (c) {
        case '!': ev.code = 0x1E; ev.modifier = MOD_LSHIFT; break;
        case '@': ev.code = 0x1F; ev.modifier = MOD_LSHIFT; break;
        case '#': ev.code = 0x20; ev.modifier = MOD_LSHIFT; break;
        case '$': ev.code = 0x21; ev.modifier = MOD_LSHIFT; break;
        case '%': ev.code = 0x22; ev.modifier = MOD_LSHIFT; break;
        case '^': ev.code = 0x23; ev.modifier = MOD_LSHIFT; break;
        case '&': ev.code = 0x24; ev.modifier = MOD_LSHIFT; break;
        case '*': ev.code = 0x25; ev.modifier = MOD_LSHIFT; break;
        case '(': ev.code = 0x26; ev.modifier = MOD_LSHIFT; break;
        case ')': ev.code = 0x27; ev.modifier = MOD_LSHIFT; break;

        case '-': ev.code = 0x2D; break;
        case '_': ev.code = 0x2D; ev.modifier = MOD_LSHIFT; break;
        case '=': ev.code = 0x2E; break;
        case '+': ev.code = 0x2E; ev.modifier = MOD_LSHIFT; break;

        case '[': ev.code = 0x2F; break;
        case '{': ev.code = 0x2F; ev.modifier = MOD_LSHIFT; break;
        case ']': ev.code = 0x30; break;
        case '}': ev.code = 0x30; ev.modifier = MOD_LSHIFT; break;
        case '\\': ev.code = 0x31; break;
        case '|': ev.code = 0x31; ev.modifier = MOD_LSHIFT; break;

        case ';': ev.code = 0x33; break;
        case ':': ev.code = 0x33; ev.modifier = MOD_LSHIFT; break;
        case '\'': ev.code = 0x34; break;
        case '"': ev.code = 0x34; ev.modifier = MOD_LSHIFT; break;

        case '`': ev.code = 0x35; break;
        case '~': ev.code = 0x35; ev.modifier = MOD_LSHIFT; break;

        case ',': ev.code = 0x36; break;
        case '<': ev.code = 0x36; ev.modifier = MOD_LSHIFT; break;
        case '.': ev.code = 0x37; break;
        case '>': ev.code = 0x37; ev.modifier = MOD_LSHIFT; break;
        case '/': ev.code = 0x38; break;
        case '?': ev.code = 0x38; ev.modifier = MOD_LSHIFT; break;

        case ' ': ev.code = 0x2C; break;
        case '\n': ev.code = 0x28; break;
        case '\r': ev.code = 0x28; break;
        case '\t': ev.code = 0x2B; break;
        case '\b': ev.code = 0x2A; break;
        case 0x1B: ev.code = 0x29; break; // Escape

        default: break;
    }

    return ev;
}

/* =========================
   NETWORK HELPERS
   ========================= */

struct client {
    int fd;
    char buf[1024];
    size_t len;
};

static int setup_server(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail;
    if (listen(s, MAX_CLIENTS) < 0)
        goto fail;

    return s;
fail:
    close(s);
    return -1;
}

static void close_client(struct client *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->len = 0;
}

static void process_line(int uhid_fd, const char *line)
{
    /* ALT+TAB to switch windows */
    if (strcmp(line, "ALT_TAB") == 0) {
        send_key(uhid_fd, 0x2B, MOD_LALT); // 0x2B = Tab
        return;
    }

    /* M dx dy [buttons] */
    if (line[0] == 'M') {
        int dx = 0, dy = 0, buttons = 0;
        if (sscanf(line + 1, "%d %d %d", &dx, &dy, &buttons) >= 2)
            send_mouse(uhid_fd, (int8_t)dx, (int8_t)dy, (uint8_t)buttons);
        return;
    }

    /* Default: treat as text to type */
    for (const char *p = line; *p; ++p) {
        struct key_event key = char_to_uhid(*p);
        if (key.code)
            send_key(uhid_fd, key.code, key.modifier);
    }
}


/* =========================
   MAIN
   ========================= */

int main(void) {
    int fd = open(UHID_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/uhid");
        fprintf(stderr, "Try: sudo modprobe uhid\n");
        return 1;
    }

    if (uhid_create(fd) < 0) {
        perror("UHID_CREATE2");
        close(fd);
        return 1;
    }

    printf("Virtual HID device created\n");
printf("Waiting for UHID_START...\n");

/* wait for START */
while (1) {
    struct uhid_event ev;
    if (read(fd, &ev, sizeof(ev)) > 0 && ev.type == UHID_START)
        break;
}

printf("START OK - sending keys\n");



    int server_fd = setup_server();
    if (server_fd >= 0)
        printf("Listening on 0.0.0.0:%d for keyboard/mouse input\n", SERVER_PORT);
    else
        fprintf(stderr, "WARNING: network input disabled (bind/listen failed)\n");

    struct client clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i)
        clients[i].fd = -1;

    send_mouse(fd, 30, 10, 0);
    sleep(1);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);

        int maxfd = fd;

        FD_SET(STDIN_FILENO, &rfds);
        if (STDIN_FILENO > maxfd)
            maxfd = STDIN_FILENO;

        if (server_fd >= 0) {
            FD_SET(server_fd, &rfds);
            if (server_fd > maxfd)
                maxfd = server_fd;
        }

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > maxfd)
                    maxfd = clients[i].fd;
            }
        }

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (server_fd >= 0 && FD_ISSET(server_fd, &rfds)) {
            int cfd = accept(server_fd, NULL, NULL);
            if (cfd >= 0) {
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = cfd;
                        clients[i].len = 0;
                        placed = 1;
                        break;
                    }
                }
                if (!placed)
                    close(cfd);
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[128];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                for (ssize_t i = 0; i < n; ++i) {
                    struct key_event key = char_to_uhid(buf[i]);
                    if (key.code)
                        send_key(fd, key.code, key.modifier);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            struct client *c = &clients[i];
            if (c->fd < 0 || !FD_ISSET(c->fd, &rfds))
                continue;

            char tmp[256];
            ssize_t n = read(c->fd, tmp, sizeof(tmp));
            if (n <= 0) {
                close_client(c);
                continue;
            }

            for (ssize_t k = 0; k < n; ++k) {
                char ch = tmp[k];
                if (ch == '\r')
                    continue;
                if (ch == '\n') {
                    c->buf[c->len] = '\0';
                    process_line(fd, c->buf);
                    c->len = 0;
                    continue;
                }
                if (c->len + 1 < sizeof(c->buf))
                    c->buf[c->len++] = ch;
                else
                    c->len = 0; // drop overlong line
            }
        }
    }

    if (server_fd >= 0)
        close(server_fd);
    for (int i = 0; i < MAX_CLIENTS; ++i)
        close_client(&clients[i]);

    uhid_destroy(fd);
    close(fd);

    return 0;
}

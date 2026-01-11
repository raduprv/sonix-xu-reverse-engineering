#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/uvcvideo.h>
#include <linux/usb/video.h>
#include <pthread.h>

typedef enum
{
    VIEW_EXPLORER,
    VIEW_GOTO,
    VIEW_HEX_EDIT
} view_mode_t;

/* Global State */
int fd_cam;
uint8_t ram[8192], ram_old[8192];
uint16_t win_start = 0x1000, cursor_addr = 0x1000;
uint8_t staging_val = 0;
view_mode_t current_view = VIEW_EXPLORER;
int running = 1, win_dirty = 0;
char last_err[128] = "System Ready", goto_buf[5] = "";
pthread_mutex_t ioctl_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- XU FUNCTIONS --- */

/**
 * Reads a register value from the Sonix bridge using UVC XU queries
 */
int sonix_read_reg(int fd, uint16_t reg)
{
    uint8_t data[4] = {reg & 0xFF, (reg >> 8) & 0xFF, 0x00, 0xFF};
    struct uvc_xu_control_query q = {.unit = 3, .selector = 1, .size = 4, .query = UVC_SET_CUR, .data = data};

    pthread_mutex_lock(&ioctl_lock);

    if (ioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0)
    {
        pthread_mutex_unlock(&ioctl_lock);
        return -1;
    }

    usleep(1200);
    q.query = UVC_GET_CUR;

    if (ioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0)
    {
        pthread_mutex_unlock(&ioctl_lock);
        return -1;
    }

    pthread_mutex_unlock(&ioctl_lock);
    return data[2];
}

/**
 * Writes a value to a Sonix bridge register and verifies readback
 */
int sonix_write_reg(int fd, uint16_t reg, uint8_t val)
{
    uint8_t data[4] = {reg & 0xFF, (reg >> 8) & 0xFF, val, 0x00};
    struct uvc_xu_control_query q = {.unit = 3, .selector = 1, .size = 4, .data = data};

    pthread_mutex_lock(&ioctl_lock);

    ioctl(fd, UVCIOC_CTRL_QUERY, (q.query = UVC_SET_CUR, &q));
    data[3] = 0xFF; /* Trigger readback mode */

    ioctl(fd, UVCIOC_CTRL_QUERY, (q.query = UVC_SET_CUR, &q));
    ioctl(fd, UVCIOC_CTRL_QUERY, (q.query = UVC_GET_CUR, &q));

    pthread_mutex_unlock(&ioctl_lock);

    return (data[2] == val) ? 0 : 1;
}

/**
 * Background thread to continuously poll RAM values
 */
void *fetcher_thread(void *arg)
{
    while (running)
    {
        if (current_view == VIEW_GOTO)
        {
            usleep(100000);
            continue;
        }

        uint16_t base = win_start;
        for (int i = 0; i < 256 && running; i++)
        {
            if (win_dirty)
            {
                win_dirty = 0;
                break;
            }

            int val = sonix_read_reg(fd_cam, base + i);
            usleep(1000);

            if (val >= 0)
            {
                ram_old[base + i] = ram[base + i];
                ram[base + i] = (uint8_t)val;
            }
        }
        usleep(10000);
    }
    return NULL;
}

/**
 * Fuzzes the current 16-byte line with random values
 */
void fuzz_line()
{
    uint16_t start_row = cursor_addr & 0xFFF0;
    for (int i = 0; i < 16; i++)
    {
        uint8_t val = rand() % 256;
        if (sonix_write_reg(fd_cam, start_row + i, val) == 0)
        {
            ram[start_row + i] = val;
        }
        usleep(5000); /* 5ms delay to prevent choking the bridge */
    }
    strcpy(last_err, "Line Fuzz Complete!");
}

/**
 * Renders the terminal UI using ANSI escape codes
 */
void draw_ui()
{
    printf("\033[H\033[1;37;44m SN9C286 EXPLORER | CURSOR: 0x%04X | VAL: 0x%02X \033[K\033[0m\n\n", cursor_addr, ram[cursor_addr]);

    for (int r = 0; r < 16; r++)
    {
        uint16_t row = win_start + (r * 16);
        printf("  %04X:  ", row);

        for (int c = 0; c < 16; c++)
        {
            uint16_t addr = row + c;
            if (addr == cursor_addr) printf("\033[7m");
            if (ram[addr] != ram_old[addr]) printf("\033[32m");
            printf("%02X\033[0m ", ram[addr]);
        }

        /* Render Scrollbar */
        int thumb = (win_start * 15) / (8192 - 256);
        printf(r == thumb ? " \033[33m█\033[0m\n" : " \033[90m▒\033[0m\n");
    }

    /* Line 20: Data Editing Row */
    printf("\033[K ADDR: 0x%04X | HEX: [", cursor_addr);
    if (current_view == VIEW_HEX_EDIT)
    {
        printf("\033[1;37;45m 0x%02X \033[0m", staging_val);
    }
    else
    {
        printf("\033[1;33m 0x%02X \033[0m", staging_val);
    }

    printf("] | BITS: ");
    for (int i = 7; i >= 0; i--)
    {
        if ((staging_val >> i) & 1) printf("\033[1;37m%d \033[0m", i);
        else printf("\033[90m%d \033[0m", i);
    }
    printf(" [\033[1;32mSET\033[0m]\n");

    /* Line 21: Action Buttons */
    printf("\033[K [DUMP]  [GOTO]  [FUZZ-LN]  [EXIT]\n");
    printf("\033[K LOG: %s\033[K", last_err);

    /* Goto Dialog Overlay */
    if (current_view == VIEW_GOTO)
    {
        printf("\033[10;20H\033[1;37;45m  GOTO: 0x%s_  \033[0m", goto_buf);
    }

    fflush(stdout);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        return printf("Usage: %s /dev/videoX\n", argv[0]), 1;
    }

    if ((fd_cam = open(argv[1], O_RDWR)) < 0)
    {
        return perror("Open"), 1;
    }

    /* Set raw terminal mode */
    struct termios oldt, newt;
    tcgetattr(0, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &newt);

    /* Enable Alternate Buffer, Mouse Tracking (SGR), and Hide Cursor */
    printf("\033[?1049h\033[?1000h\033[?1006h\033[?25l");

    pthread_t tid;
    pthread_create(&tid, NULL, fetcher_thread, NULL);

    while (running)
    {
        draw_ui();

        struct timeval tv = {0, 30000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);

        if (select(1, &fds, NULL, NULL, &tv) > 0)
        {
            char b[64];
            int n = read(0, b, sizeof(b));
            if (n <= 0) continue;

            /* Handle View: GOTO */
            if (current_view == VIEW_GOTO)
            {
                if (b[0] == 10 || b[0] == 13)
                {
                    win_start = (uint16_t)strtol(goto_buf, NULL, 16) & 0xFFF0;
                    win_dirty = 1;
                    current_view = VIEW_EXPLORER;
                    goto_buf[0] = 0;
                }
                else if (b[0] == 27)
                {
                    current_view = VIEW_EXPLORER;
                    goto_buf[0] = 0;
                }
                else if (n == 1 && strlen(goto_buf) < 4)
                {
                    strncat(goto_buf, b, 1);
                }
                continue;
            }

            /* Handle View: HEX_EDIT */
            if (current_view == VIEW_HEX_EDIT)
            {
                if (b[0] == 10 || b[0] == 13 || b[0] == 27)
                {
                    current_view = VIEW_EXPLORER;
                }
                else if ((b[0] >= '0' && b[0] <= '9') || 
                         (b[0] >= 'a' && b[0] <= 'f') || 
                         (b[0] >= 'A' && b[0] <= 'F'))
                {
                    uint8_t nib = (b[0] >= 'a') ? (b[0] - 'a' + 10) : 
                                  (b[0] >= 'A') ? (b[0] - 'A' + 10) : (b[0] - '0');
                    staging_val = (staging_val << 4) | nib;
                }
                continue;
            }

            /* Main Explorer Navigation */
            if (b[0] == 7) /* Ctrl+G */
            {
                current_view = VIEW_GOTO;
                continue;
            }

            if (b[0] == 'q') running = 0;

            /* Page Up (ANSI: ESC [ 5 ~) */
            if (n >= 4 && b[0] == 27 && b[1] == '[' && b[2] == '5')
            {
                if (win_start >= 256) win_start -= 256;
                else win_start = 0;
                win_dirty = 1;
                continue;
            }

            if (b[0] == 'r' || b[0] == 'R')
            {
                fuzz_line();
            }

            /* Page Down (ANSI: ESC [ 6 ~) */
            if (n >= 4 && b[0] == 27 && b[1] == '[' && b[2] == '6')
            {
                if (win_start <= (8192 - 512)) win_start += 256;
                else win_start = 8192 - 256;
                win_dirty = 1;
                continue;
            }

            /* Mouse Events (SGR Mode) */
            if (n > 5 && b[0] == '\033' && b[2] == '<')
            {
                int btn, x, y;
                char mode;
                sscanf(b + 3, "%d;%d;%d%c", &btn, &x, &y, &mode);

                /* Scroll Wheel */
                if (btn == 64)
                {
                    if (win_start >= 16) win_start -= 16;
                    else win_start = 0;
                    win_dirty = 1;
                }
                else if (btn == 65)
                {
                    if (win_start < (8192 - 256)) win_start += 16;
                    else win_start = 8192 - 256;
                    win_dirty = 1;
                }
                /* Click Events on Release */
                else if (mode == 'm')
                {
                    /* Click on Hex Grid */
                    if (y >= 3 && y <= 18 && x >= 10 && x <= 57)
                    {
                        cursor_addr = win_start + (y - 3) * 16 + (x - 10) / 3;
                        staging_val = ram[cursor_addr];
                    }
                    /* Click on Scrollbar */
                    else if (y >= 3 && y <= 18 && x >= 59)
                    {
                        win_start = ((y - 2) * 512) & 0x1FF0;
                        win_dirty = 1;
                    }
                    /* Click on Data Edit Row */
                    else if (y == 19)
                    {
                        if (x >= 20 && x <= 28) current_view = VIEW_HEX_EDIT;
                        else if (x >= 39 && x <= 59)
                        {
                            if (x >= 55) /* [SET] Button */
                            {
                                if (sonix_write_reg(fd_cam, cursor_addr, staging_val) == 0)
                                    strcpy(last_err, "Write OK");
                                else
                                    strcpy(last_err, "Fail");
                            }
                            else /* Bit Toggles */
                            {
                                int bit_idx = 7 - ((x - 39) / 2);
                                if (bit_idx >= 0 && bit_idx <= 7) staging_val ^= (1 << bit_idx);
                            }
                        }
                    }
                    /* Click on Bottom Action Row */
                    else if (y == 20)
                    {
                        if (x >= 1 && x <= 7)
                        {
                            FILE *f = fopen("dump.bin", "wb");
                            if (f)
                            {
                                fwrite(ram, 1, 8192, f);
                                fclose(f);
                                strcpy(last_err, "Dump Saved");
                            }
                        }
                        else if (x >= 9 && x <= 15) current_view = VIEW_GOTO;
                        else if (x >= 17 && x <= 25)
                        {
                            fuzz_line();
                        }
                        else if (x >= 27 && x <= 33) running = 0;
                    }
                }
            }
        }
    }

    /* Cleanup and exit Alternate Buffer */
    running = 0;
    pthread_join(tid, NULL);
    printf("\033[?25h\033[?1000l\033[?1006l\033[?1049l");
    tcsetattr(0, TCSANOW, &oldt);
    return 0;
}

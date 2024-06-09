#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Simplelink includes
#include "simplelink.h"

// Driverlib includes
#include "hw_types.h"
#include "hw_ints.h"
#include "rom.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "utils.h"
#include "uart.h"

// Includes from IR Remote Main
#include "hw_nvic.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "hw_apps_rcm.h"
#include "systick.h"
#include "spi.h"
#include "uart.h"
#include "pin_mux_config.h"
#include "oled_test.h"
#include "Adafruit_SSD1351.h"
#include "glcdfont.h"
#include "Adafruit_GFX.h"
#include "gpio.h"

// Common interface includes
#include "gpio_if.h"
#include "common.h"
#include "uart_if.h"
#include "i2c_if.h"
#include "adc.h"
#include "pin.h"

// Custom includes
#include "utils/network_utils.h"

// Constants
#define DATE                28    /* Current Date */
#define MONTH               5     /* Month 1-12 */
#define YEAR                2024  /* Current year */
#define HOUR                11    /* Time - hours */
#define MINUTE              10    /* Time - minutes */
#define SECOND              10    /* Time - seconds */

#define APPLICATION_NAME      "SSL"
#define APPLICATION_VERSION   "SQ24"
#define SERVER_NAME           "a1ytls0rwh8mrv-ats.iot.us-east-2.amazonaws.com"
#define GOOGLE_DST_PORT       8443
#define PORT                  80
#define BUFFER_SIZE           4096
#define SYSCLKFREQ            80000000ULL
#define SYSTICK_RELOAD_VAL    1600000UL
#define SPI_IF_BIT_RATE       20000000

#define SUCCESS               0
#define RET_IF_ERR(Func)      {int iRetVal = (Func); if (SUCCESS != iRetVal) return iRetVal;}

#if defined(ccs) || defined(gcc)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif

// Global Variables
char buffer[BUFFER_SIZE];
volatile int systick_cnt = 0;
volatile int sel_delay_cnt = 0;
volatile uint8_t tick = 0;
volatile int total_time = 0;



typedef struct {
    uint8_t length;
    uint8_t thickness;
    uint8_t x, y;
} Platform;

typedef struct {
    Platform plat;
    uint8_t x_min, x_max;
} MovablePlatform;

Platform static_plats[20][20];
MovablePlatform mov_plats[20][20];

// Function Prototypes
static void BoardInit(void);
static void SysTickInit(void);
static inline void SysTickReset(void);
static void SysTickHandler(void);
static int set_time(void);
int http_map_download(const char *path);
void console_map(uint64_t *map);
void map_fillCircle(int x_pos, int y_pos, int radius, uint8_t delete, uint64_t *map);
void map_draw(uint64_t *map, uint64_t *prev_map, unsigned int color);
Platform create_static_platform(uint8_t x, uint8_t y, uint8_t length, uint8_t thickness);
MovablePlatform create_mov_platform(uint8_t x, uint8_t y, uint8_t length, uint8_t thickness, uint8_t x_min, uint8_t x_max);
void load_level(uint64_t *map, uint64_t *prev_map, Platform *st_plats, uint8_t num_st_plats, MovablePlatform *mov_plats, uint8_t num_mov_plats);
void update_platforms(MovablePlatform *mov_plats, uint8_t num_plats, int tilt, uint64_t *map);

// Board Initialization
static void BoardInit(void) {
    #ifndef USE_TIRTOS
    #if defined(ccs)
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
    #elif defined(ewarm)
    MAP_IntVTableBaseSet((unsigned long)&__vector_table);
    #endif
    #endif
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);
    PRCMCC3200MCUInit();
}

// SysTick Initialization
static void SysTickInit(void) {
    MAP_SysTickPeriodSet(SYSTICK_RELOAD_VAL);
    MAP_SysTickIntRegister(SysTickHandler);
    MAP_SysTickIntEnable();
    MAP_SysTickEnable();
}

// SysTick Reset
static inline void SysTickReset(void) {
    HWREG(NVIC_ST_CURRENT) = 1;
    systick_cnt = 0;
}

// SysTick Handler
static void SysTickHandler(void) {
    tick = 1;
    systick_cnt++;
    sel_delay_cnt++;
    total_time++;
}

// Set Time
static int set_time(void) {
    SlDateTime g_time;
    long retVal;

    g_time.tm_day = DATE;
    g_time.tm_mon = MONTH;
    g_time.tm_year = YEAR;
    g_time.tm_sec = SECOND;
    g_time.tm_hour = HOUR;
    g_time.tm_min = MINUTE;

    retVal = sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION,
                       SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME,
                       sizeof(SlDateTime), (unsigned char *)(&g_time));

    if (retVal < 0) return retVal;
    return SUCCESS;
}

// HTTP Map Download
int http_map_download(const char *path) {
    int sockfd;
    struct sockaddr_in server_addr;
    _u32 ip_addr;
    _i8 *host = "mapdownloadsluke.s3.us-east-2.amazonaws.com";
    _u16 host_len = strlen(host);
    _u8 family = SL_AF_INET;

    if (sl_NetAppDnsGetHostByName(host, host_len, &ip_addr, family) < 0) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", host);
        return 1;
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(ip_addr);

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sockfd);
        return 1;
    }

    snprintf(buffer, sizeof(buffer),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);

    if (send(sockfd, buffer, strlen(buffer), 0) < 0) {
        perror("Send failed");
        close(sockfd);
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    int bytes_received;
    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        //printf("%s", buffer);
    }

    close(sockfd);
    return 0;
}

int GetTilt(unsigned char ucDevAddr, unsigned char ucRegOffset, unsigned char ucRdLen, unsigned char *aucRdDataBuf) {
    // Write the register address to be read from.
    // Stop bit implicitly assumed to be 0.
    RET_IF_ERR(I2C_IF_Write(ucDevAddr,&ucRegOffset,1,0));

    // Read the specified length of data
    RET_IF_ERR(I2C_IF_Read(ucDevAddr, &aucRdDataBuf[0], ucRdLen));

    return SUCCESS;
}

// Console Map
void console_map(uint64_t *map) {
    int i, j;
    for (i = 0; i < 256; i++) {
        for (j = 0; j < 64; j++) {
            printf("%c", ((map[i] << j) & 0x8000000000000000) ? '1' : '0');
        }
        if (i % 2 == 1) printf("\n");
    }
}

// Fill Circle in Map
void map_fillCircle(int x_pos, int y_pos, int radius, uint8_t delete, uint64_t *map) {
    int x, y;
    for (y = y_pos - radius; y < y_pos + radius; y++) {
        for (x = x_pos - radius; x < x_pos + radius; x++) {
            if ((x - x_pos) * (x - x_pos) + (y - y_pos) * (y - y_pos) < radius * radius) {
                if (delete) {
                    uint64_t delete_mask = (0xFFFFFFFFFFFFFFFF << (64 - (x) % 64)) | (0xFFFFFFFFFFFFFFFF >> ((x) % 64) + 1);
                    map[(y) * 2 + ((x) / 64)] &= delete_mask;
                } else {
                    map[(y) * 2 + ((x) / 64)] |= 0x8000000000000000 >> (x % 64);
                }
            }
        }
    }
}

// Draw Map
void map_draw(uint64_t *map, uint64_t *prev_map, unsigned int color) {
    unsigned int i, j;
    for (i = 0; i < 256; i++) {
        if (map[i] != prev_map[i]) {
            uint64_t map_row = map[i], prev_map_row = prev_map[i];
            uint64_t diff = map_row ^ prev_map_row;
            for (j = 0; j < 64; j++) {
                if ((diff << j) & 0x8000000000000000) {
                    if ((map_row << j) & 0x8000000000000000) {
                        drawPixel((i % 2) * 64 + j, i / 2, color);
                    } else {
                        drawPixel((i % 2) * 64 + j, i / 2, BLACK);
                    }
                }
            }
        }
    }
}

// Create Static Platform
Platform create_static_platform(uint8_t x, uint8_t y, uint8_t length, uint8_t thickness) {
    Platform plat = {length, thickness, x, y};
    return plat;
}

// Create Movable Platform
MovablePlatform create_mov_platform(uint8_t x, uint8_t y, uint8_t length, uint8_t thickness, uint8_t x_min, uint8_t x_max) {
    MovablePlatform mov_plat = {{length, thickness, x, y}, x_min, x_max};
    return mov_plat;
}

// Load Level
void load_level(uint64_t *map, uint64_t *prev_map, Platform *st_plats, uint8_t num_st_plats, MovablePlatform *mov_plats, uint8_t num_mov_plats) {
    uint8_t i, j, k;
    int map_idx;
    for (map_idx = 0; map_idx < 256; map_idx++) {
        map[map_idx] = 0;
    }
    for (i = 0; i < num_st_plats; i++) {
        for (k = 0; k < st_plats[i].thickness; k++) {
            for (j = 0; j < st_plats[i].length; j++) {
                uint8_t y = st_plats[i].y + k;
                uint8_t x = st_plats[i].x + j;
                uint64_t mask = 0x8000000000000000 >> (x % 64);
                map[(y) * 2 + ((x) / 64)] |= mask;
                uint64_t delete_mask = (0xFFFFFFFFFFFFFFFF << (64 - (x) % 64)) | (0xFFFFFFFFFFFFFFFF >> ((x) % 64) + 1);
                prev_map[(y) * 2 + ((x) / 64)] &= delete_mask;
            }
        }
    }
    for (i = 0; i < num_mov_plats; i++) {
        for (k = 0; k < mov_plats[i].plat.thickness; k++) {
            for (j = 0; j < mov_plats[i].plat.length; j++) {
                uint8_t y = mov_plats[i].plat.y + k;
                uint8_t x = mov_plats[i].plat.x + j;
                uint64_t mask = 0x8000000000000000 >> (x % 64);
                map[(y) * 2 + ((x) / 64)] |= mask;
                uint64_t delete_mask = (0xFFFFFFFFFFFFFFFF << (64 - (x) % 64)) | (0xFFFFFFFFFFFFFFFF >> ((x) % 64) + 1);
                prev_map[(y) * 2 + ((x) / 64)] &= delete_mask;
            }
        }
    }
}

// Update Platforms
void update_platforms(MovablePlatform *mov_plats, uint8_t num_plats, int tilt, uint64_t *map) {
    int i, y, x;
    if (tilt != 0) {
        for (i = 0; i < num_plats; i++) {
            mov_plats[i].plat.x += tilt;
            if (mov_plats[i].plat.x > mov_plats[i].x_max) {
                mov_plats[i].plat.x = mov_plats[i].x_max;
            } else if (mov_plats[i].plat.x < mov_plats[i].x_min) {
                mov_plats[i].plat.x = mov_plats[i].x_min;
            }
            for (y = mov_plats[i].plat.y; y < mov_plats[i].plat.y + mov_plats[i].plat.thickness; y++) {
                for (x = mov_plats[i].x_min; x < mov_plats[i].x_max + mov_plats[i].plat.length; x++) {
                    if (x < mov_plats[i].plat.x || x > mov_plats[i].plat.x + mov_plats[i].plat.length) {
                        uint64_t delete_mask = (0xFFFFFFFFFFFFFFFF << (64 - (x) % 64)) | (0xFFFFFFFFFFFFFFFF >> ((x) % 64) + 1);
                        map[(y) * 2 + ((x) / 64)] &= delete_mask;
                    } else {
                        map[(y) * 2 + ((x) / 64)] |= 0x8000000000000000 >> (x % 64);
                    }
                }
            }
        }
    }
}

void start_menu(char mode_names[2][10], int num_modes, unsigned long uiChannel, uint8_t *mode) {
    setTextSize(2);
    int mode_idx;
    unsigned long ulSample;
    float x_voltage;
    unsigned int highlight_color = WHITE;

    for (mode_idx = 0; mode_idx < num_modes; mode_idx++) {
        setCursor(10, 5 + mode_idx * 20);
        Outstr(mode_names[mode_idx]);
    }
    while (1) {
        if (MAP_ADCFIFOLvlGet(ADC_BASE, uiChannel)) {
            ulSample = MAP_ADCFIFORead(ADC_BASE, uiChannel);
            x_voltage = (((float)((ulSample >> 2) & 0x0FFF)) * 1.4) / 4096;
            if (x_voltage < 0.4) {
                fillScreen(BLACK);
                break;
            }
            // Check joystick y-value
            if (GPIOPinRead(GPIOA0_BASE, 0x80) && sel_delay_cnt >= 20) {
                uint8_t x = 5, y = 20 * (*mode);
                fillRect(x, y, strlen(mode_names[*mode]) * 12 + 5, 3, BLACK);
                fillRect(x - 3, y, 3, 23, BLACK);
                fillRect(x - 3, y + 20, strlen(mode_names[*mode]) * 12 + 11, 3, BLACK);
                fillRect(x + strlen(mode_names[*mode]) * 12 + 5, y, 3, 20, BLACK);
                *mode = (*mode == 0) ? num_modes - 1 : *mode - 1;
                sel_delay_cnt = 0;
                systick_cnt = 30;
                highlight_color = WHITE;
            }
            // Blinking box around selected map
            if (systick_cnt >= 30) {
                uint8_t x = 5, y = 20 * (*mode);
                fillRect(x, y, strlen(mode_names[*mode]) * 12 + 5, 3, highlight_color);
                fillRect(x - 3, y, 3, 23, highlight_color);
                fillRect(x - 3, y + 20, strlen(mode_names[*mode]) * 12 + 11, 3, highlight_color);
                fillRect(x + strlen(mode_names[*mode]) * 12 + 5, y, 3, 20, highlight_color);
                highlight_color = (highlight_color == BLACK) ? WHITE : BLACK;
                SysTickReset();
            }
        }
    }
}

int WIFI_connect(uint8_t *connected) {
    if (*connected) {
        return 0;
    }
    // Connect to WIFI
    long lRetVal = -1;

    setTextSize(1);
    setCursor(10, 30);
    Outstr("Connecting to WIFI");
    g_app_config.host = SERVER_NAME;
    g_app_config.port = GOOGLE_DST_PORT;
    lRetVal = connectToAccessPoint();
    lRetVal = set_time();
    if (lRetVal < 0) {
        UART_PRINT("Unable to set time in the device");
        setCursor(10, 60);
        Outstr("Connection Failed");
        MAP_UtilsDelay(40000000);
        fillScreen(BLACK);
        return -1;
    }

    lRetVal = tls_connect();
    if (lRetVal < 0) {
        ERR_PRINT(lRetVal);
        setCursor(10, 60);
        Outstr("Connection Failed");
        MAP_UtilsDelay(40000000);
        fillScreen(BLACK);
        return -1;
    }
    fillScreen(BLACK);
    *connected = 1;
    return 0;

}

void download_file(char *map_names_txt, char **content) {
    while (*content == NULL) {
        Report("Trying download of map names\r\n");
        http_map_download(map_names_txt);
        *content = strstr(buffer, "close");
        if (*content == NULL) {
            *content = buffer;
        } else {
            *content += 5;
        }
        //*content = strstr(buffer, "\r\n\r\n");
    }

    while (**content == '\t' || **content == '\n' || **content == '\v' || **content == '\f' || **content == '\r' || **content == ' ') {
        *content = *content + 1;
    }
}

void parse_map_names(char map_names[20][20], char *content, int *num_maps) {
    int idx = 0, map_char_idx = 0;
    *num_maps = 0;
    while (1) {
        if (content[idx] == '_') {
            map_names[*num_maps][map_char_idx] = '\0';
            *num_maps = *num_maps + 1;
            map_char_idx = 0;
            idx += 10;
        }
        if (content[idx] == '\0') return;
        map_names[*num_maps][map_char_idx++] = content[idx];
        idx++;
    }
}

int map_menu(char map_names[20][20], int num_maps, unsigned long uiChannel) {
    uint8_t map_sel = num_maps - 1;
    int map_idx;
    unsigned long ulSample;
    float x_voltage;
    unsigned int highlight_color = WHITE;

    setTextSize(2);
    for (map_idx = 0; map_idx < num_maps; map_idx++) {
        setCursor(10, 5 + map_idx * 20);
        Outstr(map_names[map_idx]);
    }

    // Map selection menu
    while (1) {
        if (MAP_ADCFIFOLvlGet(ADC_BASE, uiChannel)) {
            ulSample = MAP_ADCFIFORead(ADC_BASE, uiChannel);
            x_voltage = (((float)((ulSample >> 2) & 0x0FFF)) * 1.4) / 4096;
            if (x_voltage < 0.4) {
                fillScreen(BLACK);
                break;
            }
            if (GPIOPinRead(GPIOA0_BASE, 0x80) && sel_delay_cnt >= 20) {
                uint8_t x = 5, y = 20 * map_sel;
                fillRect(x, y, strlen(map_names[map_sel]) * 12 + 5, 3, BLACK);
                fillRect(x - 3, y, 3, 23, BLACK);
                fillRect(x - 3, y + 20, strlen(map_names[map_sel]) * 12 + 11, 3, BLACK);
                fillRect(x + strlen(map_names[map_sel]) * 12 + 5, y, 3, 20, BLACK);
                map_sel = (map_sel == 0) ? num_maps - 1 : map_sel - 1;
                sel_delay_cnt = 0;
                systick_cnt = 30;
                highlight_color = WHITE;
            }
            if (systick_cnt >= 30) {
                uint8_t x = 5, y = 20 * map_sel;
                fillRect(x, y, strlen(map_names[map_sel]) * 12 + 5, 3, highlight_color);
                fillRect(x - 3, y, 3, 23, highlight_color);
                fillRect(x - 3, y + 20, strlen(map_names[map_sel]) * 12 + 11, 3, highlight_color);
                fillRect(x + strlen(map_names[map_sel]) * 12 + 5, y, 3, 20, highlight_color);
                highlight_color = (highlight_color == BLACK) ? WHITE : BLACK;
                SysTickReset();
            }
        }
    }
    return map_sel;
}

void parse_map_file(char *map_content, uint8_t *num_st_platforms, uint8_t *num_mov_platforms) {
    int i = 0;
    int idx = 0;
    int level = 0;

    // Extract number of static platforms
    char num_st_plats[4];
    while (map_content[idx] != '\n') {
        num_st_plats[idx] = map_content[idx];
        idx++;
    }
    num_st_plats[idx] = '\0';


    // Convert to integer
    num_st_platforms[level] = atoi(num_st_plats);

    while (map_content[idx] == '\n' || map_content[idx] == '\t' || map_content[idx] == ' ') {
        idx++;
    }


    // Create static platforms for level
    for (i = 0; i < num_st_platforms[level]; i++) {
        // Store arguments for static platform
        char params[4][4];

        // Extract the 4 arguments (x,y,length,thickness)
        int arg_idx = 0;
        int param_idx = 0;
        while (map_content[idx] != '\n') {
            if (map_content[idx] == ',') {
                // Go to next argument
                params[arg_idx][param_idx] = '\0';
                param_idx = 0;
                arg_idx++;
                idx++;
            }
            params[arg_idx][param_idx] = map_content[idx];
            idx++;
            param_idx++;
        }
        idx++;
        static_plats[level][i].x = (uint8_t)atoi(params[0]);
        static_plats[level][i].y = (uint8_t)atoi(params[1]);
        static_plats[level][i].length = (uint8_t)atoi(params[2]);
        static_plats[level][i].thickness = (uint8_t)atoi(params[3]);
    }


    while (map_content[idx] == '\n' || map_content[idx] == '\t' || map_content[idx] == ' ') {
        idx++;
    }


    // Extract number of moving platforms
    int plat_idx = 0;
    char num_mov_plats[4];
    while (map_content[idx] != '\n' && map_content[idx] != '\0') {
        num_mov_plats[plat_idx++] = map_content[idx];
        idx++;
    }
    num_mov_plats[idx] = '\0';


    // Convert to integer
    num_mov_platforms[level] = atoi(num_mov_plats);

    while (map_content[idx] == '\n' || map_content[idx] == '\t' || map_content[idx] == ' ') {
        idx++;
    }

    // Create static platforms for level
    for (i = 0; i < num_mov_platforms[level]; i++) {
        // Store arguments for static platform
        char params[6][4];

        // Extract the 4 arguments (x,y,length,thickness)
        int arg_idx = 0;
        int param_idx = 0;
        while (map_content[idx] != '\n') {
            if (map_content[idx] == ',') {
                // Go to next argument
                params[arg_idx][param_idx] = '\0';
                param_idx = 0;
                arg_idx++;
                idx++;
            }
            params[arg_idx][param_idx] = map_content[idx];
            idx++;
            param_idx++;
        }
        idx++;
        mov_plats[level][i].plat.x = (uint8_t)atoi(params[0]);
        mov_plats[level][i].plat.y = (uint8_t)atoi(params[1]);
        mov_plats[level][i].plat.length = (uint8_t)atoi(params[2]);
        mov_plats[level][i].plat.thickness = (uint8_t)atoi(params[3]);
        mov_plats[level][i].x_min = (uint8_t)atoi(params[4]);
        mov_plats[level][i].x_max = (uint8_t)atoi(params[5]);
    }
}

// Main Function
void main(void) {
    // Variables
    float gravity = 1, x_speed = 5, jump_speed = 8;
    uint8_t character_radius = 5, max_levels = 10;
    uint8_t on_ground = 1, level, num_levels = 0;
    unsigned int color;
    int tilt = 0;

    uint8_t num_st_platforms[max_levels], num_mov_platforms[max_levels];
    uint64_t map[256], prev_map[256];
    float x_pos, y_pos;

    unsigned long uiAdcInputPin = PIN_60, uiChannel = ADC_CH_3, ulSample;
    float x_voltage = 0;
    uint8_t mode = 1, num_modes = 2;
    char mode_names[2][10] = {"Online", "Offline"};

    float y_vel = 0, term_vel = 5;

    uint8_t connected = 0;
    char *content;
    char map_names[20][20];
    int num_maps;


    // Initialization
    BoardInit();
    PinMuxConfig();
    I2C_IF_Open(I2C_MASTER_MODE_FST);
    SysTickInit();
    InitTerm();
    ClearTerm();
    MAP_PRCMPeripheralClkEnable(PRCM_GSPI, PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralReset(PRCM_GSPI);
    MAP_SPIReset(GSPI_BASE);
    MAP_SPIConfigSetExpClk(GSPI_BASE, MAP_PRCMPeripheralClockGet(PRCM_GSPI), SPI_IF_BIT_RATE, SPI_MODE_MASTER, SPI_SUB_MODE_0, (SPI_SW_CTRL_CS | SPI_4PIN_MODE | SPI_TURBO_OFF | SPI_CS_ACTIVEHIGH | SPI_WL_8));
    MAP_SPIEnable(GSPI_BASE);
    Adafruit_Init();
    Report("Filling Screen\r\n");
    fillScreen(BLACK);
    MAP_PinTypeADC(uiAdcInputPin, PIN_MODE_255);
    MAP_ADCTimerConfig(ADC_BASE, 2^17);
    MAP_ADCTimerEnable(ADC_BASE);
    MAP_ADCEnable(ADC_BASE);
    MAP_ADCChannelEnable(ADC_BASE, uiChannel);

    int row, i;



startMenu:
    // Initialize Map
    for (i = 0; i < 256; i++) {
        map[i] = 0;
        prev_map[i] = 0;
    }

    color = WHITE;
    x_pos = 64, y_pos = 127 - character_radius;
    y_vel = 0;
    level = 0;
    // Start Menu to select online or offline mode
    start_menu(mode_names, num_modes, uiChannel, &mode);


    if (mode == 1) {
        // Offline
        num_levels = 4;
        goto map_create;
    } else {
        // Online
        num_levels = 2;
    }

    // Connect to WIFI
    if (WIFI_connect(&connected) == -1) {
        // If WIFI failed go back to start menu
        goto startMenu;
    }

    // Download list of map names
    content = NULL;
    download_file("/Map_names.txt", &content);

    Report("content = '%s'\r\n", content);

    parse_map_names(map_names, content, &num_maps);

    //Report("content = '%s'\r\n", content);


    level = map_menu(map_names, num_maps, uiChannel);

    if (level == 1) {
        color = CYAN;
    } else if (level == 2) {
        color = RED;
    } else if (level == 3) {
        color = MAGENTA;
    }

    // Download selected level
    char sel_map_name[20];
    sprintf(sel_map_name, "/%s_map.txt", map_names[level]);

    content = NULL;
    download_file(sel_map_name, &content);

    Report("content = '%s'\r\n", content);
    level = 0;

    parse_map_file(content, num_st_platforms, num_mov_platforms);


map_create:
    if (mode == 1) {
        num_st_platforms[0] = 5;
        static_plats[0][0] = create_static_platform(100, 108, 20, 3);
        static_plats[0][1] = create_static_platform(60, 88, 20, 3);
        static_plats[0][2] = create_static_platform(20, 68, 20, 3);
        static_plats[0][3] = create_static_platform(60, 48, 20, 3);
        static_plats[0][4] = create_static_platform(100, 28, 20, 3);

        num_mov_platforms[0] = 0;

        num_st_platforms[1] = 2;
        static_plats[1][0] = create_static_platform(80, 108, 20, 3);
        static_plats[1][1] = create_static_platform(50, 88, 20, 3);

        num_mov_platforms[1] = 3;
        mov_plats[1][0] = create_mov_platform(20, 68, 20, 3, 15, 100);
        mov_plats[1][1] = create_mov_platform(20, 48, 20, 3, 15, 60);
        mov_plats[1][2] = create_mov_platform(20, 28, 20, 3, 15, 20);

        num_st_platforms[2] = 3;
        static_plats[2][0] = create_static_platform(100, 108, 15, 3);
        static_plats[2][1] = create_static_platform(20, 48, 15, 3);
        static_plats[2][2] = create_static_platform(0, 3, 100, 3);

        num_mov_platforms[2] = 2;
        mov_plats[2][0] = create_mov_platform(20, 78, 20, 3, 15, 100);
        mov_plats[2][1] = create_mov_platform(10, 20, 20, 3, 10, 107);
    }

    // WIN Level
    static_plats[num_levels - 1][0] = create_static_platform(20, 95, 4, 17);
    static_plats[num_levels - 1][1] = create_static_platform(20, 112, 32, 4);
    static_plats[num_levels - 1][2] = create_static_platform(34, 95, 4, 17);
    static_plats[num_levels - 1][3] = create_static_platform(48, 95, 4, 17);
    static_plats[num_levels - 1][4] = create_static_platform(60, 95, 4, 4);
    static_plats[num_levels - 1][5] = create_static_platform(60, 101, 4, 15);
    static_plats[num_levels - 1][6] = create_static_platform(72, 95, 4, 21);
    static_plats[num_levels - 1][7] = create_static_platform(76, 95, 12, 4);
    static_plats[num_levels - 1][8] = create_static_platform(88, 95, 4, 21);
    static_plats[num_levels - 1][9] = create_static_platform(54, 64, 20, 3);
    num_st_platforms[num_levels - 1] = 10;
    mov_plats[num_levels - 1][0] = create_mov_platform(54, 32, 20, 3, 34, 74);
    num_mov_platforms[num_levels - 1] = 1;

    load_level(map, prev_map, static_plats[level], num_st_platforms[level], mov_plats[level], num_mov_platforms[level]);

    total_time = 0;
    while (1) {
        if (tick == 1) {
            tick = 0;
            unsigned char reg_tilt;
            GetTilt(0x18, 0x5, 1, &reg_tilt);
            tilt = reg_tilt;
            if (reg_tilt > 128) {
                tilt = reg_tilt - 256;
            }
            tilt = -tilt / 4;

            if (MAP_ADCFIFOLvlGet(ADC_BASE, uiChannel)) {
                ulSample = MAP_ADCFIFORead(ADC_BASE, uiChannel);
                x_voltage = (((float)((ulSample >> 2) & 0x0FFF)) * 1.4) / 4096;
                if (x_voltage < 0.55 && x_voltage > .45) {
                    x_voltage = 0.5;
                }

                x_pos -= ((x_voltage - 0.5) * x_speed);
                if (x_pos < character_radius) {
                    x_pos = character_radius;
                } else if (x_pos > 127 - character_radius) {
                    x_pos = 127 - character_radius;
                }
                if (on_ground == 1 && GPIOPinRead(GPIOA0_BASE, 0x80)) {
                    y_vel = jump_speed;
                    on_ground = 0;
                }
                y_pos -= y_vel;
                y_vel -= gravity;
                if (y_vel < -term_vel) {
                    y_vel = -term_vel;
                }
                if (y_pos > 127 - character_radius) {
                    y_pos = 127 - character_radius;
                    y_vel = 0;
                    on_ground = 1;
                }
                // Check if user beat level
                else if (y_pos < character_radius) {
                    level++;
                    if (level == 1) {
                        color = CYAN;
                    } else if (level == 2) {
                        color = RED;
                    }
                    if (level == num_levels - 1) {
                        setCursor(30, 40);
                        setTextSize(1);
                        char dis_time[32];
                        sprintf(dis_time, "Time: %.2f", total_time / 50.0);
                        Outstr(dis_time);
                        color = MAGENTA;
                    } else if (level >= num_levels) {
                        fillScreen(BLACK);
                        goto startMenu;
                    }
                    load_level(map, prev_map, static_plats[level], num_st_platforms[level], mov_plats[level], num_mov_platforms[level]);
                    y_pos = 127 - character_radius;
                }

                uint64_t cur_rows[2] = {map[(int)(y_pos) * 2], map[(int)(y_pos) * 2 + 1]};
                int col;
                for (col = (int)(x_pos); col <= (int)(x_pos) + character_radius && col < 128; col++) {
                    if ((cur_rows[col / 64] << (col % 64)) & 0x8000000000000000) {
                        x_pos = col - character_radius;
                        break;
                    }
                }

                for (col = (int)(x_pos); col > (int)(x_pos) - character_radius && col > 0; col--) {
                    if ((cur_rows[col / 64] << (col % 64)) & 0x8000000000000000) {
                        x_pos = col + character_radius;
                        break;
                    }
                }

                if (y_vel <= 0) {
                    for (row = (int)(y_pos); row <= (int)(y_pos) + character_radius && row < 128; row++) {
                        for (i = (int)x_pos - character_radius + 1; i < x_pos + character_radius; i++) {
                            uint64_t cur_row = map[row * 2 + (i / 64)];
                            if ((cur_row << (i % 64)) & 0x8000000000000000) {
                                y_pos = row - character_radius - 1;
                                y_vel = 0;
                                on_ground = 1;
                                break;
                            }
                        }
                    }
                } else {
                    for (row = (int)(y_pos); row >= (int)(y_pos) - character_radius && row > 0; row--) {
                        for (i = (int)x_pos - character_radius; i < x_pos + character_radius; i++) {
                            uint64_t cur_row = map[row * 2 + (i / 64)];
                            if ((cur_row << (i % 64)) & 0x8000000000000000) {
                                y_pos = row + character_radius + 1;
                                y_vel = 0;
                                break;
                            }
                        }
                    }
                }

                update_platforms(mov_plats[level], num_mov_platforms[level], tilt, map);
                map_fillCircle((int)(x_pos), (int)(y_pos), character_radius, 0, map);
                map_draw(map, prev_map, color);

                for (i = 0; i < 256; i++) {
                    prev_map[i] = map[i];
                }
                map_fillCircle((int)(x_pos), (int)(y_pos), character_radius, 1, map);
            }
        }
    }
}

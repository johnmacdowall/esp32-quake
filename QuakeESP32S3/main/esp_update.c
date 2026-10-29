/**
 * @file esp_update.c
 * @brief Wi-Fi update mode (esp32-quake).
 *
 * - Setup: with no Wi-Fi details stored (or if the home network cannot be joined),
 *   the board opens the access point QUAKE-SETUP (password quakesetup) and serves a
 *   form at http://192.168.4.1/ for the home Wi-Fi details and an update password.
 *   Nothing secret is built into the firmware: it is all stored in NVS.
 * - Updating: on the home network the board is named "quake" (DHCP host name, so a
 *   router registering DHCP leases in DNS makes http://quake.lan/ work) and shows
 *   its address on screen. The page uploads firmware with a progress bar; curl works
 *   too (HTTP Basic auth, any user name, the update password):
 *     curl -u quake:PASSWORD -H "Expect:" --data-binary @firmware.bin http://quake.lan/update
 * - Safety net (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): new firmware boots straight
 *   into update mode and is only marked valid once its Wi-Fi is up. If it crashes or
 *   is reset before that, the bootloader goes back to the previous firmware.
 * - Wi-Fi only runs in update mode: during play the game keeps all its RAM.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_update.h"
#include "esp_board.h"
// text-mode screen functions (esp_graphics.c), declared here to keep the engine headers out
void displayPrintf(int x, int y, const char *format, ...);
void displayPrintln(bool update, const char *format, ...);
void setDisplayPen(int color, int background);
void displayFillRect(int x, int y, int w, int h, int color);
void displayRefreshNow(void);

#if CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM > 8
#error "Wi-Fi buffer settings are stale (Wi-Fi would run out of internal RAM): delete sdkconfig.esp32-8048S050 next to platformio.ini, run Full Clean, and build again."
#endif

#define AP_SSID                 "QUAKE-SETUP"
#define AP_PASSWORD             "quakesetup"
#define AP_CHANNEL              6
#define HOSTNAME                "quake"
#define NVS_NAMESPACE           "update"
#define PIN_BOOT                0
#define BOOT_WINDOW_MS          1500
#define STA_CONNECT_TIMEOUT_MS  20000
#define AUTO_PLAY_MS            15000   // after confirming new firmware, start the game if nothing happens
#define MIN_PASSWORD_LEN        4
#define REQUEST_MAGIC           0x51554B45u
#define BIT_GOT_IP              BIT0
#define UPLOAD_CHUNK            4096
#define MAX_SCAN                20
#define SCREEN_W                320
#define MAX_LINES               9
#define COLOR_GREY              0
#define COLOR_BLACK             1
#define COLOR_WHITE             2
#define COLOR_BLUE              3

static const char *TAG = "update";
RTC_NOINIT_ATTR static uint32_t rtcRequest;     // survives esp_restart(): enter update mode next boot

typedef enum { MODE_STARTING, MODE_SETUP, MODE_STA } updateMode_t;
static volatile updateMode_t mode = MODE_STARTING;
static char ssid[33], wifiPassword[65], updatePassword[65];
static bool haveWifi, haveUpdatePassword;
static char ipText[16];
static esp_netif_t *staNetif, *apNetif;
static EventGroupHandle_t wifiEvents;
static volatile bool staActive;                 // reconnect on disconnection
static bool confirmPending;                     // new firmware, waiting for Wi-Fi
static volatile bool confirmedNow;
static volatile bool uploading;
static volatile int64_t lastActivityUs;
static char scanOptions[MAX_SCAN * 80];         // <option> list for the setup page
static SemaphoreHandle_t screenLock;
static char title[41];
static char lines[MAX_LINES][41];
static volatile int progress = -1;              // 0..100, -1 = hidden

// ---------------------------------------------------------------------------
// Screen (320x200 text area, boot palette)
// ---------------------------------------------------------------------------
static void drawScreen(void)
{
    xSemaphoreTake(screenLock, portMAX_DELAY);
    displayFillRect(0, 0, SCREEN_W, 200, COLOR_GREY);
    displayFillRect(0, 0, SCREEN_W, 20, COLOR_BLUE);
    setDisplayPen(COLOR_WHITE, COLOR_BLUE);
    displayPrintf(8, 6, "%s", title);
    setDisplayPen(COLOR_BLACK, COLOR_GREY);
    for (int i = 0; i < MAX_LINES; i++)
    {
        displayPrintf(8, 30 + i * 12, "%s", lines[i]);
    }
    if (progress >= 0)
    {
        int w = 280 * progress / 100;
        displayFillRect(20, 146, 280, 16, COLOR_BLACK);
        displayFillRect(22, 148, 276, 12, COLOR_WHITE);
        displayFillRect(22, 148, w > 276 ? 276 : w, 12, COLOR_BLUE);
        displayPrintf(144, 168, "%3d%%", progress);
    }
    displayPrintf(8, 186, "%s", mode == MODE_SETUP || mode == MODE_STA ? "BOOT or any button: play" : "");
    displayRefreshNow();
    xSemaphoreGive(screenLock);
}

static void setTitle(const char *t)
{
    snprintf(title, sizeof(title), "%s", t);
}

static void setLine(int i, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vsnprintf(lines[i], sizeof(lines[i]), fmt, va);
    va_end(va);
}

static void clearLines(void)
{
    memset(lines, 0, sizeof(lines));
}

// ---------------------------------------------------------------------------
// Stored settings
// ---------------------------------------------------------------------------
static void loadSettings(void)
{
    nvs_handle_t h;
    haveWifi = haveUpdatePassword = false;
    ssid[0] = wifiPassword[0] = updatePassword[0] = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }
    size_t len = sizeof(ssid);
    haveWifi = nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0];
    len = sizeof(wifiPassword);
    if (nvs_get_str(h, "wifipass", wifiPassword, &len) != ESP_OK)
    {
        wifiPassword[0] = 0;
    }
    len = sizeof(updatePassword);
    haveUpdatePassword = nvs_get_str(h, "updpass", updatePassword, &len) == ESP_OK && updatePassword[0];
    nvs_close(h);
}

static esp_err_t saveSettings(const char *newSsid, const char *newWifiPassword, const char *newUpdatePassword)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    if (newSsid)
    {
        err = nvs_set_str(h, "ssid", newSsid);
        if (err == ESP_OK)
        {
            err = nvs_set_str(h, "wifipass", newWifiPassword);
        }
    }
    else
    {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "wifipass");
    }
    if (err == ESP_OK && newUpdatePassword)
    {
        err = nvs_set_str(h, "updpass", newUpdatePassword);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// Restarts
// ---------------------------------------------------------------------------
static void restartNow(bool intoUpdateMode)
{
    rtcRequest = intoUpdateMode ? REQUEST_MAGIC : 0;
    // BOOT held during a reset selects the ROM download mode: wait for its release
    for (int i = 0; i < 500 && !gpio_get_level(PIN_BOOT); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void restartTask(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));     // let the HTTP response go out
    restartNow((bool) (intptr_t) arg);
}

static void restartSoon(bool intoUpdateMode)
{
    xTaskCreate(restartTask, "restart", 3072, (void*) (intptr_t) intoUpdateMode, 5, NULL);
}

// ---------------------------------------------------------------------------
// Buttons: only a new press counts (released, then pressed). A button held
// from the start or stuck by a wiring fault never triggers anything.
// Bits 0..15: pad buttons, bit 16: BOOT.
// ---------------------------------------------------------------------------
#define BOOT_BIT    (1u << 16)

typedef struct
{
    uint32_t seenUp;        // buttons seen released since watching started
    uint32_t down;          // last state
    uint32_t valid;         // buttons that can be read now
} pressWatch_t;

static void watchStart(pressWatch_t *w)
{
    memset(w, 0, sizeof(*w));
}

static bool watchNewPress(pressWatch_t *w)
{
    int32_t pad = boardPadButtons();
    uint32_t down = 0, valid = BOOT_BIT;
    if (pad >= 0)
    {
        down = (uint32_t) pad & 0xFFFF;
        valid |= 0xFFFF;
    }
    else
    {
        w->seenUp &= ~0xFFFFu;      // pad gone: forget its buttons
    }
    if (!gpio_get_level(PIN_BOOT))
    {
        down |= BOOT_BIT;
    }
    bool newPress = (down & w->seenUp & valid) != 0;
    w->seenUp |= ~down & valid;
    w->down = down;
    w->valid = valid;
    return newPress;
}

// Pad buttons down all along (held at power-on, or a wiring fault)
static uint32_t watchStuck(const pressWatch_t *w)
{
    return w->down & ~w->seenUp & w->valid & 0xFFFF;
}

static void reportStuck(uint32_t stuck)
{
    for (int i = 0; i < 16; i++)
    {
        if (stuck & (1u << i))
        {
            ESP_LOGW(TAG, "pad button %s reads as held: ignored (wiring? button legs?)", boardPadButtonName(i));
            displayPrintln(1, "Button %s held/stuck: ignored", boardPadButtonName(i));
        }
    }
}

// ---------------------------------------------------------------------------
// Boot-time decision
// ---------------------------------------------------------------------------
bool updateModeRequested(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        confirmPending = true;
        return true;
    }
    if (rtcRequest == REQUEST_MAGIC)
    {
        rtcRequest = 0;
        return true;
    }
    rtcRequest = 0;
    gpio_config_t io = { 0 };
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pin_bit_mask = 1ULL << PIN_BOOT;
    gpio_config(&io);
    boardInputInit();       // button pad, if any (also called later by the game)
    displayPrintln(1, "Press BOOT or a pad button for Wi-Fi update");
    pressWatch_t w;
    watchStart(&w);
    int64_t end = esp_timer_get_time() + BOOT_WINDOW_MS * 1000LL;
    while (esp_timer_get_time() < end)
    {
        if (watchNewPress(&w))
        {
            return true;    // update mode starts its own watch: this press does not leak into it
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    reportStuck(watchStuck(&w));
    return false;
}

// ---------------------------------------------------------------------------
// Failures: never crash. Show what went wrong, then leave with a button press.
// New firmware waiting for confirmation is not confirmed: the restart rolls back.
// ---------------------------------------------------------------------------
static void waitForPressThenPlay(void);

static void failScreen(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed: %s (internal RAM free %u, largest block %u)", what, esp_err_to_name(err),
        (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    setTitle("WI-FI COULD NOT START");
    clearLines();
    setLine(0, "%.38s", what);
    setLine(1, "Error: %.31s", esp_err_to_name(err));
    setLine(3, "Free internal RAM %u kB (largest %u)",
        (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    if (confirmPending)
    {
        setLine(5, "New firmware not confirmed:");
        setLine(6, "the previous one returns on restart.");
    }
    progress = -1;
    mode = MODE_SETUP;      // shows the "button: play" hint
    drawScreen();
    waitForPressThenPlay();
}

#define WIFI_TRY(expr) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) failScreen(#expr, e_); } while (0)

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static void onWifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && staActive)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && staActive)
    {
        if (mode == MODE_STA)
        {
            setLine(1, "Wi-Fi lost, reconnecting...");
            drawScreen();
        }
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t*) data;
        snprintf(ipText, sizeof(ipText), IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(wifiEvents, BIT_GOT_IP);
        if (mode == MODE_STA)
        {
            setLine(1, "Connected to %s", ssid);
            setLine(3, "  http://%s/", ipText);
            drawScreen();
        }
    }
}

static bool joinHomeWifi(void)
{
    wifi_config_t cfg = { 0 };
    snprintf((char*) cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char*) cfg.sta.password, sizeof(cfg.sta.password), "%s", wifiPassword);
    cfg.sta.threshold.authmode = wifiPassword[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    staActive = true;
    WIFI_TRY(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(wifiEvents, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    if (bits & BIT_GOT_IP)
    {
        return true;
    }
    staActive = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    return false;
}

static void htmlEscape(char *out, size_t outSize, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < outSize; in++)
    {
        const char *rep = NULL;
        switch (*in)
        {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
        }
        if (rep)
        {
            size_t l = strlen(rep);
            memcpy(out + o, rep, l);
            o += l;
        }
        else
        {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

static void scanNetworks(void)
{
    scanOptions[0] = 0;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK)
    {
        return;
    }
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));
    if (!recs)
    {
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK)
    {
        size_t used = 0;
        for (int i = 0; i < n; i++)
        {
            const char *name = (const char*) recs[i].ssid;
            bool dup = !name[0];
            for (int j = 0; j < i && !dup; j++)
            {
                dup = !strcmp(name, (const char*) recs[j].ssid);
            }
            if (dup)
            {
                continue;
            }
            char esc[100];
            htmlEscape(esc, sizeof(esc), name);
            int w = snprintf(scanOptions + used, sizeof(scanOptions) - used, "<option value=\"%s\">", esc);
            if (w < 0 || used + w >= sizeof(scanOptions))
            {
                break;
            }
            used += w;
        }
    }
    free(recs);
}

static void startSetupAccessPoint(const char *reason)
{
    wifi_config_t ap = { 0 };
    snprintf((char*) ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    snprintf((char*) ap.ap.password, sizeof(ap.ap.password), "%s", AP_PASSWORD);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_APSTA));     // STA side: network scan for the form
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_AP, &ap));
    WIFI_TRY(esp_wifi_start());
    scanNetworks();
    mode = MODE_SETUP;
    setTitle("WI-FI SETUP");
    clearLines();
    if (reason)
    {
        setLine(0, "%s", reason);
    }
    setLine(1, "On your phone, join the Wi-Fi network");
    setLine(2, "  " AP_SSID);
    setLine(3, "  password: " AP_PASSWORD);
    setLine(5, "then open in a browser:");
    setLine(6, "  http://192.168.4.1/");
    drawScreen();
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64Decode(const char *in, char *out, int outSize)
{
    int o = 0, bits = 0;
    uint32_t acc = 0;
    for (; *in && *in != '='; in++)
    {
        int v = base64Value(*in);
        if (v < 0)
            return -1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (o + 1 >= outSize)
                return -1;
            out[o++] = (char) ((acc >> bits) & 0xFF);
        }
    }
    out[o] = 0;
    return o;
}

static bool constantTimeEquals(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = la != lb;
    for (size_t i = 0; i < la && i < lb; i++)
    {
        diff |= (unsigned char) (a[i] ^ b[i]);
    }
    return !diff;
}

// HTTP Basic auth: any user name, the update password
static bool authorized(httpd_req_t *req)
{
    if (!haveUpdatePassword)
    {
        return false;
    }
    char header[200], decoded[150];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK
        || strncmp(header, "Basic ", 6) || base64Decode(header + 6, decoded, sizeof(decoded)) < 0)
    {
        return false;
    }
    char *colon = strchr(decoded, ':');
    return colon && constantTimeEquals(colon + 1, updatePassword);
}

static esp_err_t sendUnauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"quake\"");
    return httpd_resp_sendstr(req, "Wrong update password\n");
}

static esp_err_t sendError(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, msg);
}

// Reads a small application/x-www-form-urlencoded body
static int readBody(httpd_req_t *req, char *buf, int size)
{
    int len = req->content_len;
    if (len <= 0 || len >= size)
    {
        return -1;
    }
    int got = 0;
    while (got < len)
    {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0)
            return -1;
        got += r;
    }
    buf[got] = 0;
    return got;
}

static void urlDecode(char *s)
{
    char *o = s;
    for (; *s; s++)
    {
        if (*s == '+')
        {
            *o++ = ' ';
        }
        else if (*s == '%' && isxdigit((unsigned char) s[1]) && isxdigit((unsigned char) s[2]))
        {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char) strtol(hex, NULL, 16);
            s += 2;
        }
        else
        {
            *o++ = *s;
        }
    }
    *o = 0;
}

// Extracts and decodes one field of a urlencoded body
static bool formField(const char *body, const char *name, char *out, size_t outSize)
{
    size_t nl = strlen(name);
    const char *p = body;
    while (p && *p)
    {
        if (!strncmp(p, name, nl) && p[nl] == '=')
        {
            const char *v = p + nl + 1;
            const char *end = strchr(v, '&');
            size_t l = end ? (size_t) (end - v) : strlen(v);
            if (l >= outSize * 3)
                return false;
            char tmp[256];
            if (l >= sizeof(tmp))
                return false;
            memcpy(tmp, v, l);
            tmp[l] = 0;
            urlDecode(tmp);
            if (strlen(tmp) >= outSize)
                return false;
            strcpy(out, tmp);
            return true;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    out[0] = 0;
    return false;
}

// httpd_resp_sendstr_chunk() with an empty string ends the response: skip those
static void sendChunk(httpd_req_t *req, const char *s)
{
    if (s && s[0])
    {
        httpd_resp_sendstr_chunk(req, s);
    }
}

static void touchActivity(void)
{
    lastActivityUs = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Quake handheld</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:34em;margin:1em auto;padding:0 1em;background:#111;color:#ddd}"
    "h1{font-size:1.3em;color:#e8a33c}label{display:block;margin:.8em 0 .2em}"
    "input,button{font-size:1em;padding:.5em;width:100%;box-sizing:border-box;border-radius:6px;border:1px solid #555;background:#222;color:#eee}"
    "button{background:#e8a33c;color:#111;border:0;margin-top:1em;font-weight:600}button.alt{background:#444;color:#eee}"
    ".bar{height:1.4em;background:#333;border-radius:6px;overflow:hidden;margin-top:1em}.bar div{height:100%;width:0;background:#e8a33c}"
    "code{background:#222;padding:.1em .3em;border-radius:4px;word-break:break-all}.muted{color:#999;font-size:.9em}"
    "</style></head><body>";

static esp_err_t sendSetupPage(httpd_req_t *req, const char *message)
{
    char esc[100];
    httpd_resp_set_type(req, "text/html");
    sendChunk(req, PAGE_HEAD);
    sendChunk(req, "<h1>Quake handheld: Wi-Fi setup</h1>");
    if (message)
    {
        sendChunk(req, "<p><b>");
        htmlEscape(esc, sizeof(esc), message);
        sendChunk(req, esc);
        sendChunk(req, "</b></p>");
    }
    sendChunk(req, "<form method=post action=/save><label>Home Wi-Fi name</label><input name=ssid list=nets required maxlength=32 value=\"");
    htmlEscape(esc, sizeof(esc), ssid);
    sendChunk(req, esc);
    sendChunk(req, "\"><datalist id=nets>");
    sendChunk(req, scanOptions);
    sendChunk(req, "</datalist><label>Home Wi-Fi password</label><input name=wifipass type=password maxlength=63>");
    if (haveUpdatePassword)
    {
        sendChunk(req, "<label>Current update password</label><input name=oldpass type=password required maxlength=64>");
    }
    sendChunk(req,
        "<label>New update password (needed to upload firmware)</label><input name=updpass type=password minlength=4 maxlength=64"
        );
    sendChunk(req, haveUpdatePassword ? " placeholder=\"leave empty to keep it\">" : " required>");
    sendChunk(req,
        "<label>Repeat the update password</label><input name=updpass2 type=password maxlength=64>"
        "<button>Save and connect</button></form>"
        "<p class=muted>The details are stored on the handheld only. It then restarts, joins your network "
        "and shows its address on the screen.</p></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t sendMainPage(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char buf[400], esc[100];
    httpd_resp_set_type(req, "text/html");
    sendChunk(req, PAGE_HEAD);
    sendChunk(req, "<h1>Quake handheld: firmware update</h1>");
    htmlEscape(esc, sizeof(esc), app->version);
    snprintf(buf, sizeof(buf), "<p>Running <b>%s</b> (built %s %s), slot %s.<br>Address %s, name <b>" HOSTNAME "</b>.</p>",
        esc, app->date, app->time, running ? running->label : "?", ipText);
    sendChunk(req, buf);
    if (confirmedNow)
    {
        sendChunk(req, "<p><b>New firmware confirmed.</b> The game starts by itself after a short pause.</p>");
    }
    sendChunk(req,
        "<label>Firmware file (.pio/build/esp32-8048S050/firmware.bin)</label><input type=file id=f accept=.bin>"
        "<label>Update password</label><input type=password id=pw>"
        "<button onclick=up()>Upload</button><div class=bar><div id=b></div></div><p id=m></p>"
        "<button class=alt onclick=\"post('/play','Starting the game...')\">Play (leave update mode)</button>"
        "<button class=alt onclick=\"if(confirm('Forget the home Wi-Fi? The handheld goes back to QUAKE-SETUP.'))post('/forget','Wi-Fi forgotten: join QUAKE-SETUP to set it up again.')\">Change Wi-Fi network</button>"
        "<p class=muted>From a terminal:<br><code>curl -u quake:PASSWORD -H \"Expect:\" --data-binary @firmware.bin http://");
    sendChunk(req, ipText);
    sendChunk(req, "/update</code></p><script>"
        "const m=document.getElementById('m'),b=document.getElementById('b');"
        "function auth(x){x.setRequestHeader('Authorization','Basic '+btoa(unescape(encodeURIComponent('quake:'+document.getElementById('pw').value))));}"
        "function post(u,ok){const x=new XMLHttpRequest();x.open('POST',u);auth(x);x.onload=()=>{m.textContent=x.status==200?ok:x.responseText};x.send();}"
        "function back(){fetch('/status').then(r=>r.json()).then(s=>{m.textContent='Back online: running '+s.version+(s.confirmed?' (confirmed)':'')+'.';})"
        ".catch(()=>setTimeout(back,2000));}"
        "function up(){const f=document.getElementById('f').files[0];if(!f){m.textContent='Choose a firmware file first.';return;}"
        "const x=new XMLHttpRequest();x.open('POST','/update');auth(x);"
        "x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(100*e.loaded/e.total);b.style.width=p+'%';m.textContent='Uploading '+p+'%';}};"
        "x.onload=()=>{if(x.status==200){b.style.width='100%';m.textContent='Done. Restarting into the new firmware...';setTimeout(back,8000);}"
        "else{b.style.width='0';m.textContent=x.responseText;}};"
        "x.onerror=()=>{m.textContent='Connection lost during the upload.';};x.send(f);}"
        "</script></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static esp_err_t handleRoot(httpd_req_t *req)
{
    touchActivity();
    return mode == MODE_SETUP ? sendSetupPage(req, NULL) : sendMainPage(req);
}

static esp_err_t handleStatus(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char buf[320], esc[80];
    htmlEscape(esc, sizeof(esc), app->version);
    snprintf(buf, sizeof(buf), "{\"version\":\"%s\",\"date\":\"%s %s\",\"mode\":\"%s\",\"confirmed\":%s,\"uploading\":%s,\"progress\":%d}",
        esc, app->date, app->time, mode == MODE_SETUP ? "setup" : "update", confirmedNow ? "true" : "false",
        uploading ? "true" : "false", progress);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handleSave(httpd_req_t *req)
{
    touchActivity();
    if (mode != MODE_SETUP)
    {
        return sendError(req, "403 Forbidden", "Settings can only be changed in setup mode\n");
    }
    char body[600], newSsid[33], newWifi[65], newUpd[65], newUpd2[65], oldPass[65];
    if (readBody(req, body, sizeof(body)) < 0)
    {
        return sendSetupPage(req, "The form could not be read, please try again.");
    }
    formField(body, "ssid", newSsid, sizeof(newSsid));
    formField(body, "wifipass", newWifi, sizeof(newWifi));
    formField(body, "updpass", newUpd, sizeof(newUpd));
    formField(body, "updpass2", newUpd2, sizeof(newUpd2));
    formField(body, "oldpass", oldPass, sizeof(oldPass));
    if (!newSsid[0])
    {
        return sendSetupPage(req, "Enter the name of your home Wi-Fi.");
    }
    if (haveUpdatePassword && !constantTimeEquals(oldPass, updatePassword))
    {
        return sendSetupPage(req, "The current update password is wrong.");
    }
    if (newUpd[0] || !haveUpdatePassword)
    {
        if (strlen(newUpd) < MIN_PASSWORD_LEN)
        {
            return sendSetupPage(req, "The update password needs at least 4 characters.");
        }
        if (strcmp(newUpd, newUpd2))
        {
            return sendSetupPage(req, "The two update passwords do not match.");
        }
    }
    if (saveSettings(newSsid, newWifi, newUpd[0] ? newUpd : NULL) != ESP_OK)
    {
        return sendSetupPage(req, "Saving failed (NVS error).");
    }
    char esc[100];
    htmlEscape(esc, sizeof(esc), newSsid);
    httpd_resp_set_type(req, "text/html");
    sendChunk(req, PAGE_HEAD);
    sendChunk(req, "<h1>Saved</h1><p>The handheld restarts and joins <b>");
    sendChunk(req, esc);
    sendChunk(req, "</b>. Its address appears on its screen; if your router registers DHCP names, <b>http://" HOSTNAME
        ".lan/</b> works too.</p><p>You can leave " AP_SSID " now.</p></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    setLine(8, "Saved: restarting...");
    drawScreen();
    restartSoon(true);
    return ESP_OK;
}

static esp_err_t handleUpdate(httpd_req_t *req)
{
    touchActivity();
    if (mode != MODE_STA)
    {
        return sendError(req, "403 Forbidden", "Finish the Wi-Fi setup first\n");
    }
    if (!authorized(req))
    {
        return sendUnauthorized(req);
    }
    if (uploading)
    {
        return sendError(req, "409 Conflict", "An update is already running\n");
    }
    int total = req->content_len;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target)
    {
        return sendError(req, "500 Internal Server Error", "No update slot (partition table without OTA slots)\n");
    }
    if (total <= 0)
    {
        return sendError(req, "411 Length Required", "Content-Length required\n");
    }
    if ((size_t) total > target->size)
    {
        return sendError(req, "413 Payload Too Large", "Firmware larger than the update slot\n");
    }
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf)
    {
        return sendError(req, "500 Internal Server Error", "Out of memory\n");
    }
    uploading = true;
    progress = 0;
    setLine(5, "Receiving new firmware...");
    drawScreen();
    esp_ota_handle_t ota = 0;
    bool begun = false;
    const char *fail = NULL;
    int received = 0, shownPct = 0, timeouts = 0;
    while (received < total && !fail)
    {
        int want = total - received < UPLOAD_CHUNK ? total - received : UPLOAD_CHUNK;
        // the first chunk must hold the image header and the app description
        int filled = 0;
        int needed = received == 0 ? (want < 512 ? want : 512) : 1;
        while (filled < needed)
        {
            int r = httpd_req_recv(req, buf + filled, want - filled);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 10)
                continue;
            if (r <= 0)
            {
                fail = "Connection lost during the upload\n";
                break;
            }
            timeouts = 0;
            filled += r;
        }
        if (fail)
            break;
        if (received == 0)
        {
            const esp_app_desc_t *desc = (const esp_app_desc_t*) (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            if (filled < 512 || (uint8_t) buf[0] != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD)
            {
                fail = "Not an ESP32 firmware image (use firmware.bin, not the .elf or a merged image)\n";
                break;
            }
            if (strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)))
            {
                fail = "This is not Quake firmware (another project's firmware.bin?)\n";
                break;
            }
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK)
            {
                fail = "Could not start writing the update slot\n";
                break;
            }
            begun = true;
        }
        if (esp_ota_write(ota, buf, filled) != ESP_OK)
        {
            fail = "Flash write failed\n";
            break;
        }
        received += filled;
        touchActivity();
        int pct = (int) ((int64_t) received * 100 / total);
        if (pct >= shownPct + 2 || pct == 100)
        {
            shownPct = pct;
            progress = pct;
            drawScreen();
        }
    }
    free(buf);
    if (!fail && begun)
    {
        esp_err_t err = esp_ota_end(ota);
        begun = false;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            fail = "The firmware image is damaged (incomplete upload?)\n";
        }
        else if (err != ESP_OK)
        {
            fail = "Finishing the update failed\n";
        }
        else if (esp_ota_set_boot_partition(target) != ESP_OK)
        {
            fail = "Could not select the new firmware\n";
        }
    }
    if (begun)
    {
        esp_ota_abort(ota);
    }
    if (fail)
    {
        ESP_LOGW(TAG, "update failed: %s", fail);
        progress = -1;
        setLine(5, "Update failed: still on the old firmware");
        drawScreen();
        uploading = false;
        return sendError(req, "400 Bad Request", fail);
    }
    ESP_LOGI(TAG, "update written to %s (%d bytes), restarting", target->label, total);
    setLine(5, "Update complete: restarting...");
    drawScreen();
    httpd_resp_sendstr(req, "OK: restarting into the new firmware. If it fails to come up on Wi-Fi, the previous one returns by itself.\n");
    restartSoon(false);
    return ESP_OK;
}

static esp_err_t handlePlay(httpd_req_t *req)
{
    if (!authorized(req))
    {
        return sendUnauthorized(req);
    }
    httpd_resp_sendstr(req, "OK\n");
    restartSoon(false);
    return ESP_OK;
}

static esp_err_t handleForget(httpd_req_t *req)
{
    if (!authorized(req))
    {
        return sendUnauthorized(req);
    }
    if (saveSettings(NULL, NULL, NULL) != ESP_OK)
    {
        return sendError(req, "500 Internal Server Error", "Could not erase the Wi-Fi details\n");
    }
    httpd_resp_sendstr(req, "OK\n");
    restartSoon(true);
    return ESP_OK;
}

static void startWebServer(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        setLine(8, "Web server failed to start");
        drawScreen();
        return;
    }
    static const httpd_uri_t uris[] =
    {
        { .uri = "/", .method = HTTP_GET, .handler = handleRoot },
        { .uri = "/status", .method = HTTP_GET, .handler = handleStatus },
        { .uri = "/save", .method = HTTP_POST, .handler = handleSave },
        { .uri = "/update", .method = HTTP_POST, .handler = handleUpdate },
        { .uri = "/play", .method = HTTP_POST, .handler = handlePlay },
        { .uri = "/forget", .method = HTTP_POST, .handler = handleForget },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

// ---------------------------------------------------------------------------
// Update mode
// ---------------------------------------------------------------------------
void updateModeRun(void)
{
    gpio_config_t io = { 0 };
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pin_bit_mask = 1ULL << PIN_BOOT;
    gpio_config(&io);
    boardInputInit();       // button pad, if any (safe to call twice)
    screenLock = xSemaphoreCreateMutex();
    wifiEvents = xEventGroupCreate();
    setTitle(confirmPending ? "CHECKING NEW FIRMWARE" : "WI-FI UPDATE MODE");
    clearLines();
    setLine(1, "Starting Wi-Fi...");
    drawScreen();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    WIFI_TRY(err);
    loadSettings();

    WIFI_TRY(esp_netif_init());
    WIFI_TRY(esp_event_loop_create_default());
    staNetif = esp_netif_create_default_wifi_sta();
    apNetif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(staNetif, HOSTNAME);         // DHCP option 12: quake.lan with pfSense lease registration
    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    WIFI_TRY(esp_wifi_init(&initCfg));
    WIFI_TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // credentials live in our own NVS namespace
    WIFI_TRY(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, NULL));
    WIFI_TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, NULL));

    if (haveWifi)
    {
        setLine(1, "Joining %s...", ssid);
        drawScreen();
        if (joinHomeWifi())
        {
            mode = MODE_STA;
            if (!confirmPending)
            {
                setTitle("WI-FI UPDATE MODE");
            }
            clearLines();
            setLine(1, "Connected to %s", ssid);
            setLine(2, "Open in a browser:");
            setLine(3, "  http://%s/", ipText);
            setLine(4, "  http://" HOSTNAME ".lan/");
            setLine(6, "Firmware %.28s", esp_app_get_description()->version);
        }
        else
        {
            char reason[41];
            snprintf(reason, sizeof(reason), "Could not join %.24s", ssid);
            startSetupAccessPoint(reason);
        }
    }
    else
    {
        startSetupAccessPoint(NULL);
    }
    startWebServer();

    // Wi-Fi is up: new firmware passed its test, cancel the rollback
    if (confirmPending)
    {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        {
            confirmedNow = true;
            ESP_LOGI(TAG, "new firmware confirmed");
            if (mode == MODE_STA)
            {
                setTitle("NEW FIRMWARE CONFIRMED");
                setLine(7, "Starting the game shortly...");
            }
        }
    }
    touchActivity();
    drawScreen();

    waitForPressThenPlay();
}

// Leave with a new press of BOOT or a pad button; after a confirmed update, start the game by itself
static void waitForPressThenPlay(void)
{
    pressWatch_t w;
    watchStart(&w);
    while (1)
    {
        if (watchNewPress(&w) && !uploading)
        {
            setLine(8, "Starting the game...");
            drawScreen();
            restartNow(false);
        }
        if (confirmedNow && mode == MODE_STA && !uploading && esp_timer_get_time() - lastActivityUs > AUTO_PLAY_MS * 1000LL)
        {
            restartNow(false);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

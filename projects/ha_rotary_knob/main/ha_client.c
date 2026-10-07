/*
 * ha_client.c - Wi-Fi + Home Assistant REST API.
 *
 * Home Assistant endpoints used (all need "Authorization: Bearer <token>"):
 *   GET  /api/states/<entity_id>          -> current state + attributes of one entity
 *   POST /api/services/<domain>/<service> -> run an action, e.g. light/toggle, body {"entity_id": ...}
 *
 * One background task owns the HTTP connection. It waits for commands from the UI and, when
 * there are none, refreshes every entity every CONFIG_KNOB_HA_POLL_MS so changes made elsewhere
 * (the HA app, a wall switch, an automation) show up on the knob.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "ha_client.h"

static const char *TAG = "ha";

#define WIFI_CONNECTED_BIT  BIT0
#define HTTP_BUF_SIZE       (12 * 1024)
#define HTTP_TIMEOUT_MS     5000

typedef enum {
    CMD_TOGGLE,
    CMD_ALT,
    CMD_LEVEL,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int idx;
    float value;
} ha_cmd_t;

static ha_entity_t s_entities[HA_MAX_ENTITIES];
static int s_entity_count;
static SemaphoreHandle_t s_mutex;           /* protects s_entities, s_link and s_change */
static ha_link_t s_link = HA_LINK_WIFI_CONNECTING;
static uint32_t s_change;
static QueueHandle_t s_cmd_queue;
static EventGroupHandle_t s_wifi_events;

static char s_base_url[128];
static char s_auth_header[300];
static char *s_http_buf;
static int s_http_len;

/* ---------------------------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------------------------- */

static const char *const s_domain_names[] = {
    [HA_DOMAIN_LIGHT] = "light",
    [HA_DOMAIN_SWITCH] = "switch",
    [HA_DOMAIN_INPUT_BOOLEAN] = "input_boolean",
    [HA_DOMAIN_FAN] = "fan",
    [HA_DOMAIN_COVER] = "cover",
    [HA_DOMAIN_MEDIA_PLAYER] = "media_player",
    [HA_DOMAIN_CLIMATE] = "climate",
    [HA_DOMAIN_LOCK] = "lock",
    [HA_DOMAIN_SCENE] = "scene",
    [HA_DOMAIN_SCRIPT] = "script",
    [HA_DOMAIN_OTHER] = "homeassistant",
};

static ha_domain_t domain_from_entity_id(const char *entity_id)
{
    const char *dot = strchr(entity_id, '.');
    size_t len = dot ? (size_t)(dot - entity_id) : 0;
    for (int d = 0; d < HA_DOMAIN_OTHER; d++) {
        if (strlen(s_domain_names[d]) == len && strncmp(entity_id, s_domain_names[d], len) == 0) {
            return d;
        }
    }
    return HA_DOMAIN_OTHER;
}

static void mark_changed_locked(void)
{
    s_change++;
}

static void set_link(ha_link_t link)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_link != link) {
        s_link = link;
        mark_changed_locked();
    }
    xSemaphoreGive(s_mutex);
}

static int find_entity_locked(const char *entity_id)
{
    for (int i = 0; i < s_entity_count; i++) {
        if (strcmp(s_entities[i].entity_id, entity_id) == 0) {
            return i;
        }
    }
    return -1;
}

static double json_number(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

/* ---------------------------------------------------------------------------------------------
 * Turning HA's JSON into our simplified ha_entity_t
 * ------------------------------------------------------------------------------------------- */

static void apply_state_json_locked(ha_entity_t *e, const cJSON *obj)
{
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(obj, "state");
    const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(obj, "attributes");
    if (!cJSON_IsString(state)) {
        return;
    }
    const char *s = state->valuestring;
    strlcpy(e->state, s, sizeof(e->state));

    const cJSON *fname = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
    if (cJSON_IsString(fname)) {
        strlcpy(e->name, fname->valuestring, sizeof(e->name));
    }

    e->available = strcmp(s, "unavailable") != 0 && strcmp(s, "unknown") != 0;

    switch (e->domain) {
    case HA_DOMAIN_COVER:
        e->is_on = strcmp(s, "open") == 0 || strcmp(s, "opening") == 0;
        break;
    case HA_DOMAIN_MEDIA_PLAYER:
        e->is_on = e->available && strcmp(s, "off") != 0 && strcmp(s, "standby") != 0;
        break;
    case HA_DOMAIN_CLIMATE:
        e->is_on = e->available && strcmp(s, "off") != 0;
        break;
    case HA_DOMAIN_LOCK:
        e->is_on = strcmp(s, "locked") == 0;
        break;
    default:
        e->is_on = strcmp(s, "on") == 0;
        break;
    }

    /* Work out whether the knob can adjust something, and what range it has */
    e->has_level = false;
    e->unit = "%";
    e->level_min = 0;
    e->level_max = 100;
    e->level_step = 5;
    switch (e->domain) {
    case HA_DOMAIN_LIGHT: {
        /* Only dimmable lights report supported_color_modes other than just "onoff" */
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(attrs, "supported_color_modes");
        bool dimmable = cJSON_IsArray(modes) && cJSON_GetArraySize(modes) > 0;
        if (dimmable && cJSON_GetArraySize(modes) == 1) {
            const cJSON *m = cJSON_GetArrayItem(modes, 0);
            dimmable = !(cJSON_IsString(m) && strcmp(m->valuestring, "onoff") == 0);
        }
        if (dimmable) {
            e->has_level = true;
            double bri = json_number(attrs, "brightness", e->is_on ? 255 : 0);  /* HA uses 0..255 */
            e->level = roundf((float)(bri * 100.0 / 255.0));
        }
        break;
    }
    case HA_DOMAIN_FAN:
        if (cJSON_GetObjectItemCaseSensitive(attrs, "percentage")) {
            e->has_level = true;
            e->level = (float)json_number(attrs, "percentage", 0);
            e->level_step = (float)json_number(attrs, "percentage_step", 10);
        }
        break;
    case HA_DOMAIN_COVER:
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(attrs, "current_position"))) {
            e->has_level = true;
            e->level = (float)json_number(attrs, "current_position", 0);
        }
        break;
    case HA_DOMAIN_MEDIA_PLAYER:
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(attrs, "volume_level"))) {
            e->has_level = true;
            e->level = roundf((float)(json_number(attrs, "volume_level", 0) * 100.0));
            e->level_step = 2;
        }
        break;
    case HA_DOMAIN_CLIMATE:
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(attrs, "temperature"))) {
            e->has_level = true;
            e->unit = "\xC2\xB0";  /* degree sign, UTF-8 */
            e->level = (float)json_number(attrs, "temperature", 20);
            e->level_min = (float)json_number(attrs, "min_temp", 7);
            e->level_max = (float)json_number(attrs, "max_temp", 35);
            e->level_step = (float)json_number(attrs, "target_temp_step", 0.5);
        }
        break;
    default:
        break;
    }
    if (e->level_step <= 0) {
        e->level_step = 1;
    }
    mark_changed_locked();
}

/* ---------------------------------------------------------------------------------------------
 * HTTP
 * ------------------------------------------------------------------------------------------- */

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int room = HTTP_BUF_SIZE - 1 - s_http_len;
        int n = evt->data_len < room ? evt->data_len : room;
        if (n > 0) {
            memcpy(s_http_buf + s_http_len, evt->data, n);
            s_http_len += n;
        }
    }
    return ESP_OK;
}

static esp_http_client_handle_t http_client_create(void)
{
    esp_http_client_config_t cfg = {
        .url = s_base_url,
        .event_handler = http_event_cb,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* only used for https:// URLs */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Authorization", s_auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    return client;
}

/* Run one request. Returns the HTTP status code, or -1 if HA could not be reached.
 * On return s_http_buf holds the (NUL terminated) response body. */
static int http_request(esp_http_client_handle_t client, esp_http_client_method_t method,
                        const char *path, const char *body)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base_url, path);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, method);
    esp_http_client_set_post_field(client, body, body ? strlen(body) : 0);

    s_http_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    s_http_buf[s_http_len] = '\0';
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", path, esp_err_to_name(err));
        esp_http_client_close(client);
        return -1;
    }
    int status = esp_http_client_get_status_code(client);
    if (status == 401) {
        ESP_LOGE(TAG, "Home Assistant rejected the token (401). Check CONFIG_KNOB_HA_TOKEN.");
    } else if (status >= 300) {
        ESP_LOGW(TAG, "%s -> HTTP %d: %.120s", path, status, s_http_buf);
    }
    return status;
}

static void update_link_from_status(int status)
{
    if (status == 401) {
        set_link(HA_LINK_AUTH_ERROR);
    } else if (status < 0) {
        set_link(HA_LINK_ERROR);
    } else {
        set_link(HA_LINK_OK);
    }
}

static void refresh_entity(esp_http_client_handle_t client, int idx)
{
    char path[96];
    snprintf(path, sizeof(path), "/api/states/%s", s_entities[idx].entity_id);
    int status = http_request(client, HTTP_METHOD_GET, path, NULL);
    update_link_from_status(status);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ha_entity_t *e = &s_entities[idx];
    if (status == 200) {
        cJSON *root = cJSON_Parse(s_http_buf);
        if (root) {
            apply_state_json_locked(e, root);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "could not parse state of %s (reply too large?)", e->entity_id);
        }
    } else if (status == 404) {
        if (e->available || strcmp(e->state, "not found") != 0) {
            ESP_LOGW(TAG, "entity %s does not exist in Home Assistant", e->entity_id);
            strlcpy(e->state, "not found", sizeof(e->state));
            e->available = false;
            mark_changed_locked();
        }
    }
    xSemaphoreGive(s_mutex);
}

static void call_service(esp_http_client_handle_t client, const ha_cmd_t *cmd)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ha_entity_t e = s_entities[cmd->idx];
    xSemaphoreGive(s_mutex);

    const char *domain = s_domain_names[e.domain];
    const char *service = "toggle";
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "entity_id", e.entity_id);

    if (cmd->type == CMD_TOGGLE) {
        switch (e.domain) {
        case HA_DOMAIN_SCENE:
        case HA_DOMAIN_SCRIPT:
            service = "turn_on";
            break;
        case HA_DOMAIN_CLIMATE:
            service = e.is_on ? "turn_off" : "turn_on";
            break;
        case HA_DOMAIN_LOCK:
            /* A tap only ever locks. Unlocking needs a long press so it can't happen by accident. */
            service = "lock";
            break;
        default:
            service = "toggle";
            break;
        }
    } else if (cmd->type == CMD_ALT) {
        if (e.domain != HA_DOMAIN_LOCK) {
            cJSON_Delete(body);
            return;
        }
        service = "unlock";
    } else {  /* CMD_LEVEL */
        float v = cmd->value;
        switch (e.domain) {
        case HA_DOMAIN_LIGHT:
            if (v <= 0) {
                service = "turn_off";
            } else {
                service = "turn_on";
                cJSON_AddNumberToObject(body, "brightness_pct", (int)lroundf(v));
            }
            break;
        case HA_DOMAIN_FAN:
            service = "set_percentage";
            cJSON_AddNumberToObject(body, "percentage", (int)lroundf(v));
            break;
        case HA_DOMAIN_COVER:
            service = "set_cover_position";
            cJSON_AddNumberToObject(body, "position", (int)lroundf(v));
            break;
        case HA_DOMAIN_MEDIA_PLAYER:
            service = "volume_set";
            cJSON_AddNumberToObject(body, "volume_level", v / 100.0);
            break;
        case HA_DOMAIN_CLIMATE:
            service = "set_temperature";
            cJSON_AddNumberToObject(body, "temperature", v);
            break;
        default:
            cJSON_Delete(body);
            return;
        }
    }

    char path[96];
    snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    ESP_LOGI(TAG, "POST %s %s", path, json);
    int status = http_request(client, HTTP_METHOD_POST, path, json);
    cJSON_free(json);
    update_link_from_status(status);

    /* HA replies with the list of states that changed during the call: apply them right away */
    if (status == 200) {
        cJSON *root = cJSON_Parse(s_http_buf);
        if (cJSON_IsArray(root)) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            const cJSON *item;
            cJSON_ArrayForEach(item, root) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "entity_id");
                int i = cJSON_IsString(id) ? find_entity_locked(id->valuestring) : -1;
                if (i >= 0) {
                    apply_state_json_locked(&s_entities[i], item);
                }
            }
            xSemaphoreGive(s_mutex);
        }
        cJSON_Delete(root);
    }
}

static void ha_task(void *arg)
{
    esp_http_client_handle_t client = http_client_create();
    TickType_t last_poll = 0;
    bool first = true;

    while (1) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            set_link(HA_LINK_WIFI_CONNECTING);
            xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
            set_link(HA_LINK_HA_CONNECTING);
            first = true;
        }

        TickType_t poll_ticks = pdMS_TO_TICKS(CONFIG_KNOB_HA_POLL_MS);
        TickType_t elapsed = xTaskGetTickCount() - last_poll;
        TickType_t wait = (first || elapsed >= poll_ticks) ? 0 : poll_ticks - elapsed;

        ha_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, wait) == pdTRUE) {
            call_service(client, &cmd);
            refresh_entity(client, cmd.idx);
            continue;
        }

        for (int i = 0; i < s_entity_count; i++) {
            refresh_entity(client, i);
            /* Stay responsive: if the user does something mid-refresh, handle it first */
            if (uxQueueMessagesWaiting(s_cmd_queue) > 0) {
                break;
            }
        }
        last_poll = xTaskGetTickCount();
        first = false;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Wi-Fi
 * ------------------------------------------------------------------------------------------- */

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_KNOB_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_KNOB_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    /* Power save adds ~100 ms latency to every request; a knob should feel instant */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to Wi-Fi \"%s\"", CONFIG_KNOB_WIFI_SSID);
}

/* ---------------------------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------------------------- */

static void parse_entity_list(void)
{
    char list[] = CONFIG_KNOB_HA_ENTITIES;
    char *save = NULL;
    for (char *tok = strtok_r(list, ", ", &save); tok && s_entity_count < HA_MAX_ENTITIES;
            tok = strtok_r(NULL, ", ", &save)) {
        ha_entity_t *e = &s_entities[s_entity_count++];
        strlcpy(e->entity_id, tok, sizeof(e->entity_id));
        strlcpy(e->name, tok, sizeof(e->name));
        strlcpy(e->state, "...", sizeof(e->state));
        e->domain = domain_from_entity_id(tok);
        e->unit = "%";
        ESP_LOGI(TAG, "entity %d: %s", s_entity_count - 1, tok);
    }
}

void ha_client_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(8, sizeof(ha_cmd_t));
    s_wifi_events = xEventGroupCreate();
    s_http_buf = malloc(HTTP_BUF_SIZE);
    assert(s_http_buf);

    strlcpy(s_base_url, CONFIG_KNOB_HA_URL, sizeof(s_base_url));
    size_t n = strlen(s_base_url);
    while (n > 0 && s_base_url[n - 1] == '/') {
        s_base_url[--n] = '\0';
    }
    snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", CONFIG_KNOB_HA_TOKEN);
    if (strlen(CONFIG_KNOB_HA_TOKEN) == 0) {
        ESP_LOGE(TAG, "No Home Assistant token set - run `idf.py menuconfig` -> HA Rotary Knob");
    }

    parse_entity_list();
    wifi_start();
    xTaskCreate(ha_task, "ha", 8192, NULL, 4, NULL);
}

int ha_entity_count(void)
{
    return s_entity_count;
}

bool ha_get_entity(int idx, ha_entity_t *out)
{
    if (idx < 0 || idx >= s_entity_count) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_entities[idx];
    xSemaphoreGive(s_mutex);
    return true;
}

ha_link_t ha_link_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ha_link_t link = s_link;
    xSemaphoreGive(s_mutex);
    return link;
}

uint32_t ha_change_counter(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t c = s_change;
    xSemaphoreGive(s_mutex);
    return c;
}

static void queue_cmd(cmd_type_t type, int idx, float value)
{
    if (idx < 0 || idx >= s_entity_count) {
        return;
    }
    ha_cmd_t cmd = { .type = type, .idx = idx, .value = value };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropping request");
    }
}

void ha_request_toggle(int idx)
{
    queue_cmd(CMD_TOGGLE, idx, 0);
}

void ha_request_alt_action(int idx)
{
    queue_cmd(CMD_ALT, idx, 0);
}

void ha_request_level(int idx, float value)
{
    /* Show the new value immediately; the next refresh confirms what HA actually did */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (idx >= 0 && idx < s_entity_count) {
        s_entities[idx].level = value;
        mark_changed_locked();
    }
    xSemaphoreGive(s_mutex);
    queue_cmd(CMD_LEVEL, idx, value);
}

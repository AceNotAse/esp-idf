/*
 * ha_client.h - Wi-Fi connection and the Home Assistant REST API.
 *
 * A background task keeps a local copy of every configured entity up to date
 * (by polling GET /api/states/<entity_id>) and runs service calls such as
 * light.toggle that the UI asks for.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HA_MAX_ENTITIES 16

typedef enum {
    HA_DOMAIN_LIGHT,
    HA_DOMAIN_SWITCH,
    HA_DOMAIN_INPUT_BOOLEAN,
    HA_DOMAIN_FAN,
    HA_DOMAIN_COVER,
    HA_DOMAIN_MEDIA_PLAYER,
    HA_DOMAIN_CLIMATE,
    HA_DOMAIN_LOCK,
    HA_DOMAIN_SCENE,
    HA_DOMAIN_SCRIPT,
    HA_DOMAIN_OTHER,
} ha_domain_t;

typedef struct {
    char entity_id[64];
    char name[48];          /* friendly_name from HA, or the entity_id until we hear back */
    char state[24];         /* raw HA state string, e.g. "on", "off", "playing", "locked" */
    ha_domain_t domain;
    bool available;         /* false when HA reports unavailable/unknown or we can't reach it */
    bool is_on;             /* simplified on/off view of the state */
    bool has_level;         /* true if the knob can adjust a value (brightness, volume...) */
    float level;            /* current value, in the units below */
    float level_min;
    float level_max;
    float level_step;       /* how much one knob click changes the value */
    const char *unit;       /* "%" or a degree sign */
} ha_entity_t;

typedef enum {
    HA_LINK_WIFI_CONNECTING,
    HA_LINK_HA_CONNECTING,
    HA_LINK_OK,
    HA_LINK_AUTH_ERROR,     /* HA answered 401: the token is wrong or was deleted */
    HA_LINK_ERROR,          /* HA unreachable: wrong URL, HA down, firewall... */
} ha_link_t;

/* Parse the entity list from menuconfig and start Wi-Fi + the HA task. */
void ha_client_start(void);

int ha_entity_count(void);

/* Copy entity idx into *out. Returns false if idx is out of range. */
bool ha_get_entity(int idx, ha_entity_t *out);

ha_link_t ha_link_state(void);

/* Bumped every time any entity or the link state changes, so the UI knows when to redraw. */
uint32_t ha_change_counter(void);

/* Queue a "tap" action: toggle, or turn_on for scenes/scripts, or lock for locks. */
void ha_request_toggle(int idx);

/* Queue the "long press" action for entities without a level (e.g. unlock a lock). */
void ha_request_alt_action(int idx);

/* Queue setting the entity's level (brightness %, volume %, target temperature...). */
void ha_request_level(int idx, float value);

/*
 * encoder.h - turns the rotary knob and its push button into simple events.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    KNOB_EVT_ROTATE,      /* knob turned: .steps is +N clockwise, -N counter-clockwise */
    KNOB_EVT_CLICK,       /* short press and release */
    KNOB_EVT_LONG_PRESS,  /* held down for CONFIG_KNOB_LONG_PRESS_MS */
} knob_evt_type_t;

typedef struct {
    knob_evt_type_t type;
    int steps;
} knob_evt_t;

/* Start the encoder task. Returns the queue that knob_evt_t events are posted to. */
QueueHandle_t encoder_start(void);

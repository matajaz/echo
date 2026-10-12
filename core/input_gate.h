#pragma once
#include <stdbool.h>
typedef struct { bool swallowing; } input_gate_t;
// Consume the entire physical stroke, even after waking/removing a lock.
static inline bool input_gate_filter(input_gate_t *g,bool pressed,bool blocked) {
    if (pressed && blocked) g->swallowing=true;
    bool consume=g->swallowing || blocked;
    if (!pressed) g->swallowing=false;
    return consume;
}

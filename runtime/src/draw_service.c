#include "draw_service.h"

static _Thread_local int is_owner;
static DrawServicePoll service;
static void *service_user;
static int in_service;
int g_draw_service_enabled;

int draw_service_active(void) { return is_owner && service != 0; }

void draw_service_register(DrawServicePoll callback, void *user) {
    is_owner = 1;
    service = callback;
    service_user = user;
    g_draw_service_enabled = callback != 0;
}

void draw_service_poll(void) {
    if (!is_owner || in_service || !service) return;
    in_service = 1;
    service(service_user);
    in_service = 0;
}

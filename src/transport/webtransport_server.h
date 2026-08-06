#pragma once

typedef struct {
    int placeholder;
} webtransport_server_t;

int webtransport_server_init(webtransport_server_t *server, int port);

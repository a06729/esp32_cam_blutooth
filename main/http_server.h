#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

httpd_handle_t http_server_start(void);
void           http_server_stop(httpd_handle_t server);

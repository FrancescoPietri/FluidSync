//FluidSync - web interface: HTTP parsing and replies. The page itself (graphics) is web/index.html.
//No machine logic here: requests are turned into a web_request_t, the logic decides what to do with it.
#pragma once

#include <stdbool.h>

#include "FluidSyncConfig.h"

typedef enum {
    WEB_NOT_FOUND = 0,
    WEB_BAD,            //known path, invalid parameters
    WEB_PAGE,           //GET /            -> the web app
    WEB_CONFIG,         //GET /cfg         -> config + queue as JSON
    WEB_ORDER,          //GET /order?p=N   -> order preset N
    WEB_SET_LIQUID,     //GET /liquid?i=N&n=NAME&c=RRGGBB
    WEB_SET_PRESET      //GET /preset?i=N&n=NAME&ml=VOLUME&v=P1,P2,..   (empty name = delete)
} web_req_type_t;

typedef struct {
    web_req_type_t type;
    int index;              //preset (WEB_ORDER, WEB_SET_PRESET) or pump (WEB_SET_LIQUID)
    liquid_cfg_t liquid;    //WEB_SET_LIQUID
    preset_cfg_t preset;    //WEB_SET_PRESET
} web_request_t;

//parses the HTTP request line. Numbers are range-checked and names sanitized (printable ASCII, no "\<>&%,) here
void web_parse(const char *req, web_request_t *out);

//replies, each one closes the connection
void web_reply_page(int id);
void web_reply_config(int id, const machine_config_t *cfg, int queue_len, int queue_max);
void web_reply_text(int id, int status, const char *text); //status: 200, 400, 404, 409

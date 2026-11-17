//Host test for the web request parser (no hardware needed):
//  gcc -std=c11 -I. -Ibuild -o /tmp/test_web test/test_web.c FluidSyncWeb.c && /tmp/test_web
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "FluidSyncWeb.h"
static char sent[4096]; static int sent_n;
bool hw_wifi_send(int id_, const char *d, int len){ (void)id_; memcpy(sent + sent_n, d, len); sent_n += len; return true; }
void hw_wifi_close(int id_){ (void)id_; }
int main(void){
    web_request_t r;
    web_parse("GET / HTTP/1.1\r\nHost: x", &r);               assert(r.type == WEB_PAGE);
    web_parse("GET /cfg HTTP/1.1", &r);                       assert(r.type == WEB_CONFIG);
    web_parse("GET /order?p=3 HTTP/1.1", &r);                 assert(r.type == WEB_ORDER && r.index == 3);
    web_parse("GET /order?p=8 HTTP/1.1", &r);                 assert(r.type == WEB_BAD);
    web_parse("GET /order?p=-1 HTTP/1.1", &r);                assert(r.type == WEB_BAD);
    web_parse("GET /order HTTP/1.1", &r);                     assert(r.type == WEB_BAD);
    web_parse("POST /order?p=1 HTTP/1.1", &r);                assert(r.type == WEB_NOT_FOUND);
    web_parse("GET /favicon.ico HTTP/1.1", &r);               assert(r.type == WEB_NOT_FOUND);
    web_parse("GET /liquid?i=2&n=Blue%20%3Cb%3E+ink&c=00a3e0 HTTP/1.1", &r);
    assert(r.type == WEB_SET_LIQUID && r.index == 2 && !strcmp(r.liquid.name, "Blue b ink") && r.liquid.color == 0x00a3e0);
    web_parse("GET /liquid?i=2&n=AVeryVeryLongLiquidName&c=00a3e0 HTTP/1.1", &r);
    assert(r.type == WEB_SET_LIQUID && strlen(r.liquid.name) == CFG_NAME_LEN - 1);
    web_parse("GET /liquid?i=6&n=X&c=00a3e0 HTTP/1.1", &r);    assert(r.type == WEB_BAD);
    web_parse("GET /liquid?i=1&n=%22%3C&c=00a3e0 HTTP/1.1", &r); assert(r.type == WEB_BAD); //name empty after cleaning
    web_parse("GET /liquid?i=1&n=X&c=zz0000 HTTP/1.1", &r);    assert(r.type == WEB_BAD);
    web_parse("GET /liquid?i=1&n=X&c=fff HTTP/1.1", &r);       assert(r.type == WEB_BAD);
    web_parse("GET /preset?i=0&n=Green&ml=150&v=30,0,70,0,0,0 HTTP/1.1", &r);
    assert(r.type == WEB_SET_PRESET && !strcmp(r.preset.name, "Green") && r.preset.volume_ml == 150 && r.preset.pct[0] == 30 && r.preset.pct[2] == 70);
    web_parse("GET /preset?i=0&n=G&ml=99&v=100,0,0,0,0,0 HTTP/1.1", &r);  assert(r.type == WEB_BAD);
    web_parse("GET /preset?i=0&n=G&ml=200&v=100,0,0,0,0,0 HTTP/1.1", &r); assert(r.type == WEB_BAD);
    web_parse("GET /preset?i=0&n=G&ml=199&v=100,0,0,0,0,0 HTTP/1.1", &r); assert(r.type == WEB_SET_PRESET && r.preset.volume_ml == 199);
    web_parse("GET /preset?i=0&n=G&v=100,0,0,0,0,0 HTTP/1.1", &r);        assert(r.type == WEB_BAD); //volume missing
    web_parse("GET /preset?i=0&n=&ml=100&v=0,0,0,0,0,0 HTTP/1.1", &r);      assert(r.type == WEB_SET_PRESET && !r.preset.name[0]);
    web_parse("GET /preset?i=0&n=G&ml=100&v=30,0,70,0,0 HTTP/1.1", &r);     assert(r.type == WEB_BAD);
    web_parse("GET /preset?i=0&n=G&ml=100&v=30,0,70,0,0,0,1 HTTP/1.1", &r); assert(r.type == WEB_BAD);
    web_parse("GET /preset?i=0&n=G&ml=100&v=101,0,0,0,0,0 HTTP/1.1", &r);   assert(r.type == WEB_BAD);
    web_parse("GET /preset?i=0&n=G&ml=100&v=a,0,0,0,0,0 HTTP/1.1", &r);     assert(r.type == WEB_BAD);
    web_parse("GET /preset?n=G&ml=100&v=0,0,0,0,0,0 HTTP/1.1", &r);         assert(r.type == WEB_BAD);

    machine_config_t c = {0};
    for(int i = 0; i < N_PUMPS; i++){ strcpy(c.liquid[i].name, "Liquid 123456789"); c.liquid[i].name[15] = 0; c.liquid[i].color = 0xABCDEF; }
    for(int i = 0; i < CFG_N_PRESETS; i++){ strcpy(c.preset[i].name, "Preset 12345678"); c.preset[i].volume_ml = 199; for(int k = 0; k < N_PUMPS; k++) c.preset[i].pct[k] = 100; }
    web_reply_config(0, &c, 10, 10);
    sent[sent_n] = 0;
    char *body = strstr(sent, "\r\n\r\n");
    assert(strstr(sent, "200 OK") && body && body[strlen(body) - 1] == '}');
    printf("worst-case JSON body: %zu bytes\nall web_parse tests passed\n", strlen(body + 4));
    return 0;
}

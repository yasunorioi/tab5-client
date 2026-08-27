// web_server.c — see web_server.h. Read-only HTTP status server on esp_http_server.

#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "mbedtls/base64.h"

#include "wifi_sta.h"
#include "usb_cdc_source.h"
#include "gnss_state.h"

static const char *TAG = "web";

static httpd_handle_t s_httpd;   // NULL until started; guards idempotency

// ── helpers ──────────────────────────────────────────────────────────────────

// Escape `in` for embedding in a JSON string literal, into `out` (always
// NUL-terminated, truncated to fit). Control chars are dropped; SSIDs can carry
// arbitrary bytes, so this must not be skipped.
static void json_str(char *out, size_t out_max, const char *in)
{
    size_t o = 0;
    if (out_max == 0) return;
    for (size_t i = 0; in && in[i] && o + 2 < out_max; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
        // else: other control chars dropped
    }
    out[o] = '\0';
}

// Format a double as a JSON number, or "null" if Do-Not-Use (NaN).
static void json_num(char *out, size_t n, double v, int dec)
{
    if (isnan(v)) snprintf(out, n, "null");
    else          snprintf(out, n, "%.*f", dec, v);
}

// ── GET /api/status ──────────────────────────────────────────────────────────
// Built by chunked sends so no single large buffer is needed. Stateless: raw
// counters + latest decoded SBF values; the page computes rates from polls.
static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t    w;  wifi_sta_status(&w);
    usb_cdc_status_t u;  usb_cdc_source_status(&u);
    gnss_snapshot_t  g;  gnss_state_snapshot(&g);

    const int64_t now = esp_timer_get_time();
    const long long ms_since = g.last_block_us ? (now - g.last_block_us) / 1000 : -1;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char line[320];
    char esc[128];

    httpd_resp_sendstr_chunk(req, "{");

    // wifi
    json_str(esc, sizeof esc, w.ssid);
    snprintf(line, sizeof line,
             "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"},",
             w.connected ? "true" : "false", esc, w.ip);
    httpd_resp_sendstr_chunk(req, line);

    // usb
    json_str(esc, sizeof esc, u.topo);
    snprintf(line, sizeof line,
             "\"usb\":{\"host\":%s,\"attached\":%s,\"open\":%s,"
             "\"vid\":%u,\"pid\":%u,\"num_if\":%u,\"cur_itf\":%d,"
             "\"stream_itf\":%d,\"topo\":\"%s\"},",
             u.host_installed ? "true" : "false",
             u.device_attached ? "true" : "false",
             u.cdc_open ? "true" : "false",
             u.vid, u.pid, u.num_interfaces,
             (int)(int8_t)u.cur_itf, (int)(int8_t)u.stream_itf, esc);
    httpd_resp_sendstr_chunk(req, line);

    // sbf (fix + attitude + height)
    char h[24], ha[16], va[16], pit[16], rol[16], hed[16];
    json_num(h,   sizeof h,   g.pvt_valid ? g.pvt.height_m     : NAN, 3);
    json_num(ha,  sizeof ha,  g.pvt_valid ? g.pvt.h_accuracy_m : NAN, 3);
    json_num(va,  sizeof va,  g.pvt_valid ? g.pvt.v_accuracy_m : NAN, 3);
    json_num(pit, sizeof pit, g.att_valid ? g.att.pitch_deg    : NAN, 2);
    json_num(rol, sizeof rol, g.att_valid ? g.att.roll_deg     : NAN, 2);
    json_num(hed, sizeof hed, g.att_valid ? g.att.heading_deg  : NAN, 1);
    snprintf(line, sizeof line,
             "\"sbf\":{\"blocks\":%lu,\"crc_fails\":%lu,\"ms_since_last\":%lld,"
             "\"fix\":\"%s\",\"mode\":%u,\"sv\":%u,"
             "\"height\":%s,\"hacc\":%s,\"vacc\":%s,"
             "\"pitch\":%s,\"roll\":%s,\"heading\":%s}}",
             (unsigned long)g.valid_blocks, (unsigned long)g.crc_failed, ms_since,
             g.pvt_valid ? sbf_pvt_mode_str(g.pvt.mode_type) : "-",
             g.pvt_valid ? g.pvt.mode_type : 0,
             g.pvt_valid ? g.pvt.nr_sv : 0,
             h, ha, va, pit, rol, hed);
    httpd_resp_sendstr_chunk(req, line);

    httpd_resp_sendstr_chunk(req, NULL);   // end of response
    return ESP_OK;
}

// ── GET / ────────────────────────────────────────────────────────────────────
// Self-contained dashboard: polls /api/status once a second and renders it.
// No external assets (offline box), matches the panel's dark background.
static const char INDEX_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>tab5-client</title><style>"
"body{margin:0;font:14px/1.5 system-ui,sans-serif;background:#081420;color:#cfe3f2}"
"header{padding:14px 18px;background:#0d1f30;border-bottom:1px solid #1b3550}"
"header b{font-size:17px}header span{color:#6b8bab;margin-left:8px}"
".wrap{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));padding:16px}"
".card{background:#0d1f30;border:1px solid #1b3550;border-radius:10px;padding:14px 16px}"
".card h2{margin:0 0 10px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#6b8bab}"
".row{display:flex;justify-content:space-between;gap:12px;padding:3px 0}"
".row .k{color:#8fb0cd}.row .v{font-variant-numeric:tabular-nums;text-align:right}"
".ok{color:#5fe08a}.bad{color:#ff6b6b}.dim{color:#5a7794}"
".pill{display:inline-block;padding:1px 8px;border-radius:20px;font-size:12px}"
".pill.ok{background:#123d24}.pill.bad{background:#3d1414}"
"footer{padding:10px 18px;color:#4a688a;font-size:12px}"
"</style></head><body>"
"<header><b>tab5-client</b><span id=host></span></header>"
"<div class=wrap>"
"<div class=card><h2>Fix</h2><div id=fix></div></div>"
"<div class=card><h2>Attitude / Height</h2><div id=att></div></div>"
"<div class=card><h2>Mosaic / USB</h2><div id=usb></div></div>"
"<div class=card><h2>WiFi</h2><div id=wifi></div></div>"
"</div>"
"<footer><span id=foot>connecting…</span> &middot; <a href=/admin style=\"color:#6b8bab\">admin config</a></footer>"
"<script>"
"function b(x){return x?'<span class=\"pill ok\">yes</span>':'<span class=\"pill bad\">no</span>'}"
"function r(k,v){return '<div class=row><span class=k>'+k+'</span><span class=v>'+v+'</span></div>'}"
"function num(n){return (n||0).toLocaleString()}"
"function fmt(v,u){return v==null?'<span class=dim>—</span>':(v+(u||''))}"
"function tick(){fetch('/api/status').then(function(x){return x.json()}).then(function(d){"
"document.getElementById('host').textContent=d.wifi.ip?('@ '+d.wifi.ip):'';"
"var age=d.sbf.ms_since_last;var fresh=age>=0&&age<3000;"
"document.getElementById('fix').innerHTML="
"r('stream',b(fresh))+r('solution',fresh?d.sbf.fix:'<span class=dim>—</span>')+"
"r('satellites',d.sbf.sv)+"
"r('CRC fails',(d.sbf.crc_fails?'<span class=bad>':'<span class=ok>')+num(d.sbf.crc_fails)+'</span>')+"
"r('last block',age<0?'<span class=dim>never</span>':(age<3000?'<span class=ok>'+age+' ms</span>':'<span class=bad>'+num(age)+' ms</span>'));"
"document.getElementById('att').innerHTML="
"r('pitch',fmt(d.sbf.pitch,'°'))+r('roll',fmt(d.sbf.roll,'°'))+r('heading',fmt(d.sbf.heading,'°'))+"
"r('height',fmt(d.sbf.height,' m'))+r('v-acc',fmt(d.sbf.vacc,' m'));"
"document.getElementById('usb').innerHTML="
"r('host',b(d.usb.host))+r('attached',b(d.usb.attached))+r('cdc open',b(d.usb.open))+"
"r('device',d.usb.vid?(hex(d.usb.vid)+':'+hex(d.usb.pid)):'<span class=dim>—</span>')+"
"r('stream itf',d.usb.stream_itf<0?'<span class=dim>—</span>':d.usb.stream_itf)+"
"r('topology','<span class=dim>'+(d.usb.topo||'')+'</span>');"
"document.getElementById('wifi').innerHTML="
"r('connected',b(d.wifi.connected))+r('SSID',d.wifi.ssid||'<span class=dim>—</span>')+"
"r('IP',d.wifi.ip||'<span class=dim>—</span>');"
"document.getElementById('foot').textContent='updated '+new Date().toLocaleTimeString();"
"}).catch(function(e){document.getElementById('foot').textContent='unreachable — retrying…'})}"
"function hex(n){return ('000'+n.toString(16).toUpperCase()).slice(-4)}"
"tick();setInterval(tick,1000);"
"</script></body></html>";

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// ── admin: Basic-auth credentials (NVS) ──────────────────────────────────────
#define NVS_NS_ADMIN "webadmin"

void web_admin_set_creds(const char *user, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS_ADMIN, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "user", user ? user : ""));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass ? pass : ""));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "admin credentials saved (user=%s) — /admin writes enabled",
             user ? user : "");
}

void web_admin_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_ADMIN, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "admin credentials erased — /admin writes disabled");
}

// Load stored admin creds. Returns false (→ writes disabled) if unset.
static bool admin_creds_load(char *user, size_t ul, char *pass, size_t pl)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_ADMIN, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = ul; bool ok = nvs_get_str(h, "user", user, &n) == ESP_OK;
    n = pl; ok = ok && nvs_get_str(h, "pass", pass, &n) == ESP_OK;
    nvs_close(h);
    return ok && user[0] != '\0';
}

// Validate the request's "Authorization: Basic <b64(user:pass)>" against stored
// creds. Not constant-time (hobby scope, single-user, LAN).
static bool check_basic(const char *authhdr, const char *user, const char *pass)
{
    const char *sp = strchr(authhdr, ' ');
    if (!sp) return false;
    const char *tok = sp + 1;
    unsigned char dec[128];
    size_t olen = 0;
    if (mbedtls_base64_decode(dec, sizeof dec - 1, &olen,
                              (const unsigned char *)tok, strlen(tok)) != 0)
        return false;
    dec[olen] = '\0';
    char expect[128];
    int n = snprintf(expect, sizeof expect, "%s:%s", user, pass);
    if (n <= 0 || (size_t)n >= sizeof expect) return false;
    return olen == (size_t)n && memcmp(dec, expect, olen) == 0;
}

// Gate a write handler. Returns true if authorized; otherwise sends the right
// response (503 if no password configured, 401 to prompt the browser) and the
// caller must return ESP_OK without writing anything else.
static bool require_admin(httpd_req_t *req)
{
    char user[33], pass[65];
    if (!admin_creds_load(user, sizeof user, pass, sizeof pass)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "admin password not set — run 'webadmin <user> <pass>' "
                                "on the USB console first\n");
        return false;
    }
    char hdr[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        check_basic(hdr, user, pass))
        return true;

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"tab5-client admin\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "auth required\n");
    return false;
}

// ── admin: request-body form parsing ─────────────────────────────────────────
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL-decode in place ('+' → space, %xx → byte).
static void urldecode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = hexval(p[1]), lo = hexval(p[2]);
            if (hi >= 0 && lo >= 0) { *o++ = (char)((hi << 4) | lo); p += 2; }
            else *o++ = *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

// Read the full request body into buf (NUL-terminated). Returns length or -1.
static int recv_body(httpd_req_t *req, char *buf, size_t max)
{
    size_t total = req->content_len;
    if (total >= max) return -1;
    size_t off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    buf[off] = '\0';
    return (int)off;
}

// Extract + URL-decode one application/x-www-form-urlencoded field.
static bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    if (httpd_query_key_value(body, key, out, outlen) != ESP_OK) return false;
    urldecode(out);
    return true;
}

// ── POST /admin/mosaic ───────────────────────────────────────────────────────
// Passthrough: send a raw Septentrio command and return the receiver's reply.
static esp_err_t mosaic_post(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;

    char body[256];
    if (recv_body(req, body, sizeof body) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    char cmd[160] = {0};
    if (!form_field(body, "cmd", cmd, sizeof cmd) || cmd[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need cmd");
        return ESP_OK;
    }

    char reply[512];
    size_t rlen = 0;
    esp_err_t e = usb_cdc_send_command(cmd, reply, sizeof reply, &rlen, 1000);
    httpd_resp_set_type(req, "text/plain");
    if (e == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "(no Mosaic CDC interface open)");
        return ESP_OK;
    }
    if (e != ESP_OK || rlen == 0) {
        httpd_resp_sendstr(req, "(command sent — no reply captured)");
        return ESP_OK;
    }
    // The reply port also tees binary SBF; sanitise non-printables for display
    // (same convention as the `mosaic` console command).
    for (size_t i = 0; i < rlen; i++) {
        unsigned char c = (unsigned char)reply[i];
        if (c != '\r' && c != '\n' && c != '\t' && (c < 0x20 || c > 0x7e))
            reply[i] = '.';
    }
    httpd_resp_send(req, reply, rlen);
    return ESP_OK;
}

// ── deferred reboot ──────────────────────────────────────────────────────────
// Handlers that reboot (WiFi change, reboot button) must send their HTTP
// response first, so the actual esp_restart() runs from a short-lived task.
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));   // let the response flush + socket close
    esp_restart();
}

static void schedule_reboot(void)
{
    xTaskCreate(reboot_task, "webreboot", 2048, NULL, 5, NULL);
}

// ── POST /admin/wifi ─────────────────────────────────────────────────────────
// Set (or forget) WiFi creds and reboot to apply. NOTE: this only helps while
// the box is already reachable — it cannot solve first-join provisioning (you
// need a link to reach this page). Field first-join is still console/BLE.
static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;

    char body[256];
    if (recv_body(req, body, sizeof body) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");

    char forget[4] = {0};
    if (form_field(body, "forget", forget, sizeof forget) && strcmp(forget, "1") == 0) {
        wifi_clear_creds();
        httpd_resp_sendstr(req, "WiFi creds erased — rebooting");
        schedule_reboot();
        return ESP_OK;
    }

    char ssid[33] = {0}, pass[65] = {0};
    form_field(body, "ssid", ssid, sizeof ssid);
    form_field(body, "pass", pass, sizeof pass);
    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ssid");
        return ESP_OK;
    }
    wifi_save_creds(ssid, pass);
    char msg[80];
    snprintf(msg, sizeof msg, "saved '%s' — rebooting to join", ssid);
    httpd_resp_sendstr(req, msg);
    schedule_reboot();
    return ESP_OK;
}

// ── POST /admin/reboot ───────────────────────────────────────────────────────
static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "rebooting…");
    schedule_reboot();
    return ESP_OK;
}

// ── GET /admin ───────────────────────────────────────────────────────────────
// Config page. Loading it triggers the Basic-auth prompt; the browser then
// reuses the cached creds for the /admin/* POSTs (same path prefix + realm).
static const char ADMIN_HTML[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>tab5-client admin</title><style>"
"body{margin:0;font:14px/1.5 system-ui,sans-serif;background:#081420;color:#cfe3f2}"
"header{padding:14px 18px;background:#0d1f30;border-bottom:1px solid #1b3550}"
"header b{font-size:17px}header a{color:#6b8bab;margin-left:10px;font-size:13px}"
".wrap{max-width:560px;margin:0 auto;padding:16px;display:grid;gap:14px}"
".card{background:#0d1f30;border:1px solid #1b3550;border-radius:10px;padding:14px 16px}"
".card h2{margin:0 0 10px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#6b8bab}"
"label{display:block;margin:8px 0 2px;color:#8fb0cd;font-size:13px}"
"input{width:100%;box-sizing:border-box;background:#0a1826;border:1px solid #1b3550;"
"border-radius:6px;color:#cfe3f2;padding:7px 9px;font:inherit}"
".row{display:flex;gap:10px;margin-top:12px}"
"button{background:#123d24;color:#5fe08a;border:1px solid #1d5a37;border-radius:6px;"
"padding:8px 14px;font:inherit;cursor:pointer}button.warn{background:#3d1414;color:#ff9b9b;border-color:#5a1d1d}"
".msg{margin-top:10px;font-size:13px;color:#8fb0cd;white-space:pre-wrap;font-variant-numeric:tabular-nums}"
"pre{background:#0a1826;border:1px solid #1b3550;border-radius:6px;padding:8px;overflow:auto;max-height:200px;font-size:12px}"
"</style></head><body>"
"<header><b>tab5-client</b><a href=/>&larr; status</a></header>"
"<div class=wrap>"
"<div class=card><h2>Mosaic passthrough</h2>"
"<label>Septentrio command</label><input id=mc placeholder=getSBFOutput>"
"<div class=row><button onclick=sendM()>Send</button></div>"
"<pre id=mout style=display:none></pre></div>"
"<div class=card><h2>WiFi</h2>"
"<label>SSID</label><input id=ws>"
"<label>password</label><input id=wpw type=password placeholder=\"(unchanged shown blank)\">"
"<div class=row><button onclick=saveWifi()>Save &amp; reboot</button>"
"<button class=warn onclick=forgetWifi()>Forget</button></div>"
"<div class=msg id=wmsg>changing WiFi reboots the box (and drops this page).</div></div>"
"<div class=card><h2>Device</h2>"
"<div class=row><button class=warn onclick=reboot()>Reboot</button></div>"
"<div class=msg id=dmsg></div></div>"
"</div>"
"<script>"
"function post(path,data,el){"
"var b=Object.keys(data).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(data[k])}).join('&');"
"return fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
".then(function(r){return r.text().then(function(t){return {ok:r.ok,t:t}})})}"
"function sendM(){mout.style.display='block';mout.textContent='…';"
"post('/admin/mosaic',{cmd:mc.value}).then(function(r){mout.textContent=r.t})}"
"function saveWifi(){var d={ssid:ws.value};if(wpw.value)d.pass=wpw.value;"
"post('/admin/wifi',d).then(function(r){wmsg.textContent=r.t})}"
"function forgetWifi(){if(!confirm('Erase WiFi creds and reboot?'))return;"
"post('/admin/wifi',{forget:'1'}).then(function(r){wmsg.textContent=r.t})}"
"function reboot(){if(!confirm('Reboot the box?'))return;dmsg.textContent='…';"
"post('/admin/reboot',{}).then(function(r){dmsg.textContent=r.t}).catch(function(){dmsg.textContent='rebooting…'})}"
"fetch('/api/status').then(function(r){return r.json()}).then(function(d){"
"if(d.wifi){ws.value=d.wifi.ssid||''}"
"}).catch(function(){});"
"</script></body></html>";

static esp_err_t admin_get(httpd_req_t *req)
{
    if (!require_admin(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, ADMIN_HTML, HTTPD_RESP_USE_STRLEN);
}

// ── start ────────────────────────────────────────────────────────────────────
esp_err_t web_server_start(void)
{
    if (s_httpd) return ESP_OK;   // idempotent

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WEB_ADMIN_PORT;
    cfg.lru_purge_enable = true;   // reclaim the oldest idle socket under pressure
    cfg.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }

    const httpd_uri_t index_uri  = { .uri = "/",           .method = HTTP_GET,  .handler = index_get };
    const httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get };
    const httpd_uri_t admin_uri  = { .uri = "/admin",      .method = HTTP_GET,  .handler = admin_get };
    const httpd_uri_t mos_uri    = { .uri = "/admin/mosaic",   .method = HTTP_POST, .handler = mosaic_post };
    const httpd_uri_t wifi_uri   = { .uri = "/admin/wifi",     .method = HTTP_POST, .handler = wifi_post };
    const httpd_uri_t reboot_uri = { .uri = "/admin/reboot",   .method = HTTP_POST, .handler = reboot_post };
    httpd_register_uri_handler(s_httpd, &status_uri);
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &admin_uri);
    httpd_register_uri_handler(s_httpd, &mos_uri);
    httpd_register_uri_handler(s_httpd, &wifi_uri);
    httpd_register_uri_handler(s_httpd, &reboot_uri);

    ESP_LOGI(TAG, "status web UI on :%u  (http://rtk.local:%u/  admin at /admin)",
             WEB_ADMIN_PORT, WEB_ADMIN_PORT);
    return ESP_OK;
}

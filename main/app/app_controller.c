#include "app/app_controller.h"
#include "app/navigation_model.h"
#include "web/web_session.h"
#include "web/web_server.h"
#include "web/web_templates.h"
#include "rs485/rs485_runtime.h"
#include "rs485/rs485_parser.h"
#include "rs485/rs485_simui.h"
#include "rs485/rs485_driver.h"
#include "rs485/rs485_engine.h"
#include "rs485_bridge/rs485_bridge.h"
#include "can_module/nm2k_module/nm2k_module.h"
#include "can_module/bridge_can_module/bridge_can_module.h"
#include "can_module/protocol/include/sailor_status_format.h"
#include "AISdecoder/aisdecoder_runtime.h"
#include "AISdecoder/aisdecoder.h"
#include "nmea_editor/nmea_templates.h"
#include "nmea_editor/nmea_version.h"
#include "nmea_editor/nmea_editor.h"
#include "ui/screens/screen_ui.h"
#include "ui/screens/screen_web.h"
#include "ui/screens/screen_init.h"
#include "ui/screens/screen_wifi.h"
#include "ui/ui_theme.h"
#include "ui/nmea_log.h"
#include "lvgl_port/waveshare_lvgl_port.h"
#include "wifi/wifi_manager.h"
#include "system/telnet_server.h"
#include "system/udp_nmea_server.h"
#include "system/telnet_router.h"
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define COMMANDS 12
#define RESPONSE_CACHE 32
typedef enum { CMD_REMOTE, CMD_LOCAL_MODE, CMD_LOCAL_BAUD, CMD_LOCAL_N2K_SPEED, CMD_LOCAL_SAILOR_BAUD, CMD_SCAN_RESULT } command_kind_t;
typedef struct { command_kind_t kind; int peer; uint32_t epoch; uint32_t value; bool local_at_submit; char *text; } command_t;
typedef struct { uint32_t id, hash; esp_err_t result; } reply_t;
static QueueHandle_t s_commands;
static web_session_t s_session;
static atomic_bool s_web, s_return_requested, s_scan_busy;
static atomic_uint s_epoch;
static atomic_int s_peer, s_lost_peer;
static reply_t s_replies[RESPONSE_CACHE];
static uint32_t s_highest_id, s_reply_pos;
static app_mode_t s_mode;
static uint32_t s_cursors[3], s_pending_cursors[3], s_stream_sent[3];
static bool s_stream_pending[3];
static uint8_t s_terminal_pending[256];
static size_t s_terminal_length;
static uint32_t s_terminal_seq, s_terminal_sent;
static uint32_t s_last_state;
static bool s_top_was_hidden;
static const char *TAG = "app_control";
static const char *mode_names[] = { "idle", "rx485", "tx485", "rs485_bridge", "n2k", "sailor" };

static uint64_t now_(void) { return (uint64_t)esp_timer_get_time()/1000u; }
static void *ps_alloc_(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static const char *str_(const cJSON *j,const char *key) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool number_(const cJSON *j,const char *key,double min,double max,double *out) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<min || v->valuedouble>max) return false;
    *out=v->valuedouble; return true;
}
static bool boolean_(const cJSON *j,const char *key,bool *out) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if (!cJSON_IsBool(v)) return false;
    *out=cJSON_IsTrue(v); return true;
}
static bool send_json_(int peer,cJSON *j) {
    if (!j) return false;
    char *text=cJSON_PrintUnformatted(j);
    bool ok=text && web_server_send(peer,text,strlen(text));
    cJSON_free(text); cJSON_Delete(j); return ok;
}
static void ack_(int peer,uint32_t id,esp_err_t err) {
    cJSON *j=cJSON_CreateObject(); if (!j) return;
    cJSON_AddStringToObject(j,"type","ack"); cJSON_AddNumberToObject(j,"id",id);
    cJSON_AddBoolToObject(j,"ok",err==ESP_OK);
    cJSON_AddStringToObject(j,"error",esp_err_to_name(err)); (void)send_json_(peer,j);
}
static app_mode_t detect_mode_(void) {
    if (rs485_parser_runtime_running()) return APP_MODE_RX485;
    if (rs485_simui_runtime_running()) return APP_MODE_TX485;
    if (rs485_bridge_runtime_running()) return APP_MODE_RS485_BRIDGE;
    if (nm2k_runtime_running()) return APP_MODE_N2K;
    if (bridge_can_runtime_running()) return APP_MODE_SAILOR;
    /* A worker may exit after a timeout while its port still needs cleanup. */
    switch(rs485_get_owner()) {
    case RS485_OWNER_PARSER:return APP_MODE_RX485;
    case RS485_OWNER_TRANSMITTER:return APP_MODE_TX485;
    case RS485_OWNER_NETWORK_BRIDGE:return APP_MODE_RS485_BRIDGE;
    case RS485_OWNER_CAN_BRIDGE:return APP_MODE_SAILOR;
    default:return APP_MODE_IDLE;
    }
}
static void suspend_views_(void) {
    rs485_parser_view_suspend(true); rs485_simui_view_suspend(true);
    rs485_bridge_view_suspend(true); nm2k_view_suspend(); bridge_can_view_suspend();
    aisdecoder_view_suspend(true);
    nmea_editor_suspend(true);
    screen_wifi_suspend(true);
    nmea_log_view_suspend(true);
}
static void destroy_views_(void) {
    aisdecoder_view_destroy();
    rs485_parser_view_destroy(); rs485_simui_view_destroy();
    rs485_bridge_view_destroy(); nm2k_view_destroy(); bridge_can_view_destroy();
}
static void local_view_(void) {
    lv_obj_t *parent=screen_ui_get_main();
    lv_obj_t *screen=NULL;
    switch(s_mode) {
    case APP_MODE_RX485: screen=rs485_parser_view_show(parent); break;
    case APP_MODE_TX485: screen=rs485_simui_view_show(parent); break;
    case APP_MODE_RS485_BRIDGE: screen=rs485_bridge_view_show(parent); break;
    case APP_MODE_N2K: screen=nm2k_view_show(parent); break;
    case APP_MODE_SAILOR: screen=bridge_can_view_show(parent); break;
    default: screen_ui_show(); break;
    }
    if (screen) lv_scr_load(screen);
    else if (s_mode != APP_MODE_IDLE) { ESP_LOGE(TAG,"View allocation failed"); screen_ui_show(); }
    nmea_log_view_suspend(false);
    screen_web_hide();
}
static esp_err_t stop_runtime_(void) {
    switch (s_mode) {
    case APP_MODE_RX485: return rs485_parser_runtime_stop()?ESP_OK:ESP_ERR_TIMEOUT;
    case APP_MODE_TX485: return rs485_simui_runtime_stop()?ESP_OK:ESP_ERR_TIMEOUT;
    case APP_MODE_RS485_BRIDGE: return rs485_bridge_runtime_stop()?ESP_OK:ESP_ERR_TIMEOUT;
    case APP_MODE_N2K: return nm2k_runtime_stop()?ESP_OK:ESP_ERR_TIMEOUT;
    case APP_MODE_SAILOR: return bridge_can_runtime_stop()?ESP_OK:ESP_ERR_TIMEOUT;
    default: return ESP_OK;
    }
}
static esp_err_t switch_mode_(app_mode_t mode) {
    if (mode<APP_MODE_IDLE || mode>APP_MODE_SAILOR) return ESP_ERR_INVALID_ARG;
    s_mode=detect_mode_();
    if (mode==s_mode) return ESP_OK;
    /* Suspend readers before workers/resources are stopped. */
    if (!lvgl_port_lock(-1)) return ESP_ERR_TIMEOUT;
    suspend_views_();
    if (!atomic_load(&s_web)) screen_ui_show();
    lvgl_port_unlock();
    esp_err_t err=stop_runtime_();
    if (err!=ESP_OK) {
        if (!atomic_load(&s_web) && lvgl_port_lock(-1)) { local_view_(); lvgl_port_unlock(); }
        return err;
    }
    if (lvgl_port_lock(-1)) { destroy_views_(); lvgl_port_unlock(); }
    s_mode=APP_MODE_IDLE;
    switch(mode) {
    case APP_MODE_RX485: if (!rs485_parser_runtime_start()) err=ESP_FAIL; break;
    case APP_MODE_TX485: if (!rs485_simui_runtime_start()) err=ESP_FAIL; break;
    case APP_MODE_RS485_BRIDGE: if (!rs485_bridge_runtime_start()) err=ESP_FAIL; break;
    case APP_MODE_N2K: err=nm2k_runtime_start(0); break;
    case APP_MODE_SAILOR: err=bridge_can_runtime_start(0); break;
    default: break;
    }
    s_mode=detect_mode_();
    if (s_mode==APP_MODE_SAILOR && atomic_load(&s_web)) bridge_can_set_web_terminal(true);
    if (!atomic_load(&s_web) && lvgl_port_lock(-1)) { local_view_(); lvgl_port_unlock(); }
    return err;
}
static void take_web_(void) {
    memset(s_cursors,0,sizeof(s_cursors));
    memset(s_stream_pending,0,sizeof(s_stream_pending));
    s_terminal_length=0; s_terminal_seq=0; s_terminal_sent=0;
    atomic_store(&s_web,true);
    s_mode=detect_mode_();
    if (lvgl_port_lock(-1)) {
        suspend_views_();
        s_top_was_hidden=lv_obj_has_flag(lv_layer_top(),LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv_layer_top(),LV_OBJ_FLAG_HIDDEN);
        screen_web_show(); lvgl_port_unlock();
    }
    if (s_mode==APP_MODE_SAILOR) bridge_can_set_web_terminal(true);
}
static void return_local_(const char *reason) {
    int peer=s_session.fd;
    s_terminal_length=0;
    if(s_session.state!=WEB_SESSION_LOCAL)web_session_revoke(&s_session);
    atomic_store(&s_epoch,s_session.generation); atomic_store(&s_peer,-1);
    if (peer>=0) {
        cJSON *j=cJSON_CreateObject();
        cJSON_AddStringToObject(j,"type","released"); cJSON_AddStringToObject(j,"reason",reason);
        (void)send_json_(peer,j); web_server_close(peer);
    }
    if (bridge_can_runtime_running()) bridge_can_set_web_terminal(false);
    s_mode=detect_mode_();
    if (lvgl_port_lock(-1)) {
        if (!s_top_was_hidden) lv_obj_clear_flag(lv_layer_top(),LV_OBJ_FLAG_HIDDEN);
        local_view_(); lvgl_port_unlock();
    }
    atomic_store(&s_web,false);
}
static void add_nav_(cJSON *root) {
    navigation_snapshot_t n; navigation_model_snapshot(&n);
    cJSON *j=cJSON_AddObjectToObject(root,"nav"); if(!j) return;
#define N(field) cJSON_AddNumberToObject(j,#field,n.field)
    N(lat); N(lon); N(gps_cog); N(gps_sog); N(gyro_heading); N(log_speed); N(depth);
    N(wind_angle); N(wind_speed); N(temp_c); N(valid_fields); N(stale_fields); N(revision);
#undef N
    char dir[2]={n.lat_dir,0}; cJSON_AddStringToObject(j,"lat_dir",dir);
    dir[0]=n.lon_dir; cJSON_AddStringToObject(j,"lon_dir",dir);
    cJSON_AddStringToObject(j,"time",n.time); cJSON_AddStringToObject(j,"date",n.date);
    cJSON_AddBoolToObject(j,"wind_true",n.wind_true);
}
static void state_(int peer) {
    cJSON *j=cJSON_CreateObject(); if(!j) return;
    s_mode=detect_mode_();
    cJSON_AddStringToObject(j,"type","state"); cJSON_AddStringToObject(j,"mode",mode_names[s_mode]);
    cJSON_AddNumberToObject(j,"baud",rs485_get_baudrate());
    cJSON_AddNumberToObject(j,"uptime_ms",(double)now_());
    cJSON_AddNumberToObject(j,"internal_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j,"internal_min",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j,"internal_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j,"psram_free",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(j,"version",nmea_version_get());
    cJSON_AddNumberToObject(j,"theme",ui_theme_get_id());
    cJSON_AddBoolToObject(j,"rotated",screen_orientation_is_rotated());
    char ip[20]=""; wifi_manager_get_ip_str(ip,sizeof(ip));
    cJSON_AddStringToObject(j,"ip",ip);
    cJSON_AddNumberToObject(j,"wifi_mode",wifi_manager_get_mode());
    add_nav_(j);
    cJSON *groups=cJSON_AddArrayToObject(j,"groups");
    for(int i=0;i<GRP_COUNT;++i) cJSON_AddItemToArray(groups,cJSON_CreateBool(rs485_engine_group_active(i)));
    if(s_mode==APP_MODE_RX485) {
        rs485_parser_status_t p; rs485_parser_runtime_status(&p);
        cJSON_AddBoolToObject(j,"hex",p.hex_mode); cJSON_AddBoolToObject(j,"paused",p.paused);
        cJSON_AddNumberToObject(j,"rx_bytes",p.bytes_received);
        cJSON_AddNumberToObject(j,"rx_lines",p.lines_received);
    }
    if(s_mode==APP_MODE_RS485_BRIDGE) {
        rs485_bridge_status_t p; rs485_bridge_runtime_status(&p);
        cJSON_AddNumberToObject(j,"rx_bytes",p.bytes_from_uart); cJSON_AddNumberToObject(j,"tx_bytes",p.bytes_to_uart);
    }
    if(s_mode==APP_MODE_SAILOR) {
        bridge_can_status_t p; bridge_can_runtime_status(&p);
        cJSON_AddBoolToObject(j,"terminal_ready",p.terminal_ready);
        cJSON_AddNumberToObject(j,"link_state",p.state);
        cJSON_AddNumberToObject(j,"terminal_dropped",p.terminal_dropped);
        cJSON *a=cJSON_AddObjectToObject(j,"antenna");
        if(a) {
            const protocol_antenna_status_t *v=&p.antenna;
            sailor_network_text_t network;
            sailor_status_format_network(v,&network);
            const char *keys[]={"ocean","registration","protocol","channel"};
            const char *fresh_keys[]={"ocean_fresh","registration_fresh","protocol_fresh","channel_fresh"};
            const sailor_status_value_t *values[]={&network.ocean,&network.registration,&network.protocol,&network.channel};
            for(size_t i=0;i<sizeof(values)/sizeof(*values);++i) {
                if(values[i]->known) cJSON_AddStringToObject(a,keys[i],values[i]->text);
                else cJSON_AddNullToObject(a,keys[i]);
                cJSON_AddBoolToObject(a,fresh_keys[i],values[i]->fresh);
            }
            const bool position_valid=v->position_valid && isfinite(v->latitude) &&
                isfinite(v->longitude) && fabs(v->latitude)<=90 && fabs(v->longitude)<=180;
            cJSON_AddBoolToObject(a,"online",v->online);
            cJSON_AddBoolToObject(a,"identity_valid",v->identity_valid);
            cJSON_AddBoolToObject(a,"signal_valid",v->signal_valid);
            cJSON_AddBoolToObject(a,"position_valid",position_valid);
            cJSON_AddBoolToObject(a,"position_fresh",position_valid && v->position_fresh);
            if(v->identity_valid) cJSON_AddStringToObject(a,"serial",v->serial);
            else cJSON_AddNullToObject(a,"serial");
            if(v->signal_valid) {
                cJSON_AddNumberToObject(a,"cn0_dbhz",v->cn0_dbhz);
                cJSON_AddNumberToObject(a,"signal_bars",v->signal_bars);
            } else {
                cJSON_AddNullToObject(a,"cn0_dbhz"); cJSON_AddNullToObject(a,"signal_bars");
            }
            if(position_valid) {
                cJSON_AddNumberToObject(a,"latitude",v->latitude);
                cJSON_AddNumberToObject(a,"longitude",v->longitude);
                cJSON_AddNumberToObject(a,"position_utc",v->position_utc);
            } else {
                cJSON_AddNullToObject(a,"latitude"); cJSON_AddNullToObject(a,"longitude");
                cJSON_AddNullToObject(a,"position_utc");
            }
            if(v->signal_age_ms!=UINT32_MAX)
                cJSON_AddNumberToObject(a,"signal_age_ms",v->signal_age_ms);
            else cJSON_AddNullToObject(a,"signal_age_ms");
            if(v->position_age_ms!=UINT32_MAX)
                cJSON_AddNumberToObject(a,"position_age_ms",v->position_age_ms);
            else cJSON_AddNullToObject(a,"position_age_ms");
        }
    }
    if(s_mode==APP_MODE_N2K) cJSON_AddNumberToObject(j,"can_speed",nm2k_runtime_speed());
    (void)send_json_(peer,j);
}
static void settings_(int peer) {
    cJSON *j=cJSON_CreateObject(); if(!j) return;
    char ssid[33],pass[65],ip[16];
    cJSON_AddStringToObject(j,"type","settings");
    wifi_manager_ap_get(ssid,sizeof(ssid),pass,sizeof(pass),ip,sizeof(ip));
    cJSON_AddStringToObject(j,"ap_ssid",ssid); cJSON_AddStringToObject(j,"ap_ip",ip);
    cJSON_AddBoolToObject(j,"ap_password_set",*pass!=0);
    wifi_manager_sta_get(ssid,sizeof(ssid),pass,sizeof(pass));
    cJSON_AddStringToObject(j,"sta_ssid",ssid);
    cJSON_AddBoolToObject(j,"sta_password_set",*pass!=0);
    cJSON_AddNumberToObject(j,"wifi_mode",wifi_manager_get_mode());
    (void)send_json_(peer,j);
}
static void ais_(int peer,unsigned offset) {
    aisdecoder_target_t *targets=ps_alloc_(AISDEC_MAX_TARGETS*sizeof(*targets));
    if(!targets) { ack_(peer,0,ESP_ERR_NO_MEM); return; }
    uint32_t updates; size_t count=aisdecoder_runtime_snapshot(targets,AISDEC_MAX_TARGETS,&updates);
    cJSON *j=cJSON_CreateObject(), *list=cJSON_CreateArray();
    if(!j||!list) { cJSON_Delete(j); cJSON_Delete(list); free(targets); return; }
    cJSON_AddStringToObject(j,"type","ais"); cJSON_AddNumberToObject(j,"offset",offset);
    cJSON_AddNumberToObject(j,"total",count); cJSON_AddNumberToObject(j,"updates",updates);
    cJSON_AddItemToObject(j,"targets",list);
    for(size_t i=offset;i<count && i<(size_t)offset+24;++i) {
        const aisdecoder_target_t *t=&targets[i];
        cJSON *item=cJSON_CreateObject(); if(!item) break;
        cJSON_AddNumberToObject(item,"mmsi",t->mmsi); cJSON_AddNumberToObject(item,"msg",t->msg_type);
        cJSON_AddNumberToObject(item,"lat",t->lat); cJSON_AddNumberToObject(item,"lon",t->lon);
        cJSON_AddNumberToObject(item,"sog",t->sog); cJSON_AddNumberToObject(item,"cog",t->cog);
        cJSON_AddNumberToObject(item,"heading",t->heading); cJSON_AddNumberToObject(item,"status",t->nav_status);
        cJSON_AddNumberToObject(item,"age_ms",(uint32_t)now_()-t->last_seen_ms);
        cJSON_AddStringToObject(item,"name",t->name); cJSON_AddStringToObject(item,"call",t->call);
        cJSON_AddItemToArray(list,item);
    }
    free(targets); (void)send_json_(peer,j);
}
static void n2k_(int peer) {
    nm2k_snapshot_t *s=ps_alloc_(sizeof(*s));
    if(!s) return;
    if(!nm2k_runtime_snapshot(s)) { free(s); return; }
    cJSON *j=cJSON_CreateObject(),*nodes=cJSON_CreateArray(),*traffic=cJSON_CreateArray();
    if(!j||!nodes||!traffic) { cJSON_Delete(j);cJSON_Delete(nodes);cJSON_Delete(traffic);free(s);return; }
    cJSON_AddStringToObject(j,"type","n2k"); cJSON_AddNumberToObject(j,"rx_count",s->rx_count);
    cJSON_AddNumberToObject(j,"first_seq",(double)s->first_seq);cJSON_AddNumberToObject(j,"next_seq",(double)s->next_seq);
    cJSON_AddNumberToObject(j,"dropped",s->traffic_dropped);
    cJSON_AddItemToObject(j,"nodes",nodes);cJSON_AddItemToObject(j,"traffic",traffic);
    for(size_t i=0;i<s->node_count;++i) {
        const nm2k_node_snapshot_t *n=&s->nodes[i]; cJSON *v=cJSON_CreateObject(); if(!v) break;
        char name[17]; snprintf(name,sizeof(name),"%016llX",(unsigned long long)n->name);
        cJSON_AddNumberToObject(v,"sa",n->sa);cJSON_AddStringToObject(v,"name",name);
        cJSON_AddStringToObject(v,"model",n->model);cJSON_AddStringToObject(v,"serial",n->serial);
        cJSON_AddNumberToObject(v,"pgn",n->last_pgn);cJSON_AddNumberToObject(v,"count",n->rx_count);
        cJSON_AddNumberToObject(v,"age_ms",(double)(now_()-n->last_seen_us/1000));
        cJSON_AddItemToArray(nodes,v);
    }
    for(size_t i=0;i<s->traffic_count;++i) cJSON_AddItemToArray(traffic,cJSON_CreateString(s->traffic[i]));
    free(s); (void)send_json_(peer,j);
}
static void hex_(char *dst,const uint8_t *src,size_t n) {
    const char *digits="0123456789ABCDEF";
    for(size_t i=0;i<n;++i) {dst[i*2]=digits[src[i]>>4];dst[i*2+1]=digits[src[i]&15];} dst[n*2]=0;
}
static size_t unhex_(const char *text,uint8_t *out,size_t cap) {
    size_t len=strlen(text); if(!len || len%2 || len>cap*2) return 0;
    for(size_t i=0;i<len/2;++i) {
        unsigned value=0;
        for(size_t k=0;k<2;++k) {
            char c=text[i*2+k]; int v=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
            if(v<0)return 0;
            value=(value<<4)|(unsigned)v;
        }
        out[i]=(uint8_t)value;
    }
    return len/2;
}
static void stream_(int peer) {
    typedef bool (*reader_t)(uint32_t *,rs485_runtime_event_t *,uint32_t *);
    reader_t read=NULL; int index=0;
    if(s_mode==APP_MODE_RX485) read=rs485_parser_runtime_read;
    if(s_mode==APP_MODE_TX485){read=rs485_simui_runtime_read;index=1;}
    if(s_mode==APP_MODE_RS485_BRIDGE){read=rs485_bridge_runtime_read;index=2;}
    if(!read)return;
    uint32_t stamp=(uint32_t)now_();
    if(s_stream_pending[index]&&stamp-s_stream_sent[index]<1000)return;
    uint32_t cursor=s_cursors[index],dropped=0;
    cJSON *j=cJSON_CreateObject(),*list=cJSON_CreateArray();if(!j||!list){cJSON_Delete(j);cJSON_Delete(list);return;}
    cJSON_AddStringToObject(j,"type","events");cJSON_AddStringToObject(j,"mode",mode_names[s_mode]);
    cJSON_AddItemToObject(j,"items",list);
    unsigned count=0;
    for(;count<12;++count) {
        rs485_runtime_event_t e;uint32_t gap=0;
        if(!read(&cursor,&e,&gap))break;
        dropped+=gap; cJSON *v=cJSON_CreateObject();if(!v)break;
        cJSON_AddNumberToObject(v,"seq",e.sequence);cJSON_AddNumberToObject(v,"ms",e.time_ms);cJSON_AddNumberToObject(v,"kind",e.kind);
        if(e.kind==RS485_EVENT_BRIDGE_RX||e.kind==RS485_EVENT_BRIDGE_TX) {
            char bytes[sizeof(e.data)*2+1];hex_(bytes,(uint8_t *)e.data,e.length);
            cJSON_AddStringToObject(v,"hex",bytes);
        } else cJSON_AddStringToObject(v,"text",e.data);
        cJSON_AddItemToArray(list,v);
    }
    cJSON_AddNumberToObject(j,"dropped",dropped);
    cJSON_AddNumberToObject(j,"cursor",cursor);
    if(!count){cJSON_Delete(j);return;}
    if(send_json_(peer,j)){s_pending_cursors[index]=cursor;s_stream_pending[index]=true;s_stream_sent[index]=stamp;}
}
static void terminal_(int peer) {
    if(s_mode!=APP_MODE_SAILOR){s_terminal_length=0;return;}
    if(!s_terminal_length){
        s_terminal_length=bridge_can_terminal_drain(s_terminal_pending,sizeof(s_terminal_pending));
        if(s_terminal_length){++s_terminal_seq;s_terminal_sent=0;}
    }
    uint32_t stamp=(uint32_t)now_();
    if(s_terminal_sent&&stamp-s_terminal_sent<1000)return;
    if(!s_terminal_length)return;
    char bytes[513];hex_(bytes,s_terminal_pending,s_terminal_length);
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"type","terminal");cJSON_AddStringToObject(j,"hex",bytes);
    cJSON_AddNumberToObject(j,"seq",s_terminal_seq);
    if(send_json_(peer,j))s_terminal_sent=stamp;
}
typedef struct {int peer;uint32_t epoch,id;} scan_request_t;
static void scan_task_(void *arg) {
    scan_request_t request=*(scan_request_t *)arg;free(arg);
    wifi_ap_record_t *list=ps_alloc_(20*sizeof(*list));
    cJSON *j=cJSON_CreateObject(),*items=cJSON_CreateArray();
    if(j&&items) {
        cJSON_AddStringToObject(j,"type","scan");cJSON_AddNumberToObject(j,"id",request.id);cJSON_AddItemToObject(j,"items",items);
        unsigned count=list?wifi_manager_scan(list,20):0;
        for(unsigned i=0;i<count;++i) {
            cJSON *v=cJSON_CreateObject();cJSON_AddStringToObject(v,"ssid",(char*)list[i].ssid);
            cJSON_AddNumberToObject(v,"rssi",list[i].rssi);cJSON_AddBoolToObject(v,"secure",list[i].authmode!=WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(items,v);
        }
        char *text=cJSON_PrintUnformatted(j);
        if(text) {
            command_t c={.kind=CMD_SCAN_RESULT,.peer=request.peer,.epoch=request.epoch,.text=text};
            if(xQueueSend(s_commands,&c,0)!=pdTRUE)cJSON_free(text);
        }
    } else cJSON_Delete(items);
    cJSON_Delete(j);free(list);atomic_store(&s_scan_busy,false);vTaskDelete(NULL);
}
static bool valid_baud_(uint32_t value) {
    return value==2400||value==4800||value==9600||value==19200||value==38400||value==115200;
}
static esp_err_t set_baud_(uint32_t baud) {
    if(!valid_baud_(baud))return ESP_ERR_INVALID_ARG;
    switch(s_mode) {
    case APP_MODE_RX485:return rs485_parser_runtime_set_baud(baud);
    case APP_MODE_TX485:return rs485_simui_runtime_set_baud(baud);
    case APP_MODE_RS485_BRIDGE:return rs485_bridge_runtime_set_baud(baud);
    case APP_MODE_SAILOR:return protocol_handler_set_rs_baudrate(baud);
    default:return ESP_ERR_INVALID_STATE;
    }
}
static esp_err_t apply_(int peer,uint32_t id,const cJSON *j) {
    const char *op=str_(j,"op");double v;bool enabled;
    if(!strcmp(op,"mode")) {
        const char *name=str_(j,"mode");
        for(int i=0;i<=APP_MODE_SAILOR;++i)if(!strcmp(name,mode_names[i]))return switch_mode_(i);
        return ESP_ERR_INVALID_ARG;
    }
    if(!strcmp(op,"baud"))return number_(j,"value",2400,115200,&v)&&floor(v)==v?set_baud_((uint32_t)v):ESP_ERR_INVALID_ARG;
    if(!strcmp(op,"can_speed")) {
        if(s_mode!=APP_MODE_N2K)return ESP_ERR_INVALID_STATE;
        if(!number_(j,"value",125000,500000,&v)||(v!=125000&&v!=250000&&v!=500000))return ESP_ERR_INVALID_ARG;
        if(!nm2k_runtime_stop())return ESP_ERR_TIMEOUT;
        return nm2k_runtime_start((uint32_t)v);
    }
    if(!strcmp(op,"hex")||!strcmp(op,"pause")) {
        if(s_mode!=APP_MODE_RX485)return ESP_ERR_INVALID_STATE;
        if(!boolean_(j,"value",&enabled))return ESP_ERR_INVALID_ARG;
        if(!strcmp(op,"hex"))rs485_parser_runtime_set_hex(enabled);else rs485_parser_runtime_set_paused(enabled);
        return ESP_OK;
    }
    if(!strcmp(op,"refresh")){navigation_model_refresh_templates();return nmea_templates_save_now();}
    if(!strcmp(op,"group")) {
        if(s_mode!=APP_MODE_TX485)return ESP_ERR_INVALID_STATE;
        int group=web_templates_group(str_(j,"group"));
        if(group<0||!boolean_(j,"value",&enabled))return ESP_ERR_INVALID_ARG;
        rs485_engine_set_active(group,enabled);return ESP_OK;
    }
    if(!strcmp(op,"send")) {
        if(s_mode!=APP_MODE_TX485)return ESP_ERR_INVALID_STATE;
        const char *text=str_(j,"text");return *text?rs485_simui_runtime_send(text):ESP_ERR_INVALID_ARG;
    }
    if(!strcmp(op,"terminal")||!strcmp(op,"bridge_write")) {
        uint8_t bytes[512];size_t n=unhex_(str_(j,"hex"),bytes,sizeof(bytes));
        if(!n||(!strcmp(op,"bridge_write")&&n>256))return ESP_ERR_INVALID_ARG;
        if(!strcmp(op,"terminal"))return s_mode==APP_MODE_SAILOR?bridge_can_terminal_write(bytes,n):ESP_ERR_INVALID_STATE;
        return s_mode==APP_MODE_RS485_BRIDGE?rs485_bridge_runtime_write(bytes,n):ESP_ERR_INVALID_STATE;
    }
    if(!strcmp(op,"template_get")) {
        int group=web_templates_group(str_(j,"group"));if(group<0)return ESP_ERR_INVALID_ARG;
        return send_json_(peer,web_templates_json(group))?ESP_OK:ESP_ERR_NO_MEM;
    }
    if(!strcmp(op,"template_patch")) {
        int group=web_templates_group(str_(j,"group"));
        esp_err_t err=web_templates_apply(group,cJSON_GetObjectItemCaseSensitive(j,"values"));
        if(err==ESP_OK)(void)send_json_(peer,web_templates_json(group));
        return err;
    }
    if(!strcmp(op,"save"))return nmea_templates_save_now();
    if(!strcmp(op,"ais_get")){if(!number_(j,"offset",0,255,&v))v=0;ais_(peer,(unsigned)v);return ESP_OK;}
    if(!strcmp(op,"n2k_get")){n2k_(peer);return ESP_OK;}
    if(!strcmp(op,"settings_get")){settings_(peer);return ESP_OK;}
    if(!strcmp(op,"settings")) {
        if(cJSON_HasObjectItem(j,"version")&&(!number_(j,"version",0,NMEA_VERSION_COUNT-1,&v)||floor(v)!=v))return ESP_ERR_INVALID_ARG;
        if(cJSON_HasObjectItem(j,"theme")&&(!number_(j,"theme",0,UI_THEME_COUNT-1,&v)||floor(v)!=v))return ESP_ERR_INVALID_ARG;
        if(cJSON_HasObjectItem(j,"rotated")&&!boolean_(j,"rotated",&enabled))return ESP_ERR_INVALID_ARG;
        esp_err_t err=ESP_OK;
        if(cJSON_HasObjectItem(j,"version")) {
            if(!number_(j,"version",0,NMEA_VERSION_COUNT-1,&v)||floor(v)!=v)return ESP_ERR_INVALID_ARG;
            err=nmea_version_set((nmea_version_t)v,true);
        }
        if(err!=ESP_OK)return err;
        if(!lvgl_port_lock(-1))return ESP_ERR_TIMEOUT;
        if(cJSON_HasObjectItem(j,"theme")) {
            if(number_(j,"theme",0,UI_THEME_COUNT-1,&v)&&floor(v)==v)err=ui_theme_set((ui_theme_id_t)v,true);else err=ESP_ERR_INVALID_ARG;
        }
        if(err==ESP_OK&&cJSON_HasObjectItem(j,"rotated")) {
            if(boolean_(j,"rotated",&enabled))err=screen_orientation_set_rotated(enabled,true);else err=ESP_ERR_INVALID_ARG;
        }
        lvgl_port_unlock();return err;
    }
    if(!strcmp(op,"wifi_scan")) {
        if(atomic_exchange(&s_scan_busy,true))return ESP_ERR_INVALID_STATE;
        scan_request_t *request=malloc(sizeof(*request));
        if(!request){atomic_store(&s_scan_busy,false);return ESP_ERR_NO_MEM;}
        *request=(scan_request_t){peer,s_session.generation,id};
        if(xTaskCreate(scan_task_,"web_scan",4096,request,3,NULL)!=pdPASS){free(request);atomic_store(&s_scan_busy,false);return ESP_ERR_NO_MEM;}
        return ESP_OK;
    }
    if(!strncmp(op,"wifi_",5)) {
        if(atomic_load(&s_scan_busy))return ESP_ERR_INVALID_STATE;
        /* Notify before a command deliberately changes its own network. */
        cJSON *notice=cJSON_CreateObject();cJSON_AddStringToObject(notice,"type","network_change");
        cJSON_AddNumberToObject(notice,"id",id);(void)send_json_(peer,notice);
        vTaskDelay(pdMS_TO_TICKS(100));
        if(atomic_load(&s_return_requested))return ESP_ERR_INVALID_STATE;
        if(!strcmp(op,"wifi_mode")) {
            if(!number_(j,"value",0,1,&v)||floor(v)!=v)return ESP_ERR_INVALID_ARG;
            return wifi_manager_set_mode((wifi_mgr_mode_t)v);
        }
        if(!strcmp(op,"wifi_ap")) {
            const cJSON *password=cJSON_GetObjectItemCaseSensitive(j,"password");
            char old_ssid[33],old_pass[65],old_ip[16];
            wifi_manager_ap_get(old_ssid,sizeof(old_ssid),old_pass,sizeof(old_pass),old_ip,sizeof(old_ip));
            return wifi_manager_ap_set(str_(j,"ssid"),cJSON_IsString(password)?password->valuestring:old_pass,str_(j,"ip"));
        }
        if(!strcmp(op,"wifi_sta")) {
            const cJSON *password=cJSON_GetObjectItemCaseSensitive(j,"password");
            char old_ssid[33],old_pass[65];
            wifi_manager_sta_get(old_ssid,sizeof(old_ssid),old_pass,sizeof(old_pass));
            esp_err_t err=wifi_manager_sta_set(str_(j,"ssid"),cJSON_IsString(password)?password->valuestring:old_pass);
            if(err==ESP_OK)err=wifi_manager_set_mode(WIFI_MGR_MODE_STA);
            return err;
        }
        if(!strcmp(op,"wifi_connect"))return wifi_manager_get_mode()==WIFI_MGR_MODE_STA?wifi_manager_sta_connect():wifi_manager_set_mode(WIFI_MGR_MODE_STA);
        if(!strcmp(op,"wifi_disconnect"))return wifi_manager_sta_disconnect();
        if(!strcmp(op,"wifi_off"))return wifi_manager_deinit();
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
static uint32_t hash_(const char *text) {uint32_t h=2166136261u;while(*text)h=(h^(uint8_t)*text++)*16777619u;return h;}
static void remote_(const command_t *command) {
    cJSON *j=cJSON_ParseWithOpts(command->text,NULL,true);if(!j||!cJSON_IsObject(j)){cJSON_Delete(j);ack_(command->peer,0,ESP_ERR_INVALID_ARG);return;}
    const char *op=str_(j,"op"),*token=str_(j,"token"); double raw_id;
    uint32_t id=number_(j,"id",0,2147483647,&raw_id)&&floor(raw_id)==raw_id?(uint32_t)raw_id:0;
    if(!strcmp(op,"claim")) {
        bool was_local=s_session.state==WEB_SESSION_LOCAL;
        char fresh[33];uint8_t random[16];esp_fill_random(random,sizeof(random));hex_(fresh,random,sizeof(random));
        if(!web_server_peer_alive(command->peer)||command->epoch!=atomic_load(&s_epoch)||atomic_load(&s_return_requested)||!web_session_claim_queued(&s_session,command->peer,token,fresh,command->epoch,command->local_at_submit,now_())) {
            ack_(command->peer,id,ESP_ERR_INVALID_STATE);cJSON_Delete(j);return;
        }
        memset(s_stream_sent,0,sizeof(s_stream_sent));s_terminal_sent=0;
        atomic_store(&s_epoch,s_session.generation);atomic_store(&s_peer,command->peer);
        /* Publishing peer before the second liveness check closes the race
         * with free_peer: any later disconnect is now recorded by ID. */
        if(!web_server_peer_alive(command->peer)) {
            if(was_local){web_session_revoke(&s_session);atomic_store(&s_epoch,s_session.generation);atomic_store(&s_peer,-1);}
            else web_session_disconnect(&s_session,command->peer);
            cJSON_Delete(j);return;
        }
        if(was_local){s_highest_id=0;s_reply_pos=0;memset(s_replies,0,sizeof(s_replies));take_web_();}
        cJSON *reply=cJSON_CreateObject();cJSON_AddStringToObject(reply,"type","claimed");
        cJSON_AddStringToObject(reply,"token",s_session.token);cJSON_AddNumberToObject(reply,"timeout_ms",WEB_SESSION_TIMEOUT_MS);
        cJSON_AddNumberToObject(reply,"next_id",s_highest_id+1);
        if(!send_json_(command->peer,reply)) {
            if(was_local)return_local_("connection_failed");
            else {web_session_disconnect(&s_session,command->peer);web_server_close(command->peer);}
            cJSON_Delete(j);return;
        }
        state_(command->peer);cJSON_Delete(j);return;
    }
    if(atomic_load(&s_return_requested)||command->epoch!=atomic_load(&s_epoch)||
        !web_session_authorized(&s_session,command->peer,token,command->epoch)) {
        ack_(command->peer,id,ESP_ERR_INVALID_STATE);cJSON_Delete(j);return;
    }
    if(!strcmp(op,"heartbeat")) {
        web_session_heartbeat(&s_session,now_());ack_(command->peer,0,ESP_OK);cJSON_Delete(j);return;
    }
    if(!strcmp(op,"events_ack")) {
        int index=-1; const char *mode=str_(j,"mode"); double cursor;
        if(!strcmp(mode,"rx485"))index=0;else if(!strcmp(mode,"tx485"))index=1;else if(!strcmp(mode,"rs485_bridge"))index=2;
        if(index>=0&&number_(j,"cursor",0,UINT32_MAX,&cursor)&&floor(cursor)==cursor&&
           s_stream_pending[index]&&s_pending_cursors[index]==(uint32_t)cursor){
            s_cursors[index]=(uint32_t)cursor;s_stream_pending[index]=false;
        }
        cJSON_Delete(j);return;
    }
    if(!strcmp(op,"terminal_ack")) {
        double seq;
        if(number_(j,"seq",0,UINT32_MAX,&seq)&&floor(seq)==seq&&(uint32_t)seq==s_terminal_seq)s_terminal_length=0;
        cJSON_Delete(j);return;
    }
    if(!strcmp(op,"release")) {cJSON_Delete(j);return_local_("released");return;}
    if(!id){ack_(command->peer,id,ESP_ERR_INVALID_ARG);cJSON_Delete(j);return;}
    uint32_t hash=hash_(command->text);
    if(id<=s_highest_id) {
        esp_err_t result=ESP_ERR_INVALID_STATE;
        for(size_t i=0;i<RESPONSE_CACHE;++i)if(s_replies[i].id==id&&s_replies[i].hash==hash){result=s_replies[i].result;break;}
        ack_(command->peer,id,result);cJSON_Delete(j);return;
    }
    s_highest_id=id;
    esp_err_t result=apply_(command->peer,id,j);
    s_replies[s_reply_pos++%RESPONSE_CACHE]=(reply_t){id,hash,result};
    ack_(command->peer,id,result);cJSON_Delete(j);s_last_state=0;
}
static void controller_task_(void *arg) {
    (void)arg;
    while(true) {
        if(atomic_exchange(&s_return_requested,false)&&atomic_load(&s_web))return_local_("local_button");
        int lost_peer=atomic_exchange(&s_lost_peer,-1);
        if(lost_peer>=0)web_session_disconnect(&s_session,lost_peer);
        /* Tick expires ownership, while return_local also restores presentation. */
        int expired_peer=s_session.fd;
        if(web_session_tick(&s_session,now_())) {
            if(expired_peer>=0)web_server_close(expired_peer);
            return_local_("timeout");
        } else if(s_session.state==WEB_SESSION_RECONNECT&&s_session.fd>=0) {
            int peer=s_session.fd;web_session_disconnect(&s_session,peer);web_server_close(peer);
        }
        command_t command;
        if(xQueueReceive(s_commands,&command,pdMS_TO_TICKS(50))==pdTRUE) {
            if(command.kind==CMD_REMOTE)remote_(&command);
            else if(command.kind==CMD_SCAN_RESULT) {
                if(command.epoch==s_session.generation&&command.peer==s_session.fd)
                    (void)web_server_send(command.peer,command.text,strlen(command.text));
            } else if(!atomic_load(&s_web)&&command.epoch==atomic_load(&s_epoch)) {
                esp_err_t err=ESP_OK;
                s_mode=detect_mode_();
                if(command.kind==CMD_LOCAL_MODE)err=switch_mode_((app_mode_t)command.value);
                else if(command.kind==CMD_LOCAL_N2K_SPEED) {
                    if(s_mode!=APP_MODE_N2K)err=ESP_ERR_INVALID_STATE;
                    else if(!nm2k_runtime_stop())err=ESP_ERR_TIMEOUT;
                    else err=nm2k_runtime_start(command.value);
                } else err=set_baud_(command.value);
                if(err!=ESP_OK)ESP_LOGE(TAG,"Local command failed: %s",esp_err_to_name(err));
            }
            free(command.text);
        }
        if(s_session.state==WEB_SESSION_ACTIVE&&!atomic_load(&s_return_requested)) {
            uint32_t now=(uint32_t)now_();
            if(!s_last_state||now-s_last_state>=200){state_(s_session.fd);s_last_state=now;}
            stream_(s_session.fd);terminal_(s_session.fd);
        }
    }
}
static bool enqueue_local_(command_kind_t kind,uint32_t value) {
    if(!s_commands||atomic_load(&s_web))return false;
    command_t command={.kind=kind,.value=value,.epoch=atomic_load(&s_epoch)};
    return xQueueSend(s_commands,&command,0)==pdTRUE;
}
bool app_controller_local_start(app_mode_t mode){return enqueue_local_(CMD_LOCAL_MODE,mode);}
bool app_controller_local_stop(void){return app_controller_local_start(APP_MODE_IDLE);}
bool app_controller_local_baud(uint32_t baud){return enqueue_local_(CMD_LOCAL_BAUD,baud);}
bool app_controller_local_n2k_speed(uint32_t speed){return enqueue_local_(CMD_LOCAL_N2K_SPEED,speed);}
bool app_controller_local_sailor_baud(uint32_t baud){return enqueue_local_(CMD_LOCAL_SAILOR_BAUD,baud);}
bool app_controller_is_web(void){return atomic_load(&s_web);}
void app_controller_return_local(void) {
    atomic_store(&s_return_requested,true);
    atomic_fetch_add(&s_epoch,1);
}
bool app_controller_web_submit(int peer,const char *json,size_t len) {
    if(!s_commands||!json||!len||len>4096)return false;
    /* Bound nesting before cJSON recursion, accounting for escaped strings. */
    int depth=0;bool quoted=false,escaped=false;
    for(size_t i=0;i<len;++i){char c=json[i];if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;else if(c=='{'||c=='['){if(++depth>8)return false;}else if(c=='}'||c==']')--depth;}
    command_t command={.kind=CMD_REMOTE,.peer=peer,.epoch=atomic_load(&s_epoch),
        .local_at_submit=!atomic_load(&s_web)&&!atomic_load(&s_return_requested)};
    command.text=ps_alloc_(len+1);if(!command.text)return false;
    memcpy(command.text,json,len);command.text[len]=0;
    if(xQueueSend(s_commands,&command,0)!=pdTRUE){free(command.text);return false;}
    return true;
}
void app_controller_web_disconnected(int peer){if(peer==atomic_load(&s_peer))atomic_store(&s_lost_peer,peer);}
esp_err_t app_controller_init(void) {
    if(s_commands)return ESP_OK;
    web_session_init(&s_session);atomic_store(&s_epoch,s_session.generation);atomic_store(&s_peer,-1);atomic_store(&s_lost_peer,-1);
    cJSON_Hooks hooks={.malloc_fn=ps_alloc_,.free_fn=free};cJSON_InitHooks(&hooks);
    s_commands=xQueueCreate(COMMANDS,sizeof(command_t));if(!s_commands)return ESP_ERR_NO_MEM;
    if(xTaskCreate(controller_task_,"app_control",12288,NULL,3,NULL)!=pdPASS) {
        vQueueDelete(s_commands);s_commands=NULL;return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

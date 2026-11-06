import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMON = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do {int e=(expr); if(e) return e;} while(0)
#define ESP_ERROR_CHECK(expr) assert((expr)==0)
"""


def source_between(path, start, end):
    source = (ROOT / path).read_text()
    return start + source.split(start, 1)[1].split(end, 1)[0]


class RuntimeTests(unittest.TestCase):
    def run_c(self, source, headers=None, extra_sources=()):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for name, content in (headers or {}).items():
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            binary = directory / "probe"
            command = [
                os.environ.get("CC", "cc"), "-std=c11", "-pthread",
                "-D_DEFAULT_SOURCE", "-I", str(directory), "-I", str(ROOT / "main"),
                "-x", "c", "-", *map(str, extra_sources), "-o", str(binary),
            ]
            result = subprocess.run(command, input=source, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([binary], text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_websocket_continuations_and_interleaved_ping(self):
        body = (ROOT / "main/zello_client.c").read_text().split(
            "    case WEBSOCKET_EVENT_DATA: {", 1
        )[1].split("\n    default:", 1)[0]
        self.run_c(COMMON + r"""
#define MESSAGE_MAX 4096
typedef struct {
    int op_code, data_len, payload_len, payload_offset;
    bool fin;
    const char *data_ptr;
} event_t;
static uint8_t assembly[MESSAGE_MAX], *s_asm=assembly;
static size_t s_asm_len;
static int s_asm_op, calls;
static bool s_asm_dropping;
static char received[MESSAGE_MAX+1];
static uint8_t binary_received[4];
static int binary_calls;
static void handle_text(const char *text, size_t len) {
    memcpy(received,text,len); received[len]=0; calls++;
}
static void handle_binary(const uint8_t *data, size_t len) {
    assert(len==4);memcpy(binary_received,data,len);binary_calls++;
}
static void deliver(event_t *d) {switch(1) {case 1: {
""" + body + r"""
}}
int main(void) {
    const char *message="{\"command\":\"on_channel_status\",\"status\":\"online\"}";
    int length=(int)strlen(message), split=20;
    event_t first={1,split,length,0,true,message};
    event_t second={1,length-split,length,split,true,message+split};
    deliver(&first); assert(calls==0);
    deliver(&second); assert(calls==1 && !strcmp(received,message));
    calls=0; received[0]=0;
    first.payload_len=split; first.fin=false;
    second.op_code=0; second.payload_len=length-split; second.payload_offset=0;
    deliver(&first); assert(calls==0);
    event_t ping={9,0,0,0,true,""};
    deliver(&ping);
    deliver(&second); assert(calls==1 && !strcmp(received,message));
    calls=0;
    event_t partial={0,5,length-split,0,true,message+split};
    event_t tail={0,length-split-5,length-split,5,true,message+split+5};
    deliver(&first);deliver(&partial);assert(calls==0);
    deliver(&tail);assert(calls==1 && !strcmp(received,message));
    const uint8_t voice[]={0x01,0x00,0x02,0xff};
    event_t voice_start={2,2,2,0,false,(const char *)voice};
    event_t voice_end={0,2,2,0,true,(const char *)voice+2};
    deliver(&voice_start);assert(binary_calls==0);
    deliver(&voice_end);assert(binary_calls==1 && !memcmp(binary_received,voice,4));
    calls=0;
    char oversized[MESSAGE_MAX+1]; memset(oversized,'x',sizeof(oversized));
    event_t large={1,sizeof(oversized),sizeof(oversized),0,false,oversized};
    deliver(&large);
    event_t end={0,0,0,0,true,""};
    deliver(&end); assert(calls==0);
    event_t good={1,length,length,0,true,message};
    deliver(&good); assert(calls==1 && !strcmp(received,message));
}
""")

    def test_full_length_wifi_credentials_and_overflow_rejection(self):
        connect = source_between(
            "main/net_wifi.c", "esp_err_t net_wifi_connect", "\nbool net_wifi_wait_ip"
        )
        self.run_c(COMMON + r"""
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_AUTH_OPEN 0
#define WIFI_IF_STA 0
typedef struct {
    struct {
        uint8_t ssid[32],password[64];
        struct {int authmode;} threshold;
        struct {bool capable;} pmf_cfg;
    } sta;
} wifi_config_t;
static bool s_want_connect;
static wifi_config_t captured;
static int writes;
static int esp_wifi_disconnect(void) {return 0;}
static int esp_wifi_connect(void) {return 0;}
static int esp_wifi_set_config(int mode, wifi_config_t *config) {
    (void)mode; captured=*config; writes++; return 0;
}
""" + connect + r"""
int main(void) {
    const char ssid[]="12345678901234567890123456789012";
    const char psk[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(net_wifi_connect(ssid,psk)==ESP_OK);
    assert(!memcmp(captured.sta.ssid,ssid,32));
    assert(!memcmp(captured.sta.password,psk,64));
    assert(net_wifi_connect("123456789012345678901234567890123","")==ESP_ERR_INVALID_ARG);
    assert(writes==1);
    assert(net_wifi_connect(" Home ","")==ESP_OK);
    assert(!strcmp((char *)captured.sta.ssid," Home "));
}
""")

    def test_short_received_audio_does_not_wait_for_another_transmission(self):
        play = source_between("main/audio.c", "static void play_task", "\nstatic void mic_task")
        self.run_c(COMMON + r"""
#define AUDIO_FRAME 960
#define AUDIO_FRAME_MS 60
#define AUDIO_STEREO_IO 0
#define FRAME_BYTES (AUDIO_FRAME*sizeof(int16_t))
#define IO_BYTES FRAME_BYTES
#define PLAY_START_BYTES (2*FRAME_BYTES)
#define PLAY_START_TIMEOUT_MS 120
#define pdMS_TO_TICKS(n) (n)
typedef unsigned TickType_t;
static int s_play;
static bool s_playing;
static uint8_t s_play_level;
static size_t available,drained;
static int writes;
static TickType_t now;
static jmp_buf done;
static TickType_t xTaskGetTickCount(void) {return now;}
static void vTaskDelete(void *arg) {(void)arg;abort();}
static size_t xStreamBufferBytesAvailable(int stream) {(void)stream;return available;}
static size_t xStreamBufferReceive(int stream,void *out,size_t cap,int wait) {
    (void)stream;(void)wait;
    size_t n=available<cap?available:cap;
    memset(out,1,n); available-=n;drained+=n;return n;
}
static uint8_t peak_percent(const int16_t *pcm,size_t n) {(void)pcm;(void)n;return 1;}
static void out_write(int16_t *frame,int16_t *io) {
    (void)frame;(void)io;now+=60;
    if(++writes==5) longjmp(done,1);
}
""" + play + r"""
int main(void) {
    available=FRAME_BYTES;
    if(setjmp(done)==0) play_task(NULL);
    assert(drained==FRAME_BYTES && available==0);
    available=2*FRAME_BYTES; writes=0;drained=0;now=0;s_playing=false;
    if(setjmp(done)==0) play_task(NULL);
    assert(drained==2*FRAME_BYTES);
}
""")

    def test_late_wifi_still_starts_automatic_login(self):
        app = (ROOT / "main/app_main.c").read_text().split('static const char *TAG = "main";', 1)[1]
        self.run_c(COMMON + r"""
static struct {bool auto_connect;char wifi_ssid[33],wifi_pass[65],username[33];int volume,mic_gain;} g_settings;
static bool console_ready;
static int logins;
static unsigned waited_ms;
static bool net_wifi_wait_ip(unsigned ms) {
    assert(console_ready);waited_ms+=ms;return waited_ms>=25000;
}
static bool net_wifi_wait_time(unsigned ms) {(void)ms;return true;}
static int zello_auto_connect(void) {logins++;return ESP_OK;}
#define crash_report_init() ((void)0)
#define settings_load() 0
#define board_init() 0
#define display_init() 0
#define channel_list_init() 0
#define audio_init(cb) 0
#define audio_set_volume(v) ((void)0)
#define audio_set_mic_gain(v) ((void)0)
#define zello_init() 0
#define net_wifi_init() 0
#define crash_report_start() ((void)0)
#define net_wifi_connect(a,b) 0
#define web_config_start() ((void)0)
#define ptt_button_start() ((void)0)
#define ui_start() ((void)0)
#define display_reveal() ((void)0)
#define console_start() (console_ready=true)
""" + app + r"""
int main(void) {
    g_settings.auto_connect=true;strcpy(g_settings.wifi_ssid,"Home");
    app_main();
    assert(logins==1 && waited_ms>=25000);
}
""")

    def test_failed_crash_file_close_preserves_flash_dump(self):
        tail = source_between(
            "main/crash_report.c", "    size_t addr = 0, size = 0;", "\n#endif"
        )
        self.run_c(COMMON + r"""
#define CRASH_DIR "/sd/crash"
#define ESP_PARTITION_TYPE_DATA 0
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 0
typedef struct {size_t address;} esp_partition_t;
static esp_partition_t part_value={4096};
static int erased;
static const esp_partition_t *esp_partition_find_first(int a,int b,const char*c) {
    (void)a;(void)b;(void)c;return &part_value;
}
static int esp_core_dump_image_get(size_t *addr,size_t *size) {*addr=4096;*size=64;return ESP_OK;}
static int esp_partition_read(const esp_partition_t*p,size_t off,void*out,size_t n) {
    (void)p;(void)off;memset(out,0,n);return ESP_OK;
}
static int esp_core_dump_image_erase(void) {erased++;return ESP_OK;}
static FILE *output;
static FILE *probe_open(const char *path,const char *mode) {
    (void)path;(void)mode;output=tmpfile();return output;
}
static int probe_close(FILE *f) {int result=fclose(f);return f==output?EOF:result;}
#define fopen probe_open
#define fclose probe_close
static void write_coredump(FILE *f,int idx) {
""" + tail + r"""
int main(void) {
    FILE *report=tmpfile();
    write_coredump(report,1);
    assert(erased==0);
}
""")

    def test_automatic_login_cannot_override_a_manual_connection_action(self):
        lifecycle = source_between(
            "main/zello_client.c", "void zello_disconnect(void)", "\nstatic void reconfigure_task"
        )
        self.run_c(COMMON + r"""
#define portMAX_DELAY 0
#define WS_BUFFER_SIZE 2048
#define WS_TASK_STACK 8192
#define WEBSOCKET_EVENT_ANY 0
#define ZELLO_OFFLINE 0
#define ZELLO_FAILED 1
#define ZELLO_CONNECTING 2
static struct {bool auto_connect;} g_settings;
static bool s_auto_connect_allowed=true, s_want_connect;
static int s_conn_lock, connections;
static void *s_ws;
typedef struct {
    const char *uri;
    int buffer_size,task_stack,task_prio,reconnect_timeout_ms,network_timeout_ms,ping_interval_sec;
    bool keep_alive_enable,enable_close_reconnect;
    void *crt_bundle_attach;
} esp_websocket_client_config_t;
static void *esp_crt_bundle_attach;
#define xSemaphoreTake(a,b) ((void)0)
#define xSemaphoreGive(a) ((void)0)
static void disconnect_locked(void) {s_ws=NULL;}
static void set_state(int state,const char *why) {(void)state;(void)why;}
static bool account_ready(const char **why) {(void)why;return true;}
static void settings_ws_url(char *out,size_t cap) {snprintf(out,cap,"wss://example.test");}
static void *esp_websocket_client_init(const esp_websocket_client_config_t *c) {
    assert(!strcmp(c->uri,"wss://example.test"));connections++;return (void *)1;
}
static void on_ws_event(void) {}
static int esp_websocket_register_events(void*w,int e,void (*cb)(void),void*a) {return 0;}
static int esp_websocket_client_start(void*w) {return 0;}
#define log_add(...) ((void)0)
""" + lifecycle + r"""
int main(void) {
    g_settings.auto_connect=true;
    zello_disconnect();
    assert(zello_auto_connect()==ESP_OK && connections==0);
    s_auto_connect_allowed=true;
    assert(zello_connect()==ESP_OK && connections==1);
    assert(zello_auto_connect()==ESP_OK && connections==1);
    s_auto_connect_allowed=true;
    g_settings.auto_connect=false;
    assert(zello_auto_connect()==ESP_OK && connections==1);
    g_settings.auto_connect=true;
    assert(zello_auto_connect()==ESP_OK && connections==2);
    assert(zello_auto_connect()==ESP_OK && connections==2);
    assert(zello_connect()==ESP_OK && connections==3);
}
""")

    def test_decoder_reset_and_decode_do_not_overlap(self):
        headers = {
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM -2\n",
            "esp_check.h": "#define ESP_RETURN_ON_FALSE(v,e,...) do {if(!(v)) return (e);} while(0)\n",
            "esp_log.h": "#define ESP_LOGI(...) ((void)0)\n",
            "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY 0\n",
            "freertos/semphr.h": (
                "#pragma once\n#include <pthread.h>\n#include <stdlib.h>\n"
                "typedef pthread_mutex_t *SemaphoreHandle_t;\n"
                "static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {"
                "SemaphoreHandle_t p=malloc(sizeof(*p));pthread_mutex_init(p,0);return p;}\n"
                "#define xSemaphoreTake(p,t) pthread_mutex_lock(p)\n"
                "#define xSemaphoreGive(p) pthread_mutex_unlock(p)\n"
            ),
            "mbedtls/base64.h": (
                "#include <stddef.h>\n"
                "static int mbedtls_base64_encode(unsigned char*out,size_t cap,size_t*n,"
                "const unsigned char*in,size_t len) {(void)cap;(void)in;(void)len;*n=1;out[0]='x';return 0;}\n"
            ),
            "opus.h": r"""
#pragma once
#include <stdint.h>
typedef struct {int unused;} OpusEncoder;
typedef struct {int unused;} OpusDecoder;
#define OPUS_OK 0
#define OPUS_APPLICATION_VOIP 0
#define OPUS_SIGNAL_VOICE 0
#define OPUS_RESET_STATE 1
#define OPUS_SET_BITRATE(v) 0
#define OPUS_SET_COMPLEXITY(v) 0
#define OPUS_SET_SIGNAL(v) 0
#define OPUS_SET_VBR(v) 0
#define OPUS_SET_INBAND_FEC(v) 0
#define OPUS_SET_PACKET_LOSS_PERC(v) 0
OpusEncoder *opus_encoder_create(int,int,int,int*);
OpusDecoder *opus_decoder_create(int,int,int*);
int opus_encoder_ctl(OpusEncoder*,int,...);
int opus_decoder_ctl(OpusDecoder*,int,...);
int opus_encode(OpusEncoder*,const int16_t*,int,unsigned char*,int);
int opus_decode(OpusDecoder*,const unsigned char*,int,int16_t*,int,int);
const char *opus_strerror(int);
""",
        }
        self.run_c(COMMON + r"""
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "opus.h"
#include "opus_codec.h"
static OpusEncoder enc;
static OpusDecoder dec;
static atomic_int active,overlap;
static atomic_bool start;
OpusEncoder *opus_encoder_create(int a,int b,int c,int *e) {*e=0;return &enc;}
OpusDecoder *opus_decoder_create(int a,int b,int *e) {*e=0;return &dec;}
int opus_encoder_ctl(OpusEncoder*p,int c,...) {return 0;}
const char *opus_strerror(int e) {return "";}
int opus_encode(OpusEncoder*p,const int16_t*i,int frames,unsigned char*o,int n) {return 1;}
static void touch_state(void) {
    if(atomic_fetch_add(&active,1)) atomic_store(&overlap,1);
    struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    atomic_fetch_sub(&active,1);
}
int opus_decode(OpusDecoder*p,const unsigned char*b,int n,int16_t*out,int max,int fec) {
    touch_state();return 1;
}
int opus_decoder_ctl(OpusDecoder*p,int c,...) {touch_state();return 0;}
static void *decode_worker(void *arg) {
    unsigned char packet=0;int16_t pcm[1920];
    while(!atomic_load(&start)) {}
    for(int i=0;i<30;i++) opus_codec_decode(&packet,1,pcm,1920);
    return NULL;
}
static void *reset_worker(void *arg) {
    while(!atomic_load(&start)) {}
    for(int i=0;i<30;i++) opus_codec_reset_decoder();
    return NULL;
}
int main(void) {
    assert(opus_codec_init()==ESP_OK);
    pthread_t a,b;pthread_create(&a,NULL,decode_worker,NULL);pthread_create(&b,NULL,reset_worker,NULL);
    atomic_store(&start,true);
    pthread_join(a,NULL);pthread_join(b,NULL);
    assert(!atomic_load(&overlap));
}
""", headers, [ROOT / "main/opus_codec.c"])

    def test_jwt_expiry_reads_exp_claim(self):
        self.run_c(r"""
#include <assert.h>
#include <stdint.h>
#include "jwt_util.h"
int main(void) {
    int64_t exp = 0;
    // {"alg":"RS256","typ":"JWT"} . {"iss":"x","exp":1730000000} . sig, unpadded base64url
    assert(jwt_expiry("eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJ4IiwiZXhwIjoxNzMwMDAwMDAwfQ.c2ln", &exp));
    assert(exp == 1730000000);
    // {"note":"\"exp\":5","exp" : 42}: "exp" inside a string value is skipped
    assert(jwt_expiry("h.eyJub3RlIjoiXCJleHBcIjo1IiwiZXhwIiA6IDQyfQ.s", &exp));
    assert(exp == 42);
    // {"iss":"x"}: no expiry
    assert(!jwt_expiry("h.eyJpc3MiOiJ4In0.s", &exp));
    // truncated paste: no second dot, or a bad character
    assert(!jwt_expiry("h.eyJleHAiOjF9", &exp));
    assert(!jwt_expiry("h.ey!J.s", &exp));
    assert(!jwt_expiry("not a token", &exp));
}
""", extra_sources=[ROOT / "main/jwt_util.c"])


if __name__ == "__main__":
    unittest.main()

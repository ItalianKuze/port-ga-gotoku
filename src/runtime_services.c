/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "../gpu/shim/pgg_env.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <arpa/inet.h>
#else
/* htons/inet_pton/inet_ntop (ws2_32); WIN32_LEAN_AND_MEAN keeps windows.h from pulling in winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define USER_ID 1
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_WEBAPI_CONTEXT_NOT_FOUND ((int32_t)0x80552904)
#define NP_WEBAPI_CONTEXT_MAX ((int32_t)0x8055291a)
#define NP_WEBAPI_OUT_OF_MEMORY ((int32_t)0x80552901)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define COMMON_DIALOG_ALREADY_INITIALIZED ((int32_t)0x80B80002)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define PLAYGO_INVALID_ARGUMENT ((int32_t)0x80B20004)
#define PLAYGO_NOT_INITIALIZED ((int32_t)0x80B20005)
#define PLAYGO_ALREADY_INITIALIZED ((int32_t)0x80B20006)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* sceScreenShotSetOverlayImage configures an image and its placement for later
 * screenshot capture. Hokuto calls this with a package-relative asset and
 * ignores the result. Keep the actual configuration; screenshot capture itself
 * is not implemented by this runtime. */
static pthread_mutex_t screenshot_lock=PTHREAD_MUTEX_INITIALIZER;
/* sceScreenShotEnable / Disable, as shadPS4: no capture exists here, so they only record the
 * state and return OK; each is logged the first time (the game disables screenshots in some
 * story scenes). */
static int screenshot_enabled=1;
static ABI int32_t screenshot_enable(void) {
    static int logged;
    pthread_mutex_lock(&screenshot_lock);
    screenshot_enabled=1;
    pthread_mutex_unlock(&screenshot_lock);
    if (!logged++) puts("Runtime: screenshots enabled by the game (no capture; logged once)");
    return 0;
}
static ABI int32_t screenshot_disable(void) {
    static int logged;
    pthread_mutex_lock(&screenshot_lock);
    screenshot_enabled=0;
    pthread_mutex_unlock(&screenshot_lock);
    if (!logged++) puts("Runtime: screenshots disabled by the game (no capture; logged once)");
    return 0;
}
static char screenshot_overlay_path[1024];
static int32_t screenshot_overlay_x, screenshot_overlay_y;
static ABI int32_t screenshot_set_overlay(const char *path,int32_t x,int32_t y) {
    if (!path) return (int32_t)0x80020016; /* SCE_KERNEL_ERROR_EFAULT */
    size_t n=strnlen(path,sizeof(screenshot_overlay_path));
    if (!n || n==sizeof(screenshot_overlay_path)) return (int32_t)0x80020016;
    pthread_mutex_lock(&screenshot_lock);
    memcpy(screenshot_overlay_path,path,n+1);
    screenshot_overlay_x=x; screenshot_overlay_y=y;
    pthread_mutex_unlock(&screenshot_lock);
    printf("Runtime: screenshot overlay configured: %s at (%d,%d); capture unavailable\n",path,x,y);
    return ORBIS_OK;
}
static ABI int32_t video_recording_set_info(uint32_t kind,const void *info,uint64_t size) {
    (void)size; /* shadPS4's implementation validates the enum and pointer only. */
    switch (kind) {
    case 0x2: case 0x6: case 0x7: case 0x8: case 0xd:
    case 0xa01: case 0xa007: case 0xa008: case 0xa009:
        break;
    default:
        return (int32_t)0x80A80003; /* VIDEO_RECORDING_ERROR_INVALID_VALUE */
    }
    if (!info) return (int32_t)0x80A80003;
    return ORBIS_OK; /* capture metadata is accepted but not stored by this runtime */
}

/* ---- UserService ---- */
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_color(int32_t id,int32_t *color) {
    if (!color || id!=USER_ID) return USER_INVALID_ARGUMENT;
    *color=1; /* OrbisUserServiceUserColor::Red; matches the default local profile. */
    return ORBIS_OK;
}
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    ids[0]=USER_ID; ids[1]=ids[2]=ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id!=USER_ID || !name) return USER_INVALID_ARGUMENT;
    const char *value=pgg_getenv("USER_NAME") ? pgg_getenv("USER_NAME") : "Player";
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (!pending) return USER_NO_EVENT;
    event[0]=0; event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=pgg_getenv("LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
    /* UTC offset in minutes: the local fields read back as UTC (bb_timegm, runtime.h), as in
       runtime_kernel.c's kernel_gettimezone; same as tm_gmtoff, which the Windows CRT lacks. */
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)((int64_t)(bb_timegm(&t)-now)/60); break; }
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    status[6]=1; /* OrbisSystemServiceStatus::is_cpu_mode7_cpu_normal */
    return 0;
}
typedef struct { float ratio; } SystemServiceDisplaySafeAreaInfo;
static ABI int32_t system_display_safe_area(SystemServiceDisplaySafeAreaInfo *info) {
    if (!info) return SYSTEM_PARAMETER;
    info->ratio=1.0f;
    return ORBIS_OK;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
/* shadPS4 currently models these RUDP APIs as success stubs. Preserve the
 * buffers and arguments observed at Hokuto's call sites, while making clear
 * that no RUDP transport or IO thread is provided here. */
static void *rudp_pool;
static uint64_t rudp_pool_size;
static ABI int32_t rudp_init(void *pool,uint64_t size) {
    rudp_pool=pool; rudp_pool_size=size;
    printf("Runtime: RUDP initialized in offline HLE mode (pool=%p, size=%" PRIu64 "); transport unavailable\n",
           pool,size);
    return ORBIS_OK;
}
static ABI int32_t rudp_enable_internal_io_thread(int32_t stack_size,int32_t priority) {
    printf("Runtime: RUDP internal IO thread requested (stack=%d, priority=%d); no thread started\n",
           stack_size,priority);
    return ORBIS_OK;
}
static ABI int32_t rudp_end(void) {
    rudp_pool=NULL; rudp_pool_size=0;
    note("RUDP HLE ended");
    return ORBIS_OK;
}
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}
/* shadPS4's sceNpSetNpTitleId is an offline stub: it requires the title ID and
 * 128-byte title secret structures, records no PSN state, and returns success. */
static ABI int32_t np_set_title_id(const char *title_id,const void *title_secret) {
    if (!title_id || !title_secret) return NP_INVALID_ARGUMENT;
    printf("Runtime: NP title ID configured: %.*s; PSN unavailable\n",13,title_id);
    return 0;
}

/* The upstream sceNpWebApiInitialize contract creates a library context from
 * an HTTP context ID and pool size, returning the new context ID. Keep that
 * lifetime visible even though this offline runtime has no PSN transport. */
typedef struct NpWebApiContext {
    int32_t id, http_context_id;
    uint64_t pool_size;
    struct NpWebApiContext *next;
} NpWebApiContext;
static NpWebApiContext *np_webapi_contexts;
static ABI int32_t np_webapi_initialize(int32_t http_context_id,uint64_t pool_size) {
    NpWebApiContext *ctx=calloc(1,sizeof(*ctx));
    if (!ctx) return NP_WEBAPI_OUT_OF_MEMORY;
    int32_t id=new_id();
    if (id<=0 || id>=0x8000) { free(ctx); return NP_WEBAPI_CONTEXT_MAX; }
    ctx->id=id; ctx->http_context_id=http_context_id; ctx->pool_size=pool_size;
    pthread_mutex_lock(&lock);
    ctx->next=np_webapi_contexts; np_webapi_contexts=ctx;
    pthread_mutex_unlock(&lock);
    printf("Runtime: NP Web API context %d created (HTTP context=%d, pool=%" PRIu64 "); offline PSN\n",
           id,http_context_id,pool_size);
    return id;
}
static ABI int32_t np_webapi_terminate(int32_t id) {
    pthread_mutex_lock(&lock);
    NpWebApiContext **link=&np_webapi_contexts;
    while (*link && (*link)->id!=id) link=&(*link)->next;
    if (!*link) { pthread_mutex_unlock(&lock); return NP_WEBAPI_CONTEXT_NOT_FOUND; }
    NpWebApiContext *ctx=*link; *link=ctx->next;
    pthread_mutex_unlock(&lock);
    free(ctx);
    return 0;
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; uint32_t mode, progress; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0,0,0},{"MsgDialog",0,0,0,0},{"SaveDataDialog",0,0,0,0},
                         {"NpProfileDialog",0,0,0,0},{"NpCommerceDialog",0,0,0,0},{"ImeDialog",0,0,0,0}};
static int common_initialized;
static ABI int32_t common_init(void) {
    if (common_initialized) return COMMON_DIALOG_ALREADY_INITIALIZED;
    common_initialized=1; return 0;
}
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=3; return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static ABI int32_t save_dialog_ready(void) { return 1; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4)

/* MsgDialog, as in shadPS4 (libraries/system/msgdialog.cpp) but without UI: a dialog with buttons
 * is answered at once -- OK or YES, buttonId 1 (NO and the second button would be 2) -- and every
 * dialog, its text and the answer given are logged. Dialogs without buttons (NONE, WAIT,
 * WAIT_CANCEL) and progress bars stay RUNNING until the game closes them, as on the PS4.
 * OrbisMsgDialogParam: BaseParam(48), size_t size, u32 mode, pad, then three pointers. */
enum { MSG_USER=1, MSG_PROGRESS=2, MSG_SYSTEM=3 };
enum { BTN_OK=0, BTN_YESNO=1, BTN_NONE=2, BTN_OK_CANCEL=3, BTN_WAIT=5, BTN_WAIT_CANCEL=6,
       BTN_YESNO_FOCUS_NO=7, BTN_OK_CANCEL_FOCUS_CANCEL=8, BTN_TWO_BUTTONS=9 };
typedef struct { const char *msg1, *msg2; char reserved[32]; } MsgButtonsParam;
typedef struct { uint32_t button_type; int32_t pad; const char *msg; MsgButtonsParam *buttons; char reserved[24]; } MsgUserParam;
typedef struct { uint32_t bar_type; int32_t pad; const char *msg; char reserved[64]; } MsgProgressParam;
typedef struct { uint32_t type; char reserved[32]; } MsgSystemParam;
typedef struct {
    uint64_t base_size; uint8_t base_reserved[36]; uint32_t base_magic;
    uint64_t size; uint32_t mode; int32_t pad;
    MsgUserParam *user; MsgProgressParam *progress; MsgSystemParam *system;
    int32_t user_id; char reserved[40]; int32_t pad2;
} MsgDialogParam;
typedef struct { uint32_t mode, result, button_id; char reserved[32]; } MsgDialogResult;
_Static_assert(sizeof(MsgDialogParam)==136,"OrbisMsgDialogParam layout");
_Static_assert(sizeof(MsgDialogResult)==44,"MsgDialog DialogResult layout");
#define MSG_DIALOG (&dialogs[1])
static MsgDialogResult msg_result;
static const char *button_type_name(uint32_t t) {
    static const char *names[]={"OK","YESNO","NONE","OK_CANCEL","?4","WAIT","WAIT_CANCEL",
                                "YESNO_FOCUS_NO","OK_CANCEL_FOCUS_CANCEL","TWO_BUTTONS"};
    return t<sizeof(names)/sizeof(*names) ? names[t] : "?";
}
/* The dialog text on one line, at most 200 characters. */
static void msg_text(const char *text, char *out, size_t size) {
    size_t n=0;
    for (const char *p=text; p && *p && n+1<size && n<200; ++p) out[n++]=(*p=='\n' || *p=='\r') ? ' ' : *p;
    out[n]=0;
}
static void msg_finish(uint32_t result, uint32_t button_id, const char *why) {
    msg_result=(MsgDialogResult){.mode=0,.result=result,.button_id=button_id};
    MSG_DIALOG->status=3;
    printf("Runtime: MsgDialog answered: %s (result %u, buttonId %u)\n",why,result,button_id);
}
/* ErrorDialog, as shadPS4 (libraries/ime/error_dialog.cpp) without UI: Open logs the system error
 * code and the dialog is dismissed at once (FINISHED), as if the user pressed OK.
 * Param: s32 size, s32 errorCode, s32 userId, s32 reserved. */
typedef struct { int32_t size, error_code, user_id, reserved; } ErrorDialogParam;
static int32_t error_dialog_status; /* NONE 0, INITIALIZED 1, RUNNING 2, FINISHED 3 */
static ABI int32_t error_dialog_init(void) {
    if (error_dialog_status!=0) return (int32_t)0x80B80004;                          /* ALREADY_INITIALIZED */
    error_dialog_status=1; return 0;
}
static ABI int32_t error_dialog_open(const ErrorDialogParam *p) {
    if (error_dialog_status!=1 && error_dialog_status!=3) return (int32_t)0x80B80006; /* INVALID_STATE */
    if (!p) return (int32_t)0x80B8000D;                                               /* ARG_NULL */
    printf("Runtime: ErrorDialog opened: error code 0x%08X (user %d); dismissed (no dialog UI)\n",
           (uint32_t)p->error_code,p->user_id);
    error_dialog_status=3; return 0;
}
static ABI int32_t error_dialog_status_get(void) { return error_dialog_status; }
static ABI int32_t error_dialog_close(void) {
    if (error_dialog_status!=2) return (int32_t)0x80B8000B;                           /* NOT_RUNNING */
    error_dialog_status=3; return 0;
}
static ABI int32_t error_dialog_term(void) {
    if (error_dialog_status==2) error_dialog_close();
    if (error_dialog_status==0) return (int32_t)0x80B80003;                           /* NOT_INITIALIZED */
    error_dialog_status=0; return 0;
}
static ABI int32_t msg_init(void) { return dialog_init(1); }
static ABI int32_t msg_open(const MsgDialogParam *p) {
    if (MSG_DIALOG->status!=1 && MSG_DIALOG->status!=3) return (int32_t)0x80B80006; /* INVALID_STATE */
    if (!p) return (int32_t)0x80B8000D;                                              /* ARG_NULL */
    msg_result=(MsgDialogResult){0};
    MSG_DIALOG->mode=p->mode; MSG_DIALOG->progress=0; MSG_DIALOG->status=2;         /* RUNNING */
    char text[256]="";
    if (p->mode==MSG_USER && p->user) {
        const uint32_t type=p->user->button_type;
        msg_text(p->user->msg,text,sizeof(text));
        printf("Runtime: MsgDialog opened: USER_MSG, buttons %s, text \"%s\"\n",button_type_name(type),text);
        if (type==BTN_TWO_BUTTONS && p->user->buttons) {
            char b1[64], b2[64];
            msg_text(p->user->buttons->msg1,b1,sizeof(b1)); msg_text(p->user->buttons->msg2,b2,sizeof(b2));
            printf("Runtime: MsgDialog buttons: 1 \"%s\", 2 \"%s\"\n",b1,b2);
        }
        if (type==BTN_YESNO_FOCUS_NO || type==BTN_OK_CANCEL_FOCUS_CANCEL)
            printf("Runtime: MsgDialog NOTE: the default focus is on %s; answering %s anyway\n",
                   type==BTN_YESNO_FOCUS_NO ? "NO" : "CANCEL", type==BTN_YESNO_FOCUS_NO ? "YES" : "OK");
        switch (type) {
        case BTN_NONE: case BTN_WAIT: case BTN_WAIT_CANCEL:
            puts("Runtime: MsgDialog has no buttons: stays open until the game closes it"); break;
        case BTN_YESNO: case BTN_YESNO_FOCUS_NO: msg_finish(0,1,"YES"); break;
        case BTN_TWO_BUTTONS: msg_finish(0,1,"button 1"); break;
        default: msg_finish(0,1,"OK"); break;
        }
    } else if (p->mode==MSG_PROGRESS && p->progress) {
        msg_text(p->progress->msg,text,sizeof(text));
        printf("Runtime: MsgDialog opened: PROGRESS_BAR (type %u), text \"%s\"; open until closed\n",p->progress->bar_type,text);
    } else if (p->mode==MSG_SYSTEM && p->system) {
        printf("Runtime: MsgDialog opened: SYSTEM_MSG type %u\n",p->system->type);
        msg_finish(0,1,"OK");
    } else {
        printf("Runtime: MsgDialog opened: mode %u without its parameters\n",p->mode);
        msg_finish(0,1,"OK");
    }
    return 0;
}
static ABI int32_t msg_status(void) { return MSG_DIALOG->status; }
static ABI int32_t msg_get_result(MsgDialogResult *result) {
    if (MSG_DIALOG->status!=3) return (int32_t)0x80B80005;                           /* NOT_FINISHED */
    if (!result) return (int32_t)0x80B8000D;                                         /* ARG_NULL */
    *result=msg_result; return 0;
}
static ABI int32_t msg_close(void) {
    if (MSG_DIALOG->status!=2) return (int32_t)0x80B8000B;                           /* NOT_RUNNING */
    msg_finish(0,0,"closed by the game");
    return 0;
}
static int32_t msg_progress_check(uint32_t target) {
    if (MSG_DIALOG->status!=2) return (int32_t)0x80B8000B;                           /* NOT_RUNNING */
    if (MSG_DIALOG->mode!=MSG_PROGRESS) return (int32_t)0x80B8000F;                  /* NOT_SUPPORTED */
    if (target!=0) return (int32_t)0x80B8000A;                                       /* PARAM_INVALID */
    return 0;
}
static ABI int32_t msg_progress_inc(uint32_t target, uint32_t delta) {
    int32_t e=msg_progress_check(target); if (!e) MSG_DIALOG->progress+=delta; return e;
}
static ABI int32_t msg_progress_value(uint32_t target, uint32_t value) {
    int32_t e=msg_progress_check(target); if (!e) MSG_DIALOG->progress=value; return e;
}
static ABI int32_t msg_progress_msg(uint32_t target, const char *text) {
    int32_t e=msg_progress_check(target);
    if (!e) { char t[256]; msg_text(text,t,sizeof(t)); printf("Runtime: MsgDialog progress text \"%s\"\n",t); }
    return e;
}
static ABI int32_t msg_term(void) {
    if (MSG_DIALOG->status==2) msg_close();
    return dialog_term(1);
}
/* SaveDataDialog, as shadPS4 (libraries/save_data/dialog/savedatadialog_ui.cpp) but without a list
 * on screen yet: each dialog is answered the way pressing Cross at once would answer it on the PS4.
 *   LIST: the slots the game names (items->dirName[]) that exist on disk, after a "new save" entry
 *         when saving with newItem; the item focusPos points at is chosen (LISTHEAD/DATAHEAD the
 *         first, LISTTAIL/DATATAIL the last, DATALTATEST the newest, DATAOLDEST the oldest, DIRNAME
 *         the named one) and its dirName and param are returned. Loading with nothing to load is
 *         USER_CANCELED. Before this the result never carried a dirName: every save created a new
 *         slot and "Continue" loaded nothing (2026-10-11).
 *   USER_MSG / SYSTEM_MSG: YES or OK (buttonId 1); dialogs without buttons (NONE, PROGRESS) stay
 *         open until the game closes them.
 *   ERROR_CODE: the error code is always logged, then OK.
 *   PROGRESS_BAR: open until the game closes it (save_close). */
typedef struct { char data[32]; } SaveDialogDirName;
typedef struct { char data[10]; char pad[6]; } SaveDialogTitleId;
typedef struct { const char *title; void *icon; uint64_t icon_size; uint8_t reserved[32]; } SaveDialogNewItem;
typedef struct {
    int32_t user, pad; const SaveDialogTitleId *title; const SaveDialogDirName *dirs; uint32_t dir_count, pad2;
    const SaveDialogNewItem *new_item; uint32_t focus, pad3; const SaveDialogDirName *focus_dir; uint32_t style;
    uint8_t reserved[32];
} SaveDialogItems;
typedef struct { uint32_t button_type, msg_type; const char *msg; uint8_t reserved[32]; } SaveDialogUserMsg;
typedef struct { uint32_t msg_type; int32_t pad; uint64_t value; uint8_t reserved[32]; } SaveDialogSysMsg;
typedef struct { uint32_t error_code; uint8_t reserved[32]; } SaveDialogErrorCode;
typedef struct {
    uint64_t base_size; uint8_t base_reserved[36]; uint32_t base_magic;
    int32_t size; uint32_t mode, disp_type; int32_t pad;
    void *anim; const SaveDialogItems *items; const SaveDialogUserMsg *user_msg; const SaveDialogSysMsg *sys_msg;
    const SaveDialogErrorCode *error_code; void *progress; void *user_data; void *option; uint8_t reserved[24];
} SaveDialogParam;
typedef struct {
    uint32_t mode, result, button, pad;
    SaveDialogDirName *dir_name;
    void *param, *user_data;
    uint8_t reserved[32];
} SaveDialogOutput;
_Static_assert(sizeof(SaveDialogOutput)==72,"SaveDataDialogResult layout");
_Static_assert(offsetof(SaveDialogParam,mode)==52 && offsetof(SaveDialogParam,items)==72 &&
               offsetof(SaveDialogParam,user_data)==112,"OrbisSaveDataDialogParam layout");
_Static_assert(offsetof(SaveDialogItems,dir_count)==24 && offsetof(SaveDialogItems,new_item)==32 &&
               offsetof(SaveDialogItems,focus)==40 && offsetof(SaveDialogItems,focus_dir)==48,"SaveDialogItems layout");
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_status(void) { printf("Runtime: save_status -> %d\n", dialogs[2].status); return dialog_status(2); }
static ABI int32_t save_term(void) { return dialog_term(2); }
enum { SAVE_LIST=1, SAVE_USER_MSG=2, SAVE_SYSTEM_MSG=3, SAVE_ERROR_CODE=4, SAVEDATA_DIALOG_PROGRESS_BAR=5 };
enum { SAVE_TYPE_SAVE=1, SAVE_TYPE_LOAD=2, SAVE_TYPE_DELETE=3 };
#define SAVEDATA_DIALOG_TARGET_DEFAULT 0u
/* What the last dialog answers through sceSaveDataDialogGetResult. */
static struct { uint32_t mode, result, button; char dir[33]; int has_param; uint8_t param[1328]; void *user_data; } save_answer;
static const char *save_type_name(uint32_t t) { return t==1 ? "save" : t==2 ? "load" : t==3 ? "delete" : "?"; }
static void save_finish(uint32_t result, uint32_t button) {
    save_answer.result=result; save_answer.button=button; dialogs[2].status=3; /* FINISHED */
}
/* The LIST dialog's choice: index into the shown list (new item first), as shadPS4 focuses it. */
static void save_list_answer(const SaveDialogParam *p) {
    const SaveDialogItems *it=p->items;
    if (!it) { printf("Runtime: SaveDataDialog LIST without items; cancelled\n"); save_finish(1,0); return; }
    const char *title=it->title && it->title->data[0] ? it->title->data : NULL;
    enum { MAX_ITEMS=64 };
    struct { char dir[33]; int64_t mtime; } list[MAX_ITEMS];
    int count=0;
    const int new_item = p->disp_type==SAVE_TYPE_SAVE && it->new_item;
    char names[512]=""; size_t used=0;
    for (uint32_t i=0;i<it->dir_count && it->dirs && count<MAX_ITEMS;++i) {
        char name[33]; memcpy(name,it->dirs[i].data,32); name[32]=0;
        int64_t mtime=0;
        const int exists=name[0] && runtime_savedata_slot(it->user,title,name,NULL,&mtime);
        if (used<sizeof(names)-40) used+=(size_t)snprintf(names+used,sizeof(names)-used," %s%s",name[0] ? name : "\"\"",exists ? "" : "(none)");
        if (!exists) continue;
        memcpy(list[count].dir,name,sizeof(name)); list[count].mtime=mtime; ++count;
    }
    static const char *focus_names[]={"LISTHEAD","LISTTAIL","DATAHEAD","DATATAIL","DATALATEST","DATAOLDEST","DIRNAME"};
    char focus_dir[33]=""; if (it->focus_dir) { memcpy(focus_dir,it->focus_dir->data,32); focus_dir[32]=0; }
    printf("Runtime: SaveDataDialog LIST (%s, title %s): slots%s; new item %s; focus %s%s%s\n",
           save_type_name(p->disp_type),title ? title : "(game)",names[0] ? names : " (none)",new_item ? "yes" : "no",
           it->focus<7 ? focus_names[it->focus] : "?",it->focus==6 ? " " : "",it->focus==6 ? focus_dir : "");
    /* Shown order: new item (dirName "") first, then the existing slots. */
    const int total=count+new_item;
    int choice=-1; /* -1: the new item; otherwise index into list */
    if (total==0) {
        printf("Runtime: SaveDataDialog: nothing to %s; cancelled\n",save_type_name(p->disp_type));
        save_finish(1,0); /* USER_CANCELED */
        return;
    }
    switch (it->focus) {
    case 0: case 2: choice=new_item ? -1 : 0; break;                        /* LISTHEAD, DATAHEAD */
    case 1: case 3: choice=count ? count-1 : -1; break;                     /* LISTTAIL, DATATAIL */
    case 4: case 5: {                                                        /* newest / oldest */
        choice=new_item && !count ? -1 : 0;
        for (int i=1;i<count;++i)
            if (it->focus==4 ? list[i].mtime>list[choice].mtime : list[i].mtime<list[choice].mtime) choice=i;
        break;
    }
    case 6:                                                                  /* DIRNAME */
        choice=new_item ? -1 : 0;
        for (int i=0;i<count;++i) if (!strcmp(list[i].dir,focus_dir)) choice=i;
        break;
    default: choice=new_item ? -1 : 0; break;
    }
    save_answer.has_param=0; save_answer.dir[0]=0;
    if (choice<0) {
        printf("Runtime: SaveDataDialog answered: new save (the game names the new slot when it mounts it)\n");
    } else {
        memcpy(save_answer.dir,list[choice].dir,sizeof(save_answer.dir));
        save_answer.has_param=runtime_savedata_slot(it->user,title,list[choice].dir,save_answer.param,NULL);
        printf("Runtime: SaveDataDialog answered: %s slot %s\n",
               p->disp_type==SAVE_TYPE_SAVE ? "overwrite" : p->disp_type==SAVE_TYPE_LOAD ? "load" : "delete",list[choice].dir);
    }
    save_finish(0,1); /* OK, buttonId OK */
}
static ABI int32_t save_open(const SaveDialogParam *p) {
    if (!dialogs[2].initialized) return DIALOG_NOT_INITIALIZED;
    if (!p) return (int32_t)0x80B8000D; /* CommonDialog ARG_NULL */
    dialogs[2].mode = p->mode;
    dialogs[2].progress = 0;
    memset(&save_answer,0,sizeof(save_answer));
    save_answer.mode=p->mode; save_answer.user_data=p->user_data;
    switch (p->mode) {
    case SAVEDATA_DIALOG_PROGRESS_BAR:
        dialogs[2].status = 2; /* RUNNING */
        printf("Runtime: SaveDataDialog opened (PROGRESS_BAR)\n");
        break;
    case SAVE_LIST:
        save_list_answer(p);
        break;
    case SAVE_USER_MSG: {
        const SaveDialogUserMsg *m=p->user_msg;
        char text[201]=""; if (m && m->msg) snprintf(text,sizeof(text),"%s",m->msg);
        for (char *c=text;*c;++c) if (*c=='\n' || *c=='\r') *c=' ';
        const uint32_t buttons=m ? m->button_type : 0;
        printf("Runtime: SaveDataDialog USER_MSG (%s, buttons %s, %s): \"%s\"\n",save_type_name(p->disp_type),
               buttons==0 ? "OK" : buttons==1 ? "YESNO" : buttons==2 ? "NONE" : buttons==3 ? "OKCANCEL" : "?",
               m && m->msg_type ? "error" : "normal",text);
        if (buttons==2) { dialogs[2].status=2; puts("Runtime: SaveDataDialog has no buttons: open until the game closes it"); }
        else { save_finish(0,1); printf("Runtime: SaveDataDialog answered: %s\n",buttons==1 ? "YES" : "OK"); }
        break;
    }
    case SAVE_SYSTEM_MSG: {
        const uint32_t type=p->sys_msg ? p->sys_msg->msg_type : 0;
        static const char *types[]={"?","NODATA","CONFIRM","OVERWRITE","NOSPACE","PROGRESS","FILE_CORRUPTED","FINISHED",
                                    "NOSPACE_CONTINUABLE","?","CORRUPTED_AND_DELETED","CORRUPTED_AND_CREATED","?",
                                    "CORRUPTED_AND_RESTORE","TOTAL_SIZE_EXCEEDED"};
        printf("Runtime: SaveDataDialog SYSTEM_MSG (%s): %s (value %llu)\n",save_type_name(p->disp_type),
               type<15 ? types[type] : "?",p->sys_msg ? (unsigned long long)p->sys_msg->value : 0ull);
        if (type==5) { dialogs[2].status=2; puts("Runtime: SaveDataDialog progress message: open until the game closes it"); }
        else { save_finish(0,1); printf("Runtime: SaveDataDialog answered: %s\n",type==2 || type==3 ? "YES" : "OK"); }
        break;
    }
    case SAVE_ERROR_CODE:
        printf("Runtime: SaveDataDialog ERROR_CODE (%s): error 0x%08X; answered OK\n",save_type_name(p->disp_type),
               p->error_code ? p->error_code->error_code : 0u);
        save_finish(0,1);
        break;
    default:
        printf("Runtime: SaveDataDialog opened with mode %u; answered OK\n",p->mode);
        save_finish(0,1);
        break;
    }
    return ORBIS_OK;
}
static ABI int32_t save_close(void) {
    printf("Runtime: save_close called (status=%d)\n", dialogs[2].status);
    if (dialogs[2].status != 2) return (int32_t)0x80B8000B; /* NOT_RUNNING */
    dialogs[2].status = 3; /* FINISHED */
    return ORBIS_OK;
}
static ABI int32_t save_progress_set(uint32_t target, uint32_t rate) {
    printf("Runtime: save_progress_set(target=%u, rate=%u, status=%d, mode=%u)\n", target, rate, dialogs[2].status, dialogs[2].mode);
    if (dialogs[2].status != 2) return (int32_t)0x80B8000B; /* NOT_RUNNING */
    if (dialogs[2].mode != SAVEDATA_DIALOG_PROGRESS_BAR) return (int32_t)0x80B8000F; /* NOT_SUPPORTED */
    if (target != SAVEDATA_DIALOG_TARGET_DEFAULT) return (int32_t)0x80B8000A; /* PARAM_INVALID */
    dialogs[2].progress = rate;
    return ORBIS_OK;
}
static ABI int32_t save_progress_inc(uint32_t target, uint32_t delta) {
    if (dialogs[2].status != 2) return (int32_t)0x80B8000B; /* NOT_RUNNING */
    if (dialogs[2].mode != SAVEDATA_DIALOG_PROGRESS_BAR) return (int32_t)0x80B8000F; /* NOT_SUPPORTED */
    if (target != SAVEDATA_DIALOG_TARGET_DEFAULT) return (int32_t)0x80B8000A; /* PARAM_INVALID */
    dialogs[2].progress += delta;
    return ORBIS_OK;
}
static ABI int32_t save_get_status(void) { return dialogs[2].status; }
static ABI int32_t save_result(SaveDialogOutput *result) {
    printf("Runtime: save_result called (status=%d)\n", dialogs[2].status);
    if (dialogs[2].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!result) return (int32_t)0x80B8000D; /* ARG_NULL */
    /* As shadPS4's SaveDialogResult::CopyTo: mode, result, buttonId, the chosen dirName (empty for a
     * new save) and its param when the slot has one. */
    result->mode=save_answer.mode; result->result=save_answer.result; result->button=save_answer.button;
    result->user_data=save_answer.user_data;
    if (result->dir_name) { memset(result->dir_name,0,sizeof(*result->dir_name)); memcpy(result->dir_name->data,save_answer.dir,strnlen(save_answer.dir,32)); }
    if (result->param && save_answer.has_param) memcpy(result->param,save_answer.param,sizeof(save_answer.param));
    return ORBIS_OK;
}
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=pgg_getenv("IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    if (!bbgpu_text_input_begin(initial,prompt[0] ? prompt : "Text")) {
        const char *name=pgg_getenv("USER_NAME");
        ime_complete(0,name ? name : initial[0] ? initial : "Player");
    } else printf("Runtime: ImeDialog opened: type in the game window, Enter to confirm, Esc to cancel\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=bbgpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies: accepted locally, recorded in the log ---- */
/* Live trophy contexts and handles, so Destroy can reject an unknown one as shadPS4 does
 * (np_trophy.cpp: INVALID_CONTEXT 0x80551609, INVALID_HANDLE 0x80551608). */
#define TROPHY_INVALID_HANDLE ((int32_t)0x80551608)
#define TROPHY_INVALID_CONTEXT ((int32_t)0x80551609)
enum { TROPHY_SLOTS=32 };
static int32_t trophy_contexts[TROPHY_SLOTS], trophy_handles[TROPHY_SLOTS];
static void trophy_track(int32_t *slots, int32_t id) {
    pthread_mutex_lock(&lock);
    for (int i=0;i<TROPHY_SLOTS;++i) if (!slots[i]) { slots[i]=id; break; }
    pthread_mutex_unlock(&lock);
}
static int trophy_untrack(int32_t *slots, int32_t id) {
    int found=0;
    pthread_mutex_lock(&lock);
    for (int i=0;i<TROPHY_SLOTS && !found;++i) if (id>0 && slots[i]==id) { slots[i]=0; found=1; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id(); trophy_track(trophy_contexts,*ctx); return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) {
    if (!handle) return TROPHY_INVALID;
    *handle=new_id(); trophy_track(trophy_handles,*handle); return 0;
}
static ABI int32_t trophy_destroy_context(int32_t ctx) { return trophy_untrack(trophy_contexts,ctx) ? 0 : TROPHY_INVALID_CONTEXT; }
static ABI int32_t trophy_destroy_handle(int32_t handle) { return trophy_untrack(trophy_handles,handle) ? 0 : TROPHY_INVALID_HANDLE; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) { (void)ctx; (void)handle; (void)options; return 0; }
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    printf("Runtime: trophy %d unlocked\n",id);
    if (platinum) *platinum=-1;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
typedef struct { const void *buffer; uint32_t size, reserved; } PlayGoInitParams;
static int playgo_initialized;
static ABI int32_t playgo_init(const PlayGoInitParams *params) {
    if (!params || !params->buffer) return PLAYGO_BAD_POINTER;
    if (params->size<0x200000) return PLAYGO_BAD_SIZE;
    if (playgo_initialized) return PLAYGO_ALREADY_INITIALIZED;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    playgo_initialized=1;
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    if (!handle) return PLAYGO_BAD_POINTER;
    if (param) return PLAYGO_INVALID_ARGUMENT;
    if (!playgo_initialized) return PLAYGO_NOT_INITIALIZED;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_todo(int32_t handle,void *todo_list,uint32_t count,uint32_t *out_entries) {
    if (handle!=playgo_handle) return PLAYGO_BAD_HANDLE;
    if (!todo_list || !out_entries) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    if (!playgo_initialized) return PLAYGO_NOT_INITIALIZED;
    /* The package is fully installed, so there are no chunks to schedule. */
    *out_entries=0;
    return ORBIS_OK;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
/* Install speed as shadPS4 keeps it: TRICKLE (1) until the game sets another; SUSPENDED (0)
 * goes back to TRICKLE after 30 s. Nothing is downloading, so the value is only reported back. */
static int32_t playgo_install_speed=1;
static uint64_t playgo_suspended_at;
static uint64_t playgo_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000; }
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!playgo_initialized) return PLAYGO_NOT_INITIALIZED;
    if (speed<0 || speed>2) return PLAYGO_INVALID_ARGUMENT;
    playgo_install_speed=speed;
    if (speed==0) playgo_suspended_at=playgo_now_ms();
    return 0;
}
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    if (!playgo_initialized) return PLAYGO_NOT_INITIALIZED;
    if (playgo_install_speed==0 && playgo_now_ms()-playgo_suspended_at>30000) playgo_install_speed=1;
    *speed=playgo_install_speed;
    return 0;
}
/* Every chunk is local, so nothing is left to install: ETA 0, as shadPS4. */
static ABI int32_t playgo_get_eta(int32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *eta=0;
    return 0;
}
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!playgo_initialized) return PLAYGO_NOT_INITIALIZED;
    return 0;
}
static ABI int32_t playgo_terminate(void) {
    if (!playgo_initialized) return PLAYGO_NOT_INITIALIZED;
    playgo_initialized=0; playgo_handle=0;
    return 0;
}

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserColor",user_color},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceGetDisplaySafeAreaInfo",system_display_safe_area},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceScreenShotSetOverlayImage",screenshot_set_overlay},
    {"sceScreenShotEnable",screenshot_enable}, {"sceScreenShotDisable",screenshot_disable},
    {"sceVideoRecordingSetInfo",video_recording_set_info},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceRudpInit",rudp_init}, {"sceRudpEnableInternalIOThread",rudp_enable_internal_io_thread},
    {"sceRudpEnd",rudp_end},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out}, {"sceNpGetNpId",np_signed_out},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",np_set_title_id}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",np_webapi_initialize}, {"sceNpWebApiTerminate",np_webapi_terminate},
    {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceGameLiveStreamingInitialize",ok_void},
    {"sceGameLiveStreamingPermitLiveStreaming",ok_void},
    {"sceGameLiveStreamingEnableLiveStreaming",ok_void},
    {"sceSharePlayInitialize",ok_void},
    {"sceSharePlaySetProhibition",ok_void},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    /* shadPS4's registered sceRemoteplayInitialize is a no-argument success stub. */
    {"sceRemoteplayInitialize",ok_void},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogGetStatus",msg_status}, {"sceMsgDialogGetResult",msg_get_result},
    {"sceErrorDialogInitialize",error_dialog_init}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status_get}, {"sceErrorDialogGetStatus",error_dialog_status_get},
    {"sceErrorDialogClose",error_dialog_close}, {"sceErrorDialogTerminate",error_dialog_term},
    {"sceMsgDialogClose",msg_close}, {"sceMsgDialogProgressBarInc",msg_progress_inc},
    {"sceMsgDialogProgressBarSetValue",msg_progress_value}, {"sceMsgDialogProgressBarSetMsg",msg_progress_msg},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_dialog_ready},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",save_progress_set},
    {"sceSaveDataDialogProgressBarInc",save_progress_inc},
    {"sceSaveDataDialogGetStatus",save_get_status},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyDestroyContext",trophy_destroy_context}, {"sceNpTrophyDestroyHandle",trophy_destroy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetToDoList",playgo_todo},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoGetEta",playgo_get_eta},
    {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_terminate},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};
uintptr_t runtime_services_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }

#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#ifdef _WIN32
#include <sys/utime.h>
#endif
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
int bbgpu_perf_start(int seconds) { (void)seconds; return 0; }
uint32_t runtime_save_dialog_input(uint32_t buttons, uint8_t left_y, int *open) {
    (void)buttons; (void)left_y; *open=0; return 0;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
#ifdef _WIN32
    /* read_inject sees whole seconds there: every write gets the next one. */
    static time_t stamp;
    if (!stamp) stamp=time(NULL);
    struct _utimbuf times={++stamp,stamp};
    assert(_utime(path,&times)==0);
#endif
    SDL_Delay(25);
}

/* A temporary file for the test: the game's data never comes near it. */
static void temp_file(char *path, size_t size, const char *name) {
#ifdef _WIN32
    char dir[MAX_PATH];
    assert(GetTempPathA(sizeof dir,dir));
    snprintf(path,size,"%sbbport-%s-%lu",dir,name,(unsigned long)GetCurrentProcessId());
    FILE *f=fopen(path,"w");
#else
    snprintf(path,size,"/tmp/bbport-%s-XXXXXX",name);
    int fd=mkstemp(path);
    assert(fd>=0);
    FILE *f=fdopen(fd,"w");
#endif
    assert(f);
    fclose(f);
}

/* The game's SHAKE update (eboot 0x760f60), level 0 of mg_bar_shakemini.bin: the count is what the
 * minigame asks of the player, 75 half strokes in 20. */
typedef struct { float previous, velocity, position; int low, count; } Shake;
static void shake_update(Shake *s, float accel_x) {
    const float x=-accel_x;
    s->velocity=0.85f*s->velocity+(x-s->previous);
    s->previous=x;
    s->position=0.9f*s->position+s->velocity*0.15f;
    if (s->position<-1.0f) { s->position=-1.0f; s->velocity=0; }
    else if (s->position>1.0f) { s->position=1.0f; s->velocity=0; }
    if (!s->low && s->position<-0.7f) { s->low=1; ++s->count; }
    else if (s->low && s->position>0.7f) { s->low=0; ++s->count; }
}
/* Half strokes in the level's 20 s, shaking with the runtime's oscillation or the stick. */
static int shake_count(int updates_per_second, float hz, float g) {
    Shake s={0};
    for (int i=0;i<20*updates_per_second;++i) shake_update(&s,shake_accel((double)i/updates_per_second,hz,g));
    return s.count;
}
static int stick_count(int updates_per_second, float hz, uint8_t reach) {
    Shake s={0};
    for (int i=0;i<20*updates_per_second;++i) {
        const int right=shake_accel((double)i/updates_per_second,hz,1.0f)>=0;
        shake_update(&s,stick_accel(right ? reach : (uint8_t)(255-reach)));
    }
    return s.count;
}
static bool SDLCALL sensors_enabled(void *userdata, bool enabled) { (void)userdata; (void)enabled; return true; }

int main(void) {
    char path[512];
    temp_file(path,sizeof path,"pad-test");
    setenv("PGG_PAD_FILE",path,1);
    /* pgg.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[512];
    temp_file(config,sizeof config,"pad-config");
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\n";
    FILE *config_file=fopen(config,"w");
    assert(config_file && fputs(controls,config_file)>=0);
    fclose(config_file);
    setenv("PGG_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);

    /* No sensor: the right stick Y is the acceleration, up is positive. */
    assert(!gamepad_has_motion);
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTY,-32768));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.acceleration[0]==STICK_ACCEL_G);
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTY,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.acceleration[0]==-STICK_ACCEL_G);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.acceleration[0]==0);
    capture=0;
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTY,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.acceleration[0]==0 && data.acceleration[1]==0 && data.orientation[3]==1.0f);

    /* The token shakes along X within its amplitude, and stops with the file. */
    inject(path,"shake=5,2.5");
    assert(pad_read_state(1,&data)==0);
    SDL_Delay(20);
    PadData later;
    assert(pad_read_state(1,&later)==0);
    assert(injected.shake_hz==5.0f && injected.shake_g==2.5f);
    assert(fabsf(data.acceleration[0])<=2.5f && fabsf(later.acceleration[0])<=2.5f && data.acceleration[0]!=later.acceleration[0]);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.acceleration[0]==0);

    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));

    /* A controller with sensors: acceleration in g, angular velocity as SDL gives it. */
    static const SDL_VirtualJoystickSensorDesc sensors[]={{SDL_SENSOR_ACCEL,250.0f},{SDL_SENSOR_GYRO,250.0f}};
    desc.ntouchpads=0;
    desc.touchpads=NULL;
    desc.nsensors=2;
    desc.sensors=sensors;
    desc.SetSensorsEnabled=sensors_enabled;
    id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(current_gamepad() && gamepad_has_motion);
    const float accel[3]={SDL_STANDARD_GRAVITY*0.5f,SDL_STANDARD_GRAVITY,-SDL_STANDARD_GRAVITY*2.0f}, gyro[3]={0.25f,-0.5f,1.5f};
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_ACCEL,1000,accel,3));
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,1000,gyro,3));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTY,-32768));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(fabsf(data.acceleration[0]-0.5f)<1e-5f && fabsf(data.acceleration[1]-1.0f)<1e-5f && fabsf(data.acceleration[2]+2.0f)<1e-5f);
    assert(data.angular_velocity[0]==0.25f && data.angular_velocity[1]==-0.5f && data.angular_velocity[2]==1.5f);
    /* PGG_MOTION=off: the sensor is ignored, the stick simulates it. */
    SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    setenv("PGG_MOTION","off",1);
    assert(current_gamepad() && !gamepad_has_motion);
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.acceleration[0]==STICK_ACCEL_G && data.angular_velocity[0]==0);
    SDL_CloseJoystick(joystick);
    SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));

    /* What SHAKE asks, with the level's parameters. */
    for (int rate=30;rate<=60;rate+=30) {
        assert(shake_count(rate,0.0f,0.0f)==0);
        assert(shake_count(rate,2.0f,0.5f)==0);          /* too gentle: nothing counts */
        assert(shake_count(rate,2.0f,3.0f)>=75);         /* the level's norm with the token */
        assert(stick_count(rate,2.0f,255)>=75);          /* and with the stick all the way */
        assert(stick_count(rate,2.0f,128)==0);           /* a stick at rest counts nothing */
    }
    assert(stick_count(30,4.0f,255)>=150);               /* the fastest the stick has to go */

    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, motion");
}

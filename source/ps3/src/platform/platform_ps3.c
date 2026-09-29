/**
* platform_ps3.c — PS3 platform implementation & stubs
*
* Implements platform API, audio stubs, CD audio stubs, and POSIX/sys helper stubs
* when building for PS3.
*/

#if defined(SONICR_PS3)

#include <PSGL/psgl.h>
#include <PSGL/psglu.h>
#include <cell/sysmodule.h>
#include <sysutil/sysutil_common.h>
#include <cell/sysmodule.h>
#include <cell/pad.h>
#include <sys/process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/sys_time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "platform.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "pad_bits.h"
#include "gamepad_buttons.h"

#ifndef CELL_SYSMODULE_PAD
#define CELL_SYSMODULE_PAD 0x000000000000000eULL
#endif

#ifndef CELL_PAD_OK
#define CELL_PAD_OK 0
#endif

#ifndef CELL_PAD_MAX_PORT_NUM
#define CELL_PAD_MAX_PORT_NUM 7
#endif

#ifndef CELL_PAD_BTN_OFFSET_DIGITAL1
#define CELL_PAD_BTN_OFFSET_DIGITAL1 2
#endif

#ifndef CELL_PAD_BTN_OFFSET_DIGITAL2
#define CELL_PAD_BTN_OFFSET_DIGITAL2 3
#endif

#ifndef CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X 6
#endif

#ifndef CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y
#define CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y 7
#endif

#ifndef CELL_PAD_CTRL_SELECT
#define CELL_PAD_CTRL_SELECT   (1<<0)
#define CELL_PAD_CTRL_L3       (1<<1)
#define CELL_PAD_CTRL_R3       (1<<2)
#define CELL_PAD_CTRL_START    (1<<3)
#define CELL_PAD_CTRL_UP       (1<<4)
#define CELL_PAD_CTRL_RIGHT    (1<<5)
#define CELL_PAD_CTRL_DOWN     (1<<6)
#define CELL_PAD_CTRL_LEFT     (1<<7)
#endif

#ifndef CELL_PAD_CTRL_L2
#define CELL_PAD_CTRL_L2       (1<<0)
#define CELL_PAD_CTRL_R2       (1<<1)
#define CELL_PAD_CTRL_L1       (1<<2)
#define CELL_PAD_CTRL_R1       (1<<3)
#define CELL_PAD_CTRL_TRIANGLE (1<<4)
#define CELL_PAD_CTRL_CIRCLE   (1<<5)
#define CELL_PAD_CTRL_CROSS    (1<<6)
#define CELL_PAD_CTRL_SQUARE   (1<<7)
#endif

#define MAX_GAMEPADS 4
#define JOY_BUTTONS_PER_SLOT 80
#define JOY_CFG_MAX 32

extern unsigned char g_keyPressState[320];
extern short g_joystickConfigWords[];
extern char g_joystickSlots[4][282];
extern char g_joystickDeviceNames[4][260];
extern short g_joystickDeviceFlags[8];
extern int g_initFeatureC;
extern void SyncJoystickSlots(void);

static int s_ps3PadsInitialized = 0;
static CellPadData s_lastPadData[MAX_GAMEPADS];

static GLuint s_ps3GlWidth = 640;
static GLuint s_ps3GlHeight = 480;

unsigned char s_keystate[256];

/* =====================================================================
* Platform API Implementation for PS3
* ===================================================================== */

#ifndef CELL_SYSUTIL_REQUEST_EXITGAME
#define CELL_SYSUTIL_REQUEST_EXITGAME 0x0101
#endif

static void ps3_sysutil_callback(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME || status == 0x0101) {
		extern void platform_audio_shutdown(void);
		platform_audio_shutdown();
		platform_shutdown();
		sys_process_exit(0);
	}
}

int platform_init(int width, int height, int fullscreen, const char *title)
{
	(void)width;
	(void)height;
	(void)fullscreen;
	(void)title;
	memset(s_keystate, 0, sizeof(s_keystate));

	cellSysutilRegisterCallback(0, ps3_sysutil_callback, NULL);

	/* Load system modules required by PSGL */
	cellSysmoduleLoadModule(CELL_SYSMODULE_GCM_SYS);

	PSGLinitOptions initOpts;
	memset(&initOpts, 0, sizeof(PSGLinitOptions));
	initOpts.enable = PSGL_INIT_MAX_SPUS | PSGL_INIT_INITIALIZE_SPUS | PSGL_INIT_HOST_MEMORY_SIZE;
	initOpts.maxSPUs = 1;
	initOpts.initializeSPUs = 1;
	initOpts.persistentMemorySize = 0;
	initOpts.transientMemorySize = 0;
	initOpts.errorConsole = 0;
	initOpts.fifoSize = 0;
	initOpts.hostMemorySize = 32 * 1024 * 1024;

	psglInit(&initOpts);

	PSGLdeviceParameters params;
	memset(&params, 0, sizeof(PSGLdeviceParameters));
	params.enable = PSGL_DEVICE_PARAMETERS_COLOR_FORMAT |
		PSGL_DEVICE_PARAMETERS_DEPTH_FORMAT |
		PSGL_DEVICE_PARAMETERS_MULTISAMPLING_MODE |
		PSGL_DEVICE_PARAMETERS_BUFFERING_MODE |
		PSGL_DEVICE_PARAMETERS_RESC_ADJUST_ASPECT_RATIO |
		PSGL_DEVICE_PARAMETERS_RESC_RATIO_MODE;
	params.bufferingMode = PSGL_BUFFERING_MODE_TRIPLE;
	params.colorFormat = GL_ARGB_SCE;
	params.depthFormat = GL_DEPTH_COMPONENT24;
	params.multisamplingMode = GL_MULTISAMPLING_NONE_SCE;
	params.rescRatioMode = RESC_RATIO_MODE_FULLSCREEN;

	PSGLdevice *device = psglCreateDeviceExtended(&params);
	if (!device) {
		params.depthFormat = GL_NONE;
		device = psglCreateDeviceExtended(&params);
	}
	if (!device) {
		return -1;
	}

	GLuint glWidth = 0, glHeight = 0;
	psglGetDeviceDimensions(device, &glWidth, &glHeight);
	if (glWidth > 0 && glHeight > 0) {
		s_ps3GlWidth = glWidth;
		s_ps3GlHeight = glHeight;
	}

	PSGLcontext *context = psglCreateContext();
	if (!context) {
		return -1;
	}

	psglMakeCurrent(context, device);
	psglResetCurrentContext();

	return 0;
}

void platform_shutdown(void)
{
	cellSysutilUnregisterCallback(0);

	if (s_ps3PadsInitialized) {
		cellPadEnd();
		cellSysmoduleUnloadModule(CELL_SYSMODULE_PAD);
		s_ps3PadsInitialized = 0;
		memset(s_lastPadData, 0, sizeof(s_lastPadData));
	}

	PSGLcontext *context = psglGetCurrentContext();
	PSGLdevice *device = psglGetCurrentDevice();
	if (context) {
		psglDestroyContext(context);
	}
	if (device) {
		psglDestroyDevice(device);
	}
	psglExit();
}

static char s_ps3ExeDir[1024] = "";

extern int g_optCfg_470;

static const char *s_wideTargets[] = {
	"THE_END.RAW",
	"EMERALDS.RAW",
	"SONIC.RAW",
	"TAILS.RAW",
	"KNUCKLES.RAW",
	"AMY.RAW",
	"ROBOTNIK.RAW",
	"MSONIC.RAW",
	"DTAILS.RAW",
	"MKNUCK.RAW",
	"MROBOT.RAW",
	"SSONIC.RAW",
	"SMODE00.RAW",
	"STAMOD00.RAW",
	"SMPMOD00.RAW",
	"SCHAR00.RAW",
	"SCRSE00.RAW",
	"OPT00.RAW",
	"MP00.RAW",
	"LOAD00.RAW",
	"SEGALOGO.RAW",
	"TTLOGO.RAW",
	"TITLES.RAW"
};

static int path_stricmp(const char *s1, const char *s2)
{
	while (*s1 && *s2) {
		char c1 = (*s1 >= 'a' && *s1 <= 'z') ? (*s1 - 32) : *s1;
		char c2 = (*s2 >= 'a' && *s2 <= 'z') ? (*s2 - 32) : *s2;
		if (c1 != c2) return c1 - c2;
		s1++;
		s2++;
	}
	return (unsigned char)*s1 - (unsigned char)*s2;
}

static int path_strnicmp(const char *s1, const char *s2, size_t n)
{
	while (n > 0 && *s1 && *s2) {
		char c1 = (*s1 >= 'a' && *s1 <= 'z') ? (*s1 - 32) : *s1;
		char c2 = (*s2 >= 'a' && *s2 <= 'z') ? (*s2 - 32) : *s2;
		if (c1 != c2) return c1 - c2;
		s1++;
		s2++;
		n--;
	}
	if (n == 0) return 0;
	return (unsigned char)*s1 - (unsigned char)*s2;
}

static int GetWidescreenPath(const char *path, char *buf, size_t bufSize)
{
	if (path == NULL || buf == NULL || bufSize == 0) return 0;

	const char *slash = strrchr(path, '/');
	const char *bslash = strrchr(path, '\\');
	if (bslash > slash) slash = bslash;

	const char *filename = (slash != NULL) ? (slash + 1) : path;
	size_t dirLen = (size_t)(filename - path);

	if (path_strnicmp(filename, "WIDE_", 5) == 0) return 0;

	int matched = 0;
	size_t targetCount = sizeof(s_wideTargets) / sizeof(s_wideTargets[0]);
	for (size_t i = 0; i < targetCount; i++) {
		if (path_stricmp(filename, s_wideTargets[i]) == 0) {
			matched = 1;
			break;
		}
	}

	if (!matched) return 0;

	snprintf(buf, bufSize, "%.*sWIDE_%s", (int)dirLen, path, filename);
	return 1;
}

void ps3_set_exe_path(const char *argv0)
{
	if (argv0 == NULL || argv0[0] == '\0') return;
	strncpy(s_ps3ExeDir, argv0, sizeof(s_ps3ExeDir)-1);
	s_ps3ExeDir[sizeof(s_ps3ExeDir)-1] = '\0';

	char *lastSlash = strrchr(s_ps3ExeDir, '/');
	char *lastBackslash = strrchr(s_ps3ExeDir, '\\');
	if (lastBackslash > lastSlash) lastSlash = lastBackslash;
	if (lastSlash != NULL) {
		*(lastSlash + 1) = '\0';
	}
	else {
		s_ps3ExeDir[0] = '\0';
	}
}

FILE *ps3_fOpen(const char *path, const char *mode)
{
	if (path == NULL) return NULL;

	if (g_optCfg_470 != 0) {
		char widePath[1024];
		if (GetWidescreenPath(path, widePath, sizeof(widePath))) {
			g_optCfg_470 = 0;
			FILE *fpWide = ps3_fOpen(widePath, mode);
			g_optCfg_470 = 1;
			if (fpWide != NULL) {
				return fpWide;
			}
		}
	}

	if (path[0] == '/' || path[0] == '\\') {
		FILE *fp = fopen(path, mode);
		return fp;
	}

	const char *relPath = path;
	if (relPath[0] == '.' && (relPath[1] == '/' || relPath[1] == '\\')) {
		relPath += 2;
	}

	char fullPath[1024];

	/* 1. Dynamically walk up parent directories from EXE path */
	if (s_ps3ExeDir[0] != '\0') {
		char currentLevel[1024];
		strncpy(currentLevel, s_ps3ExeDir, sizeof(currentLevel)-1);
		currentLevel[sizeof(currentLevel)-1] = '\0';

		for (int depth = 0; depth <= 5; depth++) {
			snprintf(fullPath, sizeof(fullPath), "%s%s", currentLevel, relPath);
			FILE *fp = fopen(fullPath, mode);
			if (fp != NULL) return fp;

			snprintf(fullPath, sizeof(fullPath), "%sDATA/%s", currentLevel, relPath);
			fp = fopen(fullPath, mode);
			if (fp != NULL) return fp;

			size_t len = strlen(currentLevel);
			if (len <= 1) break;
			if (currentLevel[len - 1] == '/' || currentLevel[len - 1] == '\\') {
				currentLevel[len - 1] = '\0';
			}
			char *slash = strrchr(currentLevel, '/');
			char *bslash = strrchr(currentLevel, '\\');
			if (bslash > slash) slash = bslash;
			if (slash != NULL) {
				*(slash + 1) = '\0';
			}
			else {
				break;
			}
		}
	}

	/* 2. Static fallback paths */
	static const char *staticPrefixes[] = {
		"/dev_hdd0/game/SONYCR026/USRDIR/DATA/",
		NULL
	};

	for (int i = 0; staticPrefixes[i] != NULL; i++) {
		snprintf(fullPath, sizeof(fullPath), "%s%s", staticPrefixes[i], relPath);
		FILE *fp = fopen(fullPath, mode);
		if (fp != NULL) return fp;
	}

	return NULL;
}

const char *platform_base_path(void)
{
	return "/app_home/";
}

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
	if (keystateOut && keystateSize > 0) {
		int copySize = keystateSize < 256 ? keystateSize : 256;
		memcpy(keystateOut, s_keystate, copySize);
	}
	return 0;
}

void platform_pump_events(void)
{
	cellSysutilCheckCallback();
}

static void platform_publish_joystick_name(int slot, const char *name)
{
	if (slot < 0 || slot >= 4) {
		return;
	}
	char *dst = g_joystickDeviceNames[slot];
	if (name == NULL) {
		name = "PS3 Controller";
	}
	size_t n = strlen(name);
	if (n > 258) {
		n = 258;
	}
	memcpy(dst, name, n);
	dst[n] = '\0';
}

int platform_init_gamepads(void)
{
	if (!s_ps3PadsInitialized) {
		cellSysmoduleLoadModule(CELL_SYSMODULE_PAD);
		int ret = cellPadInit(CELL_PAD_MAX_PORT_NUM);
		if (ret == CELL_PAD_OK) {
			s_ps3PadsInitialized = 1;
			memset(s_lastPadData, 0, sizeof(s_lastPadData));
		}
		else {
		}
	}

	int count = 0;
	if (s_ps3PadsInitialized) {
		for (int i = 0; i < MAX_GAMEPADS; i++) {
			CellPadData padData;
			memset(&padData, 0, sizeof(CellPadData));
			if (cellPadGetData(i, &padData) == CELL_PAD_OK && padData.len > 0) {
				s_lastPadData[i] = padData;
			}
			if (s_lastPadData[i].len > 0) {
				platform_publish_joystick_name(i, "PlayStation(R)3 Controller");
				g_joystickDeviceFlags[i] = GC_BUTTON_COUNT < JOY_SLOT_CFG_WORDS
					? GC_BUTTON_COUNT : JOY_SLOT_CFG_WORDS;
				count++;
			}
		}
	}

	g_initFeatureC = count;
	SyncJoystickSlots();
	return count;
}

static int gamepad_button_held_ps3(uint16_t digital1, uint16_t digital2, int b)
{
	switch (b) {
	case 0: return (digital2 & CELL_PAD_CTRL_CROSS) != 0;
	case 1: return (digital2 & CELL_PAD_CTRL_CIRCLE) != 0;
	case 2: return (digital2 & CELL_PAD_CTRL_SQUARE) != 0;
	case 3: return (digital2 & CELL_PAD_CTRL_TRIANGLE) != 0;
	case 4: return (digital2 & CELL_PAD_CTRL_L1) != 0;
	case 5: return (digital2 & CELL_PAD_CTRL_R1) != 0;
	case 6: return (digital2 & CELL_PAD_CTRL_L2) != 0;
	case 7: return (digital2 & CELL_PAD_CTRL_R2) != 0;
	case 8: return (digital1 & CELL_PAD_CTRL_START) != 0;
	case 9: return (digital1 & CELL_PAD_CTRL_SELECT) != 0;
	default: return 0;
	}
}

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
	if (!s_ps3PadsInitialized) {
		platform_init_gamepads();
	}

	int activeCount = 0;
	int slotsToPoll = (maxSlots < MAX_GAMEPADS) ? maxSlots : MAX_GAMEPADS;

	for (int i = 0; i < slotsToPoll; i++) {
		unsigned char *pressBase = &g_keyPressState[i * JOY_BUTTONS_PER_SLOT];

		if (!s_ps3PadsInitialized) {
			if (joySlotState) joySlotState[i] = 0;
			memset(pressBase, 0, JOY_BUTTONS_PER_SLOT);
			continue;
		}

		CellPadData padData;
		memset(&padData, 0, sizeof(CellPadData));
		if (cellPadGetData(i, &padData) == CELL_PAD_OK && padData.len > 0) {
			s_lastPadData[i] = padData;
		}

		if (s_lastPadData[i].len == 0) {
			if (joySlotState) joySlotState[i] = 0;
			memset(pressBase, 0, JOY_BUTTONS_PER_SLOT);
			continue;
		}

		activeCount++;
		platform_publish_joystick_name(i, "PlayStation(R)3 Controller");
		g_joystickDeviceFlags[i] = 10;

		uint16_t digital1 = (uint16_t)s_lastPadData[i].button[CELL_PAD_BTN_OFFSET_DIGITAL1];
		uint16_t digital2 = (uint16_t)s_lastPadData[i].button[CELL_PAD_BTN_OFFSET_DIGITAL2];

		unsigned short bits = 0;

		/* Left analog stick deflection */
		int stickX = (int)(s_lastPadData[i].button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] & 0xFF) - 128;
		int stickY = (int)(s_lastPadData[i].button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] & 0xFF) - 128;

		if (stickX < -50) {
			bits |= PAD_LEFT;
		}
		if (stickX > 50) {
			bits |= PAD_RIGHT;
		}
		if (stickY < -50) {
			bits |= PAD_UP;
		}
		if (stickY > 50) {
			bits |= PAD_DOWN;
		}

		/* Digital D-Pad directions */
		if (digital1 & CELL_PAD_CTRL_LEFT)  bits |= PAD_LEFT;
		if (digital1 & CELL_PAD_CTRL_RIGHT) bits |= PAD_RIGHT;
		if (digital1 & CELL_PAD_CTRL_UP)    bits |= PAD_UP;
		if (digital1 & CELL_PAD_CTRL_DOWN)  bits |= PAD_DOWN;

		/* System buttons */
		if (digital1 & CELL_PAD_CTRL_START)  bits |= PAD_START;
		if (digital1 & CELL_PAD_CTRL_SELECT) bits |= PAD_ACCEL;

		/* Face buttons & shoulders */
		if (digital2 & CELL_PAD_CTRL_CROSS)    bits |= (PAD_JUMP | 0x0200);   /* 0x0600: Jump + Confirm bit */
		if (digital2 & CELL_PAD_CTRL_CIRCLE)   bits |= PAD_ACCEL;             /* 0x0100: Accel + Back bit */
		if (digital2 & CELL_PAD_CTRL_SQUARE)   bits |= (PAD_JUMP | PAD_ACCEL);/* 0x0500: Jump + Accel */
		if (digital2 & CELL_PAD_CTRL_TRIANGLE) bits |= PAD_CAMERA;            /* 0x0040: Camera */
		if (digital2 & CELL_PAD_CTRL_L1)       bits |= PAD_DRIFTL;
		if (digital2 & CELL_PAD_CTRL_R1)       bits |= PAD_DRIFTR;
		if (digital2 & CELL_PAD_CTRL_L2)       bits |= PAD_DRIFTL;
		if (digital2 & CELL_PAD_CTRL_R2)       bits |= PAD_DRIFTR;

		/* Buttons mapping & key press state */
		const short *slotCfg = (const short *)&g_joystickSlots[i][0x104];
		for (int b = 0; b < 10; b++) {
			int held = gamepad_button_held_ps3(digital1, digital2, b);
			pressBase[b] = held ? 0x80 : 0x00;
			if (!held) continue;

			short cfg = 0;
			if (b < JOY_SLOT_CFG_WORDS && slotCfg[b] != 0) {
				cfg = slotCfg[b];
			}
			else if (b < JOY_CFG_MAX) {
				cfg = g_joystickConfigWords[b];
			}
			bits |= (unsigned short)cfg;
		}

		for (int b = 10; b < JOY_BUTTONS_PER_SLOT; b++) {
			pressBase[b] = 0x00;
		}

		if (joySlotState) {
			joySlotState[i] = bits;
		}
	}

	if (joySlotState) {
		for (int i = slotsToPoll; i < maxSlots; i++) {
			joySlotState[i] = 0;
		}
	}

	for (int i = slotsToPoll; i < MAX_GAMEPADS; i++) {
		memset(&g_keyPressState[i * JOY_BUTTONS_PER_SLOT], 0, JOY_BUTTONS_PER_SLOT);
	}

	g_initFeatureC = activeCount;
	return activeCount;
}

uint32_t platform_get_time_ms(void)
{
	static uint64_t start_us = 0;
	uint64_t current_us = sys_time_get_system_time();
	if (start_us == 0) {
		start_us = current_us;
	}
	return (uint32_t)((current_us - start_us) / 1000);
}

void platform_sleep_ms(int ms)
{
	(void)ms;
}

void platform_gl_swap(void)
{
	psglSwap();
}

void platform_get_drawable_size(int *w, int *h)
{
	if (w) *w = (s_ps3GlWidth > 0) ? (int)s_ps3GlWidth : 640;
	if (h) *h = (s_ps3GlHeight > 0) ? (int)s_ps3GlHeight : 480;
}

int platform_net_init(void)
{
	return 0;
}

void platform_net_shutdown(void)
{
}

int platform_net_is_modem(void)
{
	return 0;
}

int platform_get_region(void)
{
	return 0;
}

/* =====================================================================
* POSIX / Pthreads Stubs for PS3
* ===================================================================== */

int pthread_create(void *thread, const void *attr, void *(*start_routine)(void *), void *arg)
{
	(void)thread; (void)attr; (void)start_routine; (void)arg;
	return -1;
}

int pthread_join(void *thread, void **retval)
{
	(void)thread; (void)retval;
	return 0;
}

int pthread_mutex_lock(void *mutex)
{
	(void)mutex;
	return 0;
}

int pthread_mutex_unlock(void *mutex)
{
	(void)mutex;
	return 0;
}

int usleep(unsigned int usec)
{
	(void)usec;
	return 0;
}

char *getenv(const char *name)
{
	(void)name;
	return NULL;
}

#endif /* SONICR_PS3 */

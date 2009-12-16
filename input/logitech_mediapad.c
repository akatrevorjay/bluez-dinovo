/************************ Logitech Mediapad Driver ********************************
 *     (C) 2006-2009 Tim Hentenaar <tim@hentenaar.com>                            *
 *     Licensed under the GNU General Public License (v2).                        *
 *     For more information, see http://hentenaar.com                             *
 *                                                                                *
 *     12/16/09 thentenaar:                                                       *
 *               * Added new DBus method: BindKey.                                *
 *               * Added keymap tables, updated default keysyms.                  *
 *               * Massively cleaned up the code.                                 *
 *               * Integrated atomic ops from Glen Rolle's 3.36 patch.            *
 *               * Rewrote DBus code to use gdbus.                                *
 *               * Ported mediapad driver up to master.                           *
 *               * Forked bluez git.                                              *
 *                                                                                *
 *     Notes:                                                                     *
 *		1) The i18n for the device isn't currently supported.             *
 *			The way that the i18n works, is that when the device      *
 *			connects, the Winblows app retrieves the respective       *
 *			strings from the device and verifies/updates them.        *
 *                                                                                *
 *			Simple enough to do, but I'll worry about it later.       *
 *		2) The '000' key actually sends 3 0's and is not a special key.   *
 *		3) The "Copy calulator result to clipboard" requires an           *
 *		   activation packet that I haven't isolated to date.             *
 *      4) Git-R-Done!                                                            *
 **********************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <glib.h>
#include <syslog.h>
#include <gdbus.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fakehid.h"
#include "uinput.h"
#include "logging.h"

/* Screen modes */
#define LCD_SCREEN_TEXT   0x00
#define LCD_SCREEN_CLOCK  0x01

/* Display modes */
#define LCD_DISP_MODE_INIT    0x01 /* Initialize the line */
#define LCD_DISP_MODE_BUF1    0x10 /* Display the first 16 chars of the line */
#define LCD_DISP_MODE_BUF2    0x11 /* ... 2nd 16 */
#define LCD_DISP_MODE_BUF3    0x12 /* ... 3rd 16 */
#define LCD_DISP_MODE_SCROLL  0x20 /* Scroll char-by-char */
#define LCD_DISP_MODE_SCROLL2 0x02 /* ... by 16-chars (or'd in) */
#define LCD_DISP_MODE_SCROLL3 0x03 /* ... by 32-chars (or'd in) */

/* Icons */
#define LCD_ICON_EMAIL	0x01
#define LCD_ICON_IM		0x02
#define LCD_ICON_MUTE	0x04
#define LCD_ICON_ALERT	0x08
#define LCD_ICON_ALL    0x0f

/* Icon states */
#define LCD_ICON_OFF    0x00
#define LCD_ICON_ON		0x01
#define LCD_ICON_BLINK	0x02

/* Speaker / LED */
#define LCD_LOW_BEEP	0x01
#define LCD_LONG_BEEP	0x02
#define LCD_SHORT_BEEP	0x03

/* Keypad Modes */
#define MODE_NUM		0x00
#define MODE_NAV		0x01

/* DBus Paths */
#define MP_DBUS_INTF	"com.hentenaar.Dinovo.MediaPad"
#define MP_DBUS_PATH	"/com/hentenaar/Dinovo/MediaPad"

/* Lengths */
#define LCD_BUF_LEN		16
#define LCD_LINE_LEN	(LCD_BUF_LEN*3)

/* Mediapad State */
struct mp_state {
	int mode;
	int discard_keyup;
	int prev_key;
	int icons;
	int uinput;
	int sock;
	DBusConnection *db_conn;
};

/* Mediapad Command */
struct mpcmd {	
	char    command[22];
	uint8_t len;
};

struct mpcmd screen_mode = { /* 0 = text, 1 =  clock */
	{ 0xA2, 0x10, 0x00, 0x80, 0x10, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd screen_start = { /* Signals the start of a screen write operation */
	{ 0xA2, 0x10, 0x00, 0x81, 0x10, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd screen_finish = { /* Signals the end of a screen write operation */
	{ 0xA2, 0x10, 0x00, 0x83, 0x11, 0x00, 0x00, 0x00 }, 8
};

struct mpcmd display_mode = { /* Set the display mode of a line */
	{ 0xA2, 0x10, 0x00, 0x80, 0x12, 0x00, 0x00, 0x00 }, 8
};

static struct mpcmd set_icons = { /* Set Icons (0 = off) */
	{ 0xA2, 0x11, 0x00, 0x82, 0x11, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 21
};

static struct mpcmd set_text_buffer = { /* Write a single buffer to the LCD */
	{ 0xA2, 0x11, 0x00, 0x82, 0x20, 0x20, 0x20, 0x20, 0x20, 
	  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 }, 21
};

static struct mpcmd set_ledspk[] = { /* LED / Speaker Control */
	{{ 0xA2, 0x10, 0x00, 0x81, 0x50, 0x00, 0x00, 0x00 }, 8},
	{{ 0xA2, 0x10, 0x00, 0x80, 0x50, 0x00, 0x00, 0x00 }, 8},
	{{ 0 }, 0}
};

static struct mpcmd setclk[] = { /* Set the clock */ 
	{{ 0xA2, 0x10, 0x00, 0x80, 0x31, 0x00, 0x00, 0x00 }, 8},
	{{ 0xA2, 0x10, 0x00, 0x80, 0x32, 0x02, 0x00, 0x00 }, 8},
	{{ 0xA2, 0x10, 0x00, 0x80, 0x33, 0x00, 0x00, 0x00 }, 8},
	{{ 0 }, 0}
};

/**
 * Mediapad Keymap (non-media)
 */
static uint8_t mp_keymap[2][16] = {
	/* Numeric mode */
	{ 
		KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER,
		KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
		KEY_7, KEY_8, KEY_9, KEY_0, KEY_DOT
	},

	/* Nav mode */
	{
		KEY_KPSLASH, KEY_KPASTERISK, KEY_KPMINUS, KEY_KPPLUS, KEY_KPENTER,
		KEY_OPEN, KEY_LEFTMETA, KEY_UNDO, KEY_LEFT, KEY_DOWN, KEY_RIGHT,
		KEY_BACK, KEY_UP, KEY_FORWARD, KEY_0, KEY_DOT
	}
};

/**
 * Mediapad Keymap (media)
 */
static uint8_t mp_keymap_m[2][8] = {
	/* Numeric mode */
	{ 
		KEY_MEDIA, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_STOP,
		KEY_PLAYPAUSE, KEY_MUTE,  KEY_VOLUMEUP, KEY_VOLUMEDOWN
	},

	/* Nav mode */
	{
		KEY_MEDIA, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_STOP,
		KEY_PLAYPAUSE, KEY_MUTE,  KEY_VOLUMEUP, KEY_VOLUMEDOWN
	}
};

/* Media key scancodes */
#define MP_KEY_MEDIA    0x83
#define MP_KEY_FFWD		0xb5
#define MP_KEY_REW		0xb6
#define MP_KEY_STOP		0xb7
#define MP_KEY_PLAY		0xcd
#define MP_KEY_MUTE		0xe2
#define MP_KEY_VOLUP	0xe9
#define MP_KEY_VOLDOWN	0xea

#define inject_key(X,Y,Z)         { send_event(X,EV_KEY,Y,Z); send_event(X,EV_SYN,SYN_REPORT,0); }
#define do_write(X,Y,Z)           if (write(X,Y,Z)) {};
#define mp_lcd_write_start(sock)  write_mpcmd(sock,screen_start)
#define mp_lcd_write_finish(sock) write_mpcmd(sock,screen_finish)

/* Forward declarations to satisfy warnings... */
int logitech_mediapad_setup_uinput(struct fake_input *fake_input, struct fake_hid *fake_hid);
gboolean logitech_mediapad_event(GIOChannel *chan, GIOCondition cond, gpointer data);

/* This is easier than including device.h, etc. */
struct fake_input {
	int		flags;
	GIOChannel	*io;
	int		uinput;		/* uinput socket */
	int		rfcomm;		/* RFCOMM socket */
	uint8_t		ch;		/* RFCOMM channel number */
	gpointer connect;
	gpointer disconnect;
	void		*priv;
};

/**
 * Send a uinput event
 */
static void send_event(int fd, uint16_t type, uint16_t code, int32_t value) {
	struct uinput_event event;

	memset(&event,0,sizeof(event));
	event.type	= type;
	event.code	= code;
	event.value	= value;
	gettimeofday(&event.time,NULL);
	do_write(fd,&event,sizeof(event));
}

/**
 * Translate a key scancode to a uinput key identifier 
 */
static uint8_t translate_key(int mode, int key) {
	/* Media keys */
	if ((key & 0xff) > 0x82) {
		switch (key & 0xff) {
			case MP_KEY_MEDIA:   return mp_keymap_m[mode ? 1 : 0][0];
			case MP_KEY_FFWD:    return mp_keymap_m[mode ? 1 : 0][1];
			case MP_KEY_STOP:    return mp_keymap_m[mode ? 1 : 0][2];
			case MP_KEY_PLAY:    return mp_keymap_m[mode ? 1 : 0][3];
			case MP_KEY_MUTE:    return mp_keymap_m[mode ? 1 : 0][4];
			case MP_KEY_VOLUP:   return mp_keymap_m[mode ? 1 : 0][5];
			case MP_KEY_VOLDOWN: return mp_keymap_m[mode ? 1 : 0][6];
		}
	} 
	
	/* Non-media keys */
	if (key > 0x63) return KEY_UNKNOWN;
	return mp_keymap[mode ? 1 : 0][key-0x54];
}

/**
 * Write a command to the mediapad 
 */
static void write_mpcmd(int sock, struct mpcmd command) {
	if (sock < 4) return;
	do_write(sock,command.command,command.len);
}

/*
 * Set LCD mode
 */
static void mp_lcd_set_mode(int sock, uint8_t mode) {
	screen_mode.command[6] = (char)mode;
	write_mpcmd(sock,screen_mode);
}

/**
 * Set display mode
 */
static void mp_lcd_set_display_mode(int sock,uint8_t mode1, uint8_t mode2, uint8_t mode3) {
	display_mode.command[5] = mode1;
	display_mode.command[6] = mode2;
	display_mode.command[7] = mode3;
	write_mpcmd(sock,display_mode);
}

/**
 * Set the status of one or more indicators
 */
static void mp_lcd_set_indicator(int sock, uint8_t indicator, uint8_t blink) {
	uint8_t mode = (blink >= 1) ? ((blink == 2) ? LCD_ICON_BLINK : LCD_ICON_ON) : 0; 
	uint8_t sel = 5;

	if (sock < 4 || indicator == 0) return;
	while (!(indicator & 1)) { sel++; indicator >>= 1; }
	while (indicator & 1) { set_icons.command[sel++] = mode; indicator >>= 1; }
	write_mpcmd(sock,set_icons);
}


/**
 * Clear the screen
 */
static void mp_lcd_clear(int sock) {
	mp_lcd_set_mode(sock,LCD_SCREEN_CLOCK);
	mp_lcd_write_start(sock);
	mp_lcd_set_indicator(sock,LCD_ICON_ALL,LCD_ICON_OFF);
	mp_lcd_write_finish(sock);
}

/**
 * Manipulate the speaker / LED 
 */
static void mp_blink_or_beep(int sock, uint8_t beep, uint8_t blink) {
	int i = 0;

	if (beep)  set_ledspk[1].command[5] = (beep & 3);
	if (blink) set_ledspk[1].command[6] = 1;
	while (set_ledspk[i].len != 0) { write_mpcmd(sock,set_ledspk[i]); i++; }
}

/**
 * Set the Mediapad's clock
 */
static void mp_set_clock(int sock) {
	struct tm tx; time_t tim = 0; int i = 0;

	if (sock < 4) return;
	time(&tim); localtime_r(&tim,&tx);
	setclk[0].command[5] = (char)(tx.tm_sec);
	setclk[0].command[6] = (char)(tx.tm_min);
	setclk[0].command[7] = (char)(tx.tm_hour);
	setclk[1].command[6] = (char)(tx.tm_mday);
	setclk[1].command[7] = (char)(tx.tm_mon);
	setclk[2].command[5] = (char)(tx.tm_year - 100);
	
	while (setclk[i].len != 0) { write_mpcmd(sock,setclk[i]); i++; }
}

/**
 * Write a single buffer of text to the LCD (<= 16 chars.)
 */
static void mp_lcd_write_buffer(int sock, char *text, uint8_t bufno) {
	if (!text || sock < 4 || bufno > 9) return;
	set_text_buffer.command[4] = 0x20 + bufno - 1;
	memcpy(&set_text_buffer.command+5,text,(strlen(text) > LCD_BUF_LEN) ? LCD_BUF_LEN : strlen(text));
	write_mpcmd(sock,set_text_buffer);
}

/**
 * Write a single line of text to the LCD (<= 48 chars.)
 */
static void mp_lcd_write_line(int sock, char *text, uint8_t lineno) {
	char line[LCD_LINE_LEN]; uint32_t i = 0,z = 0; uint8_t f = LCD_DISP_MODE_BUF1;

	if (!text || sock < 4) return;
	lineno = (lineno > 3) ? 3 : (!lineno) ? 1 : lineno;
	z      = (strlen(text) > LCD_LINE_LEN) ? LCD_LINE_LEN : strlen(text);

	/* Copy the line text */
	memset(line,0x20,LCD_LINE_LEN);
	memcpy(line,text,z);

	/* Adjust flags for autoscrolling */
	if (z > LCD_BUF_LEN) {
		f |= LCD_DISP_MODE_SCROLL | LCD_DISP_MODE_SCROLL2;
		if (z > LCD_BUF_LEN*2) f++;
	}

	/* Write the text */
	mp_lcd_write_start(sock);
	mp_lcd_set_display_mode(sock,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT);
	mp_lcd_write_start(sock);
	for (i=0;i<3;i++) mp_lcd_write_buffer(sock,line+i*LCD_BUF_LEN,lineno*3+i);
	mp_lcd_set_display_mode(sock,f,f,f);
	mp_lcd_write_finish(sock);
}

/**
 * Write a buffer of text to the LCD -- with autoscrolling. (<= 144 chars)
 */
static void mp_lcd_write_text(int sock, char *text) {
	char lines[LCD_BUF_LEN*9]; uint32_t i = 0,z = 0; 
	uint8_t f1 = LCD_DISP_MODE_BUF1, f2 = LCD_DISP_MODE_BUF1, f3 = LCD_DISP_MODE_BUF1;

	if (!text || sock < 4) return;
	z = (strlen(text) > LCD_BUF_LEN*9) ? LCD_BUF_LEN*9 : strlen(text);

	/* Copy the text */
	memset(lines,0x20,LCD_BUF_LEN*9);
	memcpy(lines,text,z);

	/* Set flags for autoscrolling */
	if (z > LCD_BUF_LEN*3) { 
		f1 |= LCD_DISP_MODE_SCROLL | LCD_DISP_MODE_SCROLL2; 
		f2 = f3 = f1;
		if (z > LCD_BUF_LEN*6) { f1++; f2++; f3++; }
	}

	/* Write the text */
	mp_lcd_write_start(sock);
	mp_lcd_set_display_mode(sock,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT,LCD_DISP_MODE_INIT);
	mp_lcd_write_start(sock);
	for (i=0;i<3;i++) {
		mp_lcd_write_buffer(sock,lines+(LCD_BUF_LEN*i),i);
		mp_lcd_write_buffer(sock,lines+(LCD_BUF_LEN*i+3),i+3);
		mp_lcd_write_buffer(sock,lines+(LCD_BUF_LEN*i+6),i+6);
	}
	mp_lcd_set_display_mode(sock,f1,f2,f3);
	mp_lcd_write_finish(sock);
}	

/**************** DBus Methods *******************/

/* SetIndicator(indicator, blink) 
 *	[ indicator := see LCD_ICON_* above ]
 *	[ blink     := 0 (off) | 1 (on) | >= 2 (blink) ] 
 */ 
static DBusMessage *mp_dbus_set_indicator(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusError db_err; uint32_t indicator,blink;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&indicator,DBUS_TYPE_UINT32,&blink,DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&db_err)) error("logitech_mediapad: SetIndicator: unable to get args! (%s)",db_err.message);
	else mp_lcd_set_indicator(mp->sock,indicator,blink);
	dbus_error_free(&db_err);
	return NULL;
}

/* BlinkOrBeep(beep_type, blink)
 * 	[beep_type := 0 (none) | 1 (low beep) | 2 (beep-beep) | 3 (short beep) ] 
 *	[blink     := 0 (no)   | 1 (yes) ]
 */
static DBusMessage *mp_dbus_blink_or_beep(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusError db_err; uint32_t beep_type,blink;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&beep_type,DBUS_TYPE_UINT32,&blink,DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&db_err)) error("logitech_mediapad: BlinkOrBeep: unable to get args! (%s)",db_err.message);
	else mp_blink_or_beep(mp->sock,beep_type,blink);
	dbus_error_free(&db_err);
	return NULL;
}

/* BindKey(scancode,mode,key)  - see <linux/input.h> for KEY_* values
 *	[ scancode := Mediapad scancode ]
 *	[ mode     := 0 (normal) | 1 (nav) ]
 *	[ key      := key value to translate to (e.g. KEY_*) ]
 */ 
static DBusMessage *mp_dbus_bind_key(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusError db_err; uint32_t scancode,mode,key;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	dbus_error_init(&db_err);
	dbus_message_get_args(msg,&db_err,DBUS_TYPE_UINT32,&scancode,DBUS_TYPE_UINT32,&mode,&key,DBUS_TYPE_UINT32,DBUS_TYPE_INVALID);
	if (dbus_error_is_set(&db_err)) error("logitech_mediapad: BindKey: unable to get args! (%s)",db_err.message);
	else {
		/* Media keys */
		if (scancode > 0x82) {
			switch (scancode) {
				case MP_KEY_MEDIA:   mp_keymap_m[mode ? 1 : 0][0] = key; break;
				case MP_KEY_FFWD:    mp_keymap_m[mode ? 1 : 0][1] = key; break;
				case MP_KEY_STOP:    mp_keymap_m[mode ? 1 : 0][2] = key; break;
				case MP_KEY_PLAY:    mp_keymap_m[mode ? 1 : 0][3] = key; break;
				case MP_KEY_MUTE:    mp_keymap_m[mode ? 1 : 0][4] = key; break;
				case MP_KEY_VOLUP:   mp_keymap_m[mode ? 1 : 0][5] = key; break;
				case MP_KEY_VOLDOWN: mp_keymap_m[mode ? 1 : 0][6] = key; break;
			}
		} 
	
		/* Non-media keys */
		if (scancode < 0x63) mp_keymap[mode ? 1 : 0][scancode-0x54] = key;
	}

	dbus_error_free(&db_err);
	return NULL;
}

/* SyncClock() */
static DBusMessage *mp_dbus_sync_clock(DBusConnection *conn, DBusMessage *msg, void *data) {
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	mp_set_clock(mp->sock);
	return NULL;
}

/* ClearScreen() */
static DBusMessage *mp_dbus_clear_screen(DBusConnection *conn, DBusMessage *msg, void *data) {
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	mp_lcd_clear(mp->sock);
	return NULL; 
}

/* WriteText(text) Max Length: 144 */
static DBusMessage *mp_dbus_write_text(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessageIter db_args; char *text;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&db_args,&text);
			if (text && strlen(text) > 0) mp_lcd_write_text(mp->sock,text);
		}
	}

	return NULL;
}

/* WriteLine(lineno, text) Max Length: 48 */
static DBusMessage *mp_dbus_write_line(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessageIter db_args; char *text; uint32_t lineno;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&lineno);
		if (dbus_message_iter_has_next(&db_args)) {
			dbus_message_iter_next(&db_args);
			if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&db_args,&text);
				if (text && strlen(text) > 0) mp_lcd_write_line(mp->sock,text,lineno);
			}
		}
	}

	return NULL;
}

/* WriteBuffer(bufno, text) Max Length: 16 */
static DBusMessage *mp_dbus_write_buffer(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessageIter db_args; char *text; uint32_t bufno;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&bufno);
		if (dbus_message_iter_has_next(&db_args)) {
			dbus_message_iter_next(&db_args);
			if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&db_args,&text);
				if (text && strlen(text) > 0) mp_lcd_write_buffer(mp->sock,text,bufno);
			}
		}
	}

	return NULL;
}

/* WriteTextBin(chars) Max Length: 144 */
static DBusMessage *mp_dbus_write_text_bin(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessageIter db_args,db_sub; uint8_t val,i; char *chars;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&db_args,&db_sub);
			if ((chars = g_new0(char,1+(16*9)))) {
				for (i=0;i<=16*9;i++) {
					dbus_message_iter_get_basic(&db_sub,&val);
					chars[i] = (char)val;
					if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
					else break;
				} 
				if (i > 0) mp_lcd_write_text(mp->sock,chars); 
				g_free(chars);
			}
		}
	}

	return NULL;
}

/* WriteLineBin(lineno, chars) Max Length: 48 */
static DBusMessage *mp_dbus_write_line_bin(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessageIter db_args,db_sub; char *chars; uint32_t lineno; uint8_t val,i;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&lineno);
			if (dbus_message_iter_has_next(&db_args)) {
				dbus_message_iter_next(&db_args);
				if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
					dbus_message_iter_recurse(&db_args,&db_sub);
					if ((chars = g_new0(char,1+(16*3)))) {
						for (i=0;i<=16*3;i++) {
							dbus_message_iter_get_basic(&db_sub,&val);
							chars[i] = (char)val;
							if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
							else break;
						} 
						if (i > 0) mp_lcd_write_line(mp->sock,chars,lineno); 
						g_free(chars);
					}
				}
			}
	}

	return NULL;
}

/* WriteBufferBin(lineno, chars) Max Length: 16 */
static DBusMessage *mp_dbus_write_buffer_bin(DBusConnection *conn, DBusMessage *msg, void *data) {
	DBusMessageIter db_args,db_sub; char *chars; uint32_t bufno; uint8_t val,i;
	struct mp_state *mp = (struct mp_state *)data;

	if (!mp) return NULL;
	if (dbus_message_iter_init(msg,&db_args)) {
		if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_UINT32) 
			dbus_message_iter_get_basic(&db_args,&bufno);
			if (dbus_message_iter_has_next(&db_args)) {
				dbus_message_iter_next(&db_args);
				if (dbus_message_iter_get_arg_type(&db_args) == DBUS_TYPE_ARRAY) {
					dbus_message_iter_recurse(&db_args,&db_sub);
					if ((chars = g_new0(char,1+(16*3)))) {
						for (i=0;i<=16;i++) {
							dbus_message_iter_get_basic(&db_sub,&val);
							chars[i] = (char)val;
							if (dbus_message_iter_has_next(&db_sub)) dbus_message_iter_next(&db_sub);
							else break;
						} 
						if (i > 0) mp_lcd_write_buffer(mp->sock,chars,bufno); 
						g_free(chars);
					}
				}
			}
	}

	return NULL;
}

static GDBusMethodTable mp_methods[] = {
	{ "SetIndicator",   "uu",  "", mp_dbus_set_indicator,    G_DBUS_METHOD_FLAG_NOREPLY },
	{ "BlinkOrBeep",    "uu",  "", mp_dbus_blink_or_beep,    G_DBUS_METHOD_FLAG_NOREPLY },
	{ "BindKey",        "uuu", "", mp_dbus_bind_key,         G_DBUS_METHOD_FLAG_NOREPLY },
	{ "SyncClock",      "",    "", mp_dbus_sync_clock,       G_DBUS_METHOD_FLAG_NOREPLY },
	{ "ClearScreen",    "",    "", mp_dbus_clear_screen,     G_DBUS_METHOD_FLAG_NOREPLY },
	{ "WriteText",      "s",   "", mp_dbus_write_text,       G_DBUS_METHOD_FLAG_NOREPLY }, 
	{ "WriteLine",      "us",  "", mp_dbus_write_line,       G_DBUS_METHOD_FLAG_NOREPLY },
	{ "WriteBuffer",    "us",  "", mp_dbus_write_buffer,     G_DBUS_METHOD_FLAG_NOREPLY },
	{ "WriteTextBin",   "ai",  "", mp_dbus_write_text_bin,   G_DBUS_METHOD_FLAG_NOREPLY },
	{ "WriteLineBin",   "uai", "", mp_dbus_write_line_bin,   G_DBUS_METHOD_FLAG_NOREPLY },
	{ "WriteBufferBin", "uai", "", mp_dbus_write_buffer_bin, G_DBUS_METHOD_FLAG_NOREPLY }
};

/**************** Initialization / Event Code *******************/

/**
 * Initialize the mediapad
 */
int logitech_mediapad_setup_uinput(struct fake_input *fake_input, struct fake_hid *fake_hid) {
	DBusError db_err; struct uinput_dev dev; struct mp_state *mp; int i;
	
	/* Allocate a new mp_state struct */
	if (!(mp = g_new0(struct mp_state,1))) return 1;
	
	/* Open uinput */
	if ((mp->uinput = open("/dev/input/uinput",O_WRONLY|O_NONBLOCK)) <= 0) {
		if ((mp->uinput = open("/dev/uinput",O_WRONLY|O_NONBLOCK)) <= 0) {
			if ((mp->uinput = open("/dev/misc/uinput",O_WRONLY|O_NONBLOCK)) <= 0) {
				error("Error opening uinput device!");
				g_free(mp);
				return 1;
			}
		}
	}

	/* Setup the uinput device */
	memset(&dev,0,sizeof(struct uinput_dev));
	snprintf(dev.name,sizeof(dev.name),"Logitech DiNovo Mediapad");
	dev.id.bustype = BUS_BLUETOOTH;
	dev.id.vendor  = fake_hid->vendor;
	dev.id.product = fake_hid->product;

	if (write(mp->uinput,&dev,sizeof(struct uinput_dev)) != sizeof(struct uinput_dev)) {
		error("Unable to create uinput device");
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Enable events */
	if (ioctl(mp->uinput,UI_SET_EVBIT,EV_KEY) < 0) {
		error("Error enabling uinput key events");
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	if (ioctl(mp->uinput,UI_SET_EVBIT,EV_SYN) < 0) {
		error("Error enabling uinput syn events");
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Enable keys */
	for (i=0;i<KEY_UNKNOWN;i++) {
		if (ioctl(mp->uinput,UI_SET_KEYBIT,i) < 0) {
			error("Error enabling key #%d",i);
			close(mp->uinput);
			g_free(mp);
			return 1;
		}
	}
	
	/* Create the uinput device */
	if (ioctl(mp->uinput,UI_DEV_CREATE) < 0) {
		error("Error creating uinput device");
		close(mp->uinput);
		g_free(mp);
		return 1;
	} 

	/* Get-on-D-Bus :P */
	dbus_error_init(&db_err);
	if (!(mp->db_conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM,MP_DBUS_INTF,&db_err))) {
		info("db_conn is NULL!\n");
		if (dbus_error_is_set(&db_err)) 
			info("db_err was set! (%s: %s)\n",db_err.name,db_err.message ? db_err.message : "Out of Memory?");
		dbus_error_free(&db_err);
		close(mp->uinput);
		g_free(mp);
		return 1;
	} 
	
	dbus_connection_set_exit_on_disconnect(mp->db_conn,FALSE);
	dbus_error_free(&db_err);

	/* Register our interface */
	if (!g_dbus_register_interface(mp->db_conn,MP_DBUS_PATH,MP_DBUS_INTF,mp_methods,NULL,NULL,mp,NULL)) {
		error("Failed to register mediapad interface on path %s",MP_DBUS_PATH);
		dbus_connection_unref(mp->db_conn);
		close(mp->uinput);
		g_free(mp);
		return 1;
	}

	/* Get the interrupt socket */
	mp->sock           = g_io_channel_unix_get_fd(fake_input->io);
	fake_hid->priv     = mp;
	fake_input->uinput = mp->uinput;

	/* Set the mediapad clock */
	sleep(3); mp_set_clock(mp->sock);
	return 0;
}

/**
 * Handle an event from the mediapad
 */
gboolean logitech_mediapad_event(GIOChannel *chan, GIOCondition cond, gpointer data) {
	int ln = 0, isk = 0; char buf[8]; 
	struct fake_input *fake_input = (struct fake_input *)data;
	struct mp_state *mp = (struct mp_state *)(((struct fake_hid *)(fake_input->priv))->priv);
	isk = g_io_channel_unix_get_fd(chan);

	if (cond == G_IO_IN) {
		memset(buf,0,8);
		if ((ln = read(isk, buf, sizeof(buf))) <= 0) { 
			g_free(buf);
			g_io_channel_unref(chan);
			return FALSE;
		} 

		/*info("dinovo: m %d: in: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x",mp->mode,buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);*/

		/* Translate/Inject keypresses */
		if (buf[1] == 0x03) { /* Media keys */
			switch (buf[2] & 0xff) {
				case 0x00: /* (Media) Key up event */
					if (!mp->discard_keyup) {
						if (mp->prev_key != 0) { inject_key(mp->uinput,mp->prev_key,0); mp->prev_key = 0; } 
						mp->mode = 0;
					} else mp->discard_keyup = 0; 
				break;
				case MP_KEY_MEDIA:
					switch (buf[3]) {
						case 0x01: /* Media key */
							mp->prev_key = translate_key(mp->mode,MP_KEY_MEDIA);
							inject_key(mp->uinput,mp->prev_key,1);
						break;
						case 0x02: /* Clear Screen key */
							mp_lcd_clear(isk);
							if (mp->icons & LCD_ICON_MUTE) { 
								mp->icons = LCD_ICON_MUTE; 
								mp_lcd_set_indicator(isk,LCD_ICON_MUTE,1); 
							}
						break;
					}
				break;
				case MP_KEY_FFWD:
				case MP_KEY_REW:
				case MP_KEY_STOP:
				case MP_KEY_PLAY:
					mp->prev_key = translate_key(mp->mode,buf[2]);
					inject_key(mp->uinput,mp->prev_key,1);
				break;
				break;
				case MP_KEY_MUTE:
					/* XXX: Is there some way to be notified if the audio is already muted on init? */
					mp->prev_key = translate_key(mp->mode,KEY_MUTE);
					mp->icons   ^= LCD_ICON_MUTE; 
					inject_key(mp->uinput,mp->prev_key,1);
					mp_lcd_set_indicator(isk,LCD_ICON_MUTE,(mp->icons & LCD_ICON_MUTE) ? 1 : 0);
				break;
				case MP_KEY_VOLUP:
				case MP_KEY_VOLDOWN:
					mp->prev_key = translate_key(mp->mode,buf[2]);  
					mp->icons   &= ~LCD_ICON_MUTE; 
					inject_key(mp->uinput,mp->prev_key,1);
					mp_lcd_set_indicator(isk,LCD_ICON_MUTE,0);
				break;
			}
		} else if (buf[1] == 0x01 && buf[2] == 0x00) { /* Non-media keys */
			/* NAV key */
			if (buf[4] == 0x53 && buf[5] == 0x00) { 
				mp->mode ^= 1; 
				mp->prev_key = 0; 
				mp->discard_keyup = 1;
			} else {
				/* (Non-media) Key up event */
				if (buf[4] == 0x00 && buf[5] == 0x00 && mp->prev_key != 0) {
					inject_key(mp->uinput,mp->prev_key,0);
				} else if (buf[4] != 0x00) { /* Non-media key press */
					mp->prev_key = translate_key(mp->mode,buf[4] & 0x7f); 
					inject_key(mp->uinput,mp->prev_key,1); 
				}
			}
		} else if (buf[1] == 0x11 && buf[2] == 0x0a) { 
			/* Calculator Result */
			/*
			 cwtmp = &buf[4]; while (*cwtmp && *cwtmp == 0x20) cwtmp++;
			 syslog(LOG_WARNING,"Got Calc result");
			 */
		}
	} else {
		if (mp->db_conn) {
			g_dbus_unregister_interface(mp->db_conn,MP_DBUS_PATH,MP_DBUS_INTF);
			dbus_connection_unref(mp->db_conn); 
		}
		g_free(mp); 
		return FALSE; 
	}
	return TRUE;
}
/* vi:set ts=4: */

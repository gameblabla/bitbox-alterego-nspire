#include <stdlib.h>


#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <SDL/SDL.h>
#include <libndls.h>
#include <keys.h>

// emulated interfaces
#define draw_buffer __draw_buffer // prevents defining draw buffers to pixel t 
#include "bitbox.h"
#undef draw_buffer

#define DIR NIX_DIR // prevents name clashes with datfs DIR
#include <dirent.h>
#undef DIR

#include "fatfs/ff.h"

#define KBR_MAX_NBR_PRESSED 6

#define WM_TITLE_LED_ON  "Bitbox emulator (*)"
#define WM_TITLE_LED_OFF "Bitbox emulator"
/*
   TODO

 handle SLOW + PAUSE + FULLSCREEN (alt-enter) as keyboard handles
 handle mouse,
 keyboard (treat keyboard gamepads as config for quick saves)
 handling other events (plugged, ...)

 really handle set_led (as window title)

*/



// ----------------------------- kernel ----------------------------------
/* The only function of the kernel is
- calling game_init and game_frame
- calling update draw_buffer (which calls object_blitX)

- displaying other buffer updating line, frame
- read user input and update gamepad

*/

// ticks in ms


unsigned short screen_width;
unsigned short screen_height;

#define TICK_INTERVAL 1000/60
#define LINE_BUFFER 1024

#define USER_BUTTON_KEY SDLK_F12

static uint32_t next_time;

// options
static unsigned short quiet=1; // quiet by default now


// Video
SDL_Surface* screen;
uint16_t mybuffer1[LINE_BUFFER];
uint16_t mybuffer2[LINE_BUFFER];
uint16_t *draw_buffer = mybuffer1; // volatile ?

volatile uint16_t gamepad_buttons[2];
uint32_t vga_line;
volatile uint32_t vga_frame;
#ifdef VGA_SKIPLINE
volatile int vga_odd;
#endif

// IO
volatile int8_t mouse_x, mouse_y;
volatile uint8_t mouse_buttons;
 
int user_button=0;

// sound
// pixel_t audio_buffer[BITBOX_SNDBUF_LEN]; // stereo, 31khz 1frame

// joystick handling.
static const int gamepad_max_buttons = 12;
static const int gamepad_max_pads = 2;

volatile int8_t gamepad_x[2], gamepad_y[2]; // analog pad values

volatile uint8_t keyboard_mod[2]; // LCtrl =1, LShift=2, LAlt=4, LWin - Rctrl, ...
volatile uint8_t keyboard_key[2][KBR_MAX_NBR_PRESSED]; // using raw USB key codes


#if VGA_MODE != NONE
extern uint16_t palette_flash[256];

void __attribute__((weak)) graph_vsync() {} // default empty


void expand_buffer ( void )
{
    if (vga_odd) {
        // expand in place buffer from 8bits RRRGGBBL to 15bits RRRrrGGLggBBLbb
        // cost is ~ 5 cycles per pixel. not accelerated by putting palette in CCMRAM
        const uint32_t * restrict src = (uint32_t*)&draw_buffer[VGA_H_PIXELS/2-2];
        uint32_t * restrict dst=(uint32_t*)&draw_buffer[VGA_H_PIXELS-2];
        for (int i=0;i<VGA_H_PIXELS/4;i++) {
            uint32_t pix=*src--; // read 4 src pixels
            *dst-- = palette_flash[pix>>24]<<16         | palette_flash[(pix>>16) &0xff]; // write 2 pixels
            *dst-- = palette_flash[(pix>>8) & 0xff]<<16 | palette_flash[pix &0xff]; // write 2 pixels
        }
    }
}


/* naive pixel conversion
from 16bit bitbox pixel 0RRRRRGGGGGBBBBB
to 16bit color (565) RRRRRGGGGG0BBBBB
*/
static inline uint16_t pixelconv(uint16_t pixel)
{
    return (pixel & (uint16_t)(~0x1f))<<1 | (pixel & 0x1f);
}


static void refresh_screen (SDL_Surface *scr)
// uses global line + vga_odd, scale two times
{
    uint16_t * restrict dst = (uint16_t*)scr->pixels; // will render 2 pixels at a time horizontally

    draw_buffer = mybuffer1; // currently 16bit data

    for (vga_line=0;vga_line<screen_height;vga_line++) 
    {
        #ifdef VGA_SKIPLINE
            vga_odd=0;
            graph_line(); // using line, updating draw_buffer ...
            #if VGA_BPP==8
            expand_buffer();
            #endif 
            vga_odd=1; 
            graph_line(); //  a second time for SKIPLINE modes
            #if VGA_BPP==8
            expand_buffer();
            #endif 
        #else 
            graph_line(); 
            #if VGA_BPP==8
            expand_buffer();
            #endif 
        #endif

        // copy to screen at this position
        uint16_t *restrict src = (uint16_t*) draw_buffer;
		for (unsigned short i=0;i<screen_width;i++)
			*dst++= pixelconv(*src++);
			
        // swap lines buffers to simulate double line buffering
        draw_buffer = ( draw_buffer == &mybuffer1[0] ) ? &mybuffer2[0] : &mybuffer1[0];

    }
    for (;vga_line<screen_height+VGA_V_SYNC;vga_line++) {
        #ifdef VGA_SKIPLINE
            vga_odd=0;
            graph_vsync(); // using line, updating draw_buffer ...
            vga_odd=1; 
            graph_vsync(); //  a second time for SKIPLINE modes
        #else 
            graph_vsync(); // once
        #endif
    }
}
#endif


void set_mode(int width, int height)
{
    screen_width = width;
    screen_height = height;
    screen = SDL_SetVideoMode(width,height, 16, SDL_SWSURFACE);
    if ( !screen )
    {
        printf("%s\n",SDL_GetError());
        die(-1,0);
    }

}

static void joy_init()
// by David Lau
{
}


int init(void)
{
    // initialize SDL video
    if ( SDL_Init( SDL_INIT_VIDEO ) < 0 )
    {
        printf( "Unable to init SDL: %s\n", SDL_GetError() );
        return 1;
    }
    set_led(0); // off by default
    
    // create a default new window
    set_mode(VGA_H_PIXELS,VGA_V_PIXELS);

    joy_init();

    SDL_ShowCursor(SDL_DISABLE);
    
    return 0;
}


void instructions ()
{
}



// reverse mapping to USB BootP

#ifdef __HAIKU__
uint8_t key_trans[256] = {
	[0x01]=0,41, 58,59,60,61, 62,63,64,65, 66,67,68,69,  70,71,72,

	[0x11]=53,30,31,32,33,34,35,36,37,38,39,45,46,42,    73,74,75,  83,84,85,86,
	[0x26]=43, 20,26, 8,21,23,28,24,12,18,19,47,48,49,   76,77,78,  95,96,97,87,
	[0x3b]=57,   4,22, 7, 9,10,11,13,14,15,51,52,40,                92,93,94,
	[0x4b]=225,  29,27, 6,25, 5,17,16,54,55,56,229,         82,     89,90,91,88,
	[0x5c]=224,226,           44             ,230,228,   80,81,79,  98,   99,
	        227,                              231,118
};
#else
uint8_t key_trans[256] = { // scan_code -> USB BootP code
    [0x6F]=0x52, // up
    [0x74]=0x51, // down
    [0x71]=0x50, // left
    [0x72]=0x4F, // right
    [0x41]=0x2C, // space
    [0x09]=0x29, // ESC -- needs to define DISABLE_ESC_EXIT in makefile to avoid escaping emulator !
    [0x17]=0x2B, // TAB
    [0x16]=42, // backspace on mine... (lowagner)
    [0x77]=76, // delete
    [0x76]=73, // insert
    [0x7f]=0x48, // pause

    [0x0a]=30,31,32,33,34,35,36,37,38,39,45,46, // 1234567890-=
    [0x18]=20,26, 8,21,23,28,  24,  12,  18,  19,  // qwertyuiop
    [0x22]=47,48, // []
    [0x26]= 4,22, 7, 9,10,11,13,14,15,16, // asdfghjklm
    [0x30]=52,53, // ' and `
    [0x33]=49,  //  backslash
    [0x34]=29,27, 6,25, 5,17,54,55,56, // zxcvbnm,./

    [0x6e]=74, // home
    [0x73]=77, // end
    [0x70]=75, // pgup
    [0x75]=78, // pgdn
    [0x5a]=98,99, // 0. on number pad
    [0x57]=89,90,91, // 1 through 3
    [0x53]=92,93,94, // 4 through 6
    [0x4f]=95,96,97, // 7 through 9 on number pad

    [0x32]=225, // left shift
    [0x3e]=229, // right shift
    [0x40]=226, // L alt
    [0x6c]=230, // R alt
    [0x25]=0xe0, // L CTRL
    [0x69]=0xe4, // R CTRL
    [0x85]=0xe0, // L cmd (mac)
    [0x86]=0xe4, // R cmd (mac)
    [0x24]=0x28, // Enter

};
#endif

// this is a copy of the same function in usbh_hid_keybd
void kbd_emulate_gamepad (void)
{
    gamepad_buttons[0]=0;
    
	if (isKeyPressed(KEY_NSPIRE_UP))
		gamepad_buttons[0] |= gamepad_up;
	else if (isKeyPressed(KEY_NSPIRE_DOWN))
		gamepad_buttons[0] |= gamepad_down;
		
	if (isKeyPressed(KEY_NSPIRE_LEFT))
		gamepad_buttons[0] |= gamepad_left;
	else if (isKeyPressed(KEY_NSPIRE_RIGHT))
		gamepad_buttons[0] |= gamepad_right;	
		
	if (isKeyPressed(KEY_NSPIRE_CTRL))
		gamepad_buttons[0] |= gamepad_A;
	else if (isKeyPressed(KEY_NSPIRE_SHIFT))
		gamepad_buttons[0] |= gamepad_B;
		
	if (isKeyPressed(KEY_NSPIRE_VAR))
		gamepad_buttons[0] |= gamepad_X;
	else if (isKeyPressed(KEY_NSPIRE_DEL))
		gamepad_buttons[0] |= gamepad_Y;
		
	if (isKeyPressed(KEY_NSPIRE_TAB))
		gamepad_buttons[0] |= gamepad_L;
	else if (isKeyPressed(KEY_NSPIRE_ENTER))
		gamepad_buttons[0] |= gamepad_R;	
		
	if (isKeyPressed(KEY_NSPIRE_MENU))
		gamepad_buttons[0] |= gamepad_start;
}




static bool handle_events()
{
	if (isKeyPressed(KEY_NSPIRE_ESC))
		return true;
	
    return false; // don't exit  now
}

// -------------------------------------------------
// limited fatfs-related functions.
// XXX add non readonly features
FRESULT f_mount (FATFS* fs, const TCHAR* path, BYTE opt)
{
    return FR_OK;
}

FRESULT f_open (FIL* fp, const TCHAR* path, BYTE mode)
{
	char final_path[128];
	snprintf(final_path, sizeof(final_path), "./%s.tns", path);
	
    char *mode_host=0;

    // XXX quite buggy ...
    if (mode & FA_OPEN_ALWAYS) {
        if (!access(final_path, F_OK)) // 0 if OK
            mode_host = "r+";
        else
            mode_host = "w+";

    } else switch (mode) {
        // Not a very good approximation, should rewrite to handle properly
        case FA_READ | FA_OPEN_EXISTING : mode_host="r"; break;
        case FA_READ | FA_WRITE | FA_OPEN_EXISTING : mode_host="r+"; break;
        case FA_WRITE | FA_OPEN_EXISTING : mode_host="r+"; break; // faked

        case FA_WRITE | FA_CREATE_NEW : mode_host="wx"; break;
        case FA_READ | FA_WRITE | FA_CREATE_NEW : mode_host="wx+"; break;

        case FA_READ | FA_WRITE | FA_CREATE_ALWAYS : mode_host="w+"; break;
        case FA_WRITE | FA_CREATE_ALWAYS : mode_host="w"; break;

        default :
            return FR_DISK_ERR;
    }

    fp->fs = (FATFS*) fopen ((const char*)final_path,mode_host); // now ignores mode.
    return fp->fs ? FR_OK : FR_DISK_ERR; // XXX duh.
}

FRESULT f_close (FIL* fp)
{
    int res = fclose( (FILE*) fp->fs);
    fp->fs=NULL;
    return res?FR_DISK_ERR:FR_OK; // FIXME handle reasons ?
}

FRESULT f_read (FIL* fp, void* buff, UINT btr, UINT* br)
{
    *br = fread ( buff, 1,btr, (FILE *)fp->fs);
    return FR_OK; // XXX handle ferror
}

FRESULT f_write (FIL* fp, const void* buff, UINT btr, UINT* br)
{
    *br = fwrite ( buff,1, btr, (FILE *)fp->fs);
    return FR_OK; // XXX handle ferror
}


FRESULT f_lseek (FIL* fp, DWORD ofs)
{
    int res = fseek ( (FILE *)fp->fs, ofs, SEEK_SET);
    return res ? FR_DISK_ERR : FR_OK; // always from start
}

/* Change current directory */
FRESULT f_chdir (const char* path)
{
    int res = chdir(path);
    return res ? FR_DISK_ERR : FR_OK;
}


FRESULT f_opendir ( DIR* dp, const TCHAR* path )
{
    NIX_DIR *res = opendir(path);
    if (res) {
        dp->fs = (FATFS*) res; // hides it in the fs field as a fatfs variable
        dp->dir = (unsigned char *)path;
        return FR_OK;
    } else {
        printf("Error opening directory : %s\n",strerror(errno));
        return FR_DISK_ERR;
    }
}

FRESULT f_readdir ( DIR* dp, FILINFO* fno )
{
    errno=0;
    char buffer[260]; // assumes max path size for FAT32

    struct dirent *de = readdir((NIX_DIR *)dp->fs); 

    if (de) {
        for (int i=0;i<13;i++)
            fno->fname[i]=de->d_name[i];

        fno->fattrib = 0;

        // check attributes of found file
        strncpy(buffer,(char *)dp->dir,260); // BYTE->char
        strcat(buffer,"/");
        strcat(buffer,fno->fname);

        struct stat stbuf;
        stat(buffer,&stbuf);

        if (S_ISDIR(stbuf.st_mode))
             fno->fattrib = AM_DIR;
        return FR_OK;

    } else {
        if (errno) {
            printf("Error reading directory %s : %s\n",dp->dir, strerror(errno)); // not neces an erro, can be end of dir.
            return FR_DISK_ERR;
        } else {
            fno->fname[0]='\0';
            return FR_OK;
        }
    }
}

FRESULT f_closedir (DIR* dp)
{
    if (!closedir((NIX_DIR *)dp->fs)) {
        return FR_OK ;
    } else {
        printf("Error closing directory %s : %s\n",dp->dir, strerror(errno));
        return FR_DISK_ERR;
    }
}


// user button
int button_state() {
    return user_button;
}

// user LED
void set_led(int x) {
}

int main ( int argc, char* argv[] )
{
    // program main loop
    unsigned char done = false;
	
	enable_relative_paths(argv);
	
    gamepad_buttons[0] = 0; // all up
    gamepad_buttons[1] = 0;

    if (init()) return 1;
    game_init();
    
    while (!done)
    {

        // message processing loop
        done = handle_events();
        kbd_emulate_gamepad();
        // update game
        game_frame();

        // update time
        vga_frame++;
        
        #if VGA_MODE!=NONE
        refresh_screen(screen);
        #endif

        SDL_Flip(screen);
    } // end main loop

    return 0;
}

void message (const char *fmt, ...)
{
    /*va_list argptr;
    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);*/
}

void die(int where, int cause)
{
   /* printf("ERROR : dying doing %d at  %d.\n",cause, where);*/
    exit(0);
}

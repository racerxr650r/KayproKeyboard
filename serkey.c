/*
 * serkey.c
 *
 * Linux serial keyboard driver that supports the Kaypro keyboard and other 
 * custom key mappings.
 *
 * Created: 10/8/2024 12:39:38 PM
 * Author : john anderson
 *
 * Copyright (C) 2024 by John Anderson <racerxr650r@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any 
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES 
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */ 
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>

// Macros *********************************************************************
// I've never liked how the static keyword is overloaded in C
// These macros make it more intuitive
#define local        static
#define persistent   static

#define LOG(fmt_str,...)	do{\
   if(appConfig.verbose) \
	   fprintf(stdout,fmt_str, ##__VA_ARGS__); \
}while(0)

// Constants ******************************************************************
#define KEYMAPS      4
#define KEYS_PER_MAP 256

// Data Types *****************************************************************
// Keymap
typedef enum
{
   KEYMAP_KAYPRO,
   KEYMAP_ASCII,
   KEYMAP_MEDIA_KEYS,
   KEYMAP_CUSTOM
}keymaps_t;

typedef struct
{
    char key;
    bool control,shift,makebreak;
}keymap_t;

// Serial Port
typedef enum
{
   PARITY_NONE,
   PARITY_EVEN,
   PARITY_ODD
}parity_t;

typedef enum
{
   DATABITS_5 = CS5,
   DATABITS_6 = CS6,
   DATABITS_7 = CS7,
   DATABITS_8 = CS8
}databits_t;

typedef enum
{
   STOPBITS_1 = 0,
   STOPBITS_2 = CSTOPB
}stopbits_t;

typedef struct
{
   int      baudrate;
   speed_t  speed;
}baudrate_t;

// Configuration
typedef struct CONFIG
{
   speed_t     speed;
   parity_t    parity;
   databits_t  databits;
   stopbits_t   stopbits;
   keymaps_t   keymap;
   char        *tty;
   bool        fork, verbose;
}config_t;

// Globals ********************************************************
// Serial Port 
struct termios ttyConfig;
int            ttyFd = 0;
baudrate_t     speeds[] =
{
   {.baudrate = 50, .speed = B50},
   {.baudrate = 110, .speed = B110},
   {.baudrate = 300, .speed = B300},
   {.baudrate = 1200, .speed = B1200},
   {.baudrate = 2400, .speed = B2400},
   {.baudrate = 4800, .speed = B4800},
   {.baudrate = 9600, .speed = B9600},
   {.baudrate = 19200, .speed = B19200},
   {.baudrate = 38400, .speed = B38400},
   {.baudrate = 57600, .speed = B57600},
   {.baudrate = 115200, .speed = B115200},
   {.baudrate = 230400, .speed = B230400},
   {.baudrate = 460800, .speed = B460800},
   {.baudrate = 921600, .speed = B921600},
   {.baudrate = 1152000, .speed = B1152000}
};

// Keymap
local keymap_t keymap[KEYMAPS][KEYS_PER_MAP];  // Scroll to the bottom of the file for definition

// Configuration w/default values
config_t appConfig = {  .speed = B300,
                        .parity = PARITY_NONE,
                        .databits = DATABITS_8,
                        .stopbits = STOPBITS_1,
                        .keymap = KEYMAP_KAYPRO,
                        .tty = "/dev/ttyAMA4",
                        .fork = false,
                        .verbose = false};

// Local function prototypes **************************************************
// Application ctrl
local void parseCommandLine(  int argc,      // Total count of arguments
                              char *argv[]); // Array of pointers to the argument strings

local void displayUsage(FILE *ouput);        // File pointer to output the text to

local void exitApp(  char* error_str,        // Descriptive char string
                     bool display_usage,     // Display usage?
                     int return_code);       // Return code to use for exit()

// Uinput Interface
local void emit(  int fd,              // File descriptor for Uinput
                  int type,            // Type of code
                  int code,            // Key code
                  int val);            // Code modifier

local void emitKey(  int fd,           // File descriptor for Uinput
                     keymap_t *key);   // Keymap entry for the key to be passed to Uinput

local int connectUinput(void);

// Serial port
local int getSerialConfig( int fd,                    // File descriptor
                           struct termios *config);   // termios configuration

local int setSerialConfig( int fd,                    // File descriptor
                           struct termios *config);   // termios configuration

local int configSerial( int         fd,               // File descriptor for /dev/tty?
                        speed_t     speed,            // Baudrate B? [B50 to B115200]
                        parity_t    parity,           // Parity [PARITY_NONE | PARITY_ODD | PARITY_EVEN]
                        databits_t  dataBits,         // Number of data bits [DATABITS_5 | DATABITS_6 | DATABITS_7 | DATABITS_8]
                        stopbits_t  stopBits);        // Number of stop bits [STOPBITS_1 | STOPBITS_2]

local int openSerial(char *tty,                       // Path/Name of the tty device
                     speed_t     speed,               // Baudrate B? [B50 to B115200]
                     parity_t    parity,              // Parity [PARITY_NONE | PARITY_ODD | PARITY_EVEN]
                     databits_t  dataBits,            // Number of data bits [DATABITS_5 | DATABITS_6 | DATABITS_7 | DATABITS_8]
                     stopbits_t  stopBits);           // Number of stop bits [STOPBITS_1 | STOPBITS_2]

local int closeSerial(int fd);                        // File descriptor of serial device

/*
 * Main Entry Point ***********************************************************
 */
int main(int argc, char *argv[])
{
   // Parse the command line and setup the config
   parseCommandLine(argc, argv);

   // If enabled fork process closing the parent and returning without error
   if(appConfig.fork)
   {
      if(daemon(0,1))
         exitApp("Daemon failed to start",false,-1);
      LOG("Forked daemon\n\r");
   }

   // Open and configure the serial port
   int fdSerial;
   if((fdSerial = openSerial(appConfig.tty, appConfig.speed, appConfig.parity, appConfig.databits, appConfig.stopbits))<1)
      exitApp("Unable to open serial device",false,-1);
   LOG("Opened and configured serial device\n\r");

   // Connect to the uinput kernel module
   int uinput_fd = connectUinput();
   LOG("Connected to uintput\n\r");

   // Loop forever reading keystrokes from the serial port and writing the 
   // mapped key code to Uninput 
   do
   {
      unsigned char  key;
      size_t         count;

      // Read the next key from the serial port
      // This call is blocking
      count = read(fdSerial, &key, sizeof(key));

      // If read a key from from the serial port...
      if(count!=0)
      {
         // Display it to stdout
         if(isprint(key))
            LOG(" In - Key: \"%c\" code: %03d ", (char)key, key);
         else
            LOG(" In - Key: N/A code: %03d ", key);

         // Send the mapped key code to uinput
         emitKey(uinput_fd, &keymap[appConfig.keymap][key]);
      }
      else
      {
         if(errno!=0)
            exitApp("read returned an error", false, -2);
         else
            exitApp("read returned zero bytes", false, 0);
      }

   } while(true);

   return 0;
}

// Program Runtime Functions **************************************************
/*
 * Parse the application command line and set up the configuration
 */
local void parseCommandLine(int argc, char *argv[])
{
   // For each command line argument...
   for(int i=1;i<=argc-1;++i)
   {
      // If command line switch "-" character...
      if(argv[i][0]=='-')
      {
         int baudrate, databits, stopbits;

         // Decode the command line switch and apply...
         switch(argv[i][1])
         {
            case 'b':
               baudrate = atoi(argv[++i]);
               // If the baudrate is not 0...
               for(int j=0;j<sizeof(speeds)/sizeof(baudrate_t);++j)
                  if(speeds[j].baudrate==baudrate)
                  {
                     appConfig.speed = speeds[j].speed;
                     break;
                  }
               // If searched to the end of the table of speeds...
               if(i==sizeof(speeds)/sizeof(baudrate_t))
                  exitApp("Invalid Baudrate", true, -4);
               break;
            case 'p':
               ++i;
               // If valid parity setting...
               if(!strcmp(argv[i],"odd"))
                  appConfig.parity = PARITY_ODD;
               else if(!strcmp(argv[i],"even"))
                  appConfig.parity = PARITY_EVEN;
               else if(!strcmp(argv[i],"none"))
                  appConfig.parity=PARITY_NONE;
               // Else error...
               else
                  exitApp("Invalid parity", true, -5);
               break;
            case 'd':
               databits = atoi(argv[++i]);
               // If valid data bits...
               if(databits == 5)
                  appConfig.databits = DATABITS_5;
               else if(databits == 6)
                  appConfig.databits = DATABITS_6;
               else if(databits == 7)
                  appConfig.databits = DATABITS_7;
               else if(databits == 6)
                  appConfig.databits = DATABITS_8;
               // Else error...
               else 
                  exitApp("Invalid data bits", true, -6);
               break;
            case 's':
               stopbits = atoi(argv[++i]);
               // If valid stop bits...
               if(stopbits == 1)
                  appConfig.stopbits = STOPBITS_1;
               else if(stopbits == 2)
                  appConfig.stopbits = STOPBITS_2;
               // Else error...
               else
                  exitApp("Invalid stop bits", true, -7);
               break;
            case 'k':
               ++i;
               // If kaypro key map setting...
               if(!strcmp(argv[i],"kaypro"))
                  appConfig.keymap = KEYMAP_KAYPRO;
               // Else if media_keys key map setting...
               else if(!strcmp(argv[i],"media_keys"))
                  appConfig.keymap = KEYMAP_MEDIA_KEYS;
               // Else if ascii key map setting...
               else if(!strcmp(argv[i],"ascii"))
                  appConfig.keymap = KEYMAP_ASCII;
               // Else error...
               else
                  exitApp("Invalid key map", true, -8);
               break;
            case 'f':
               appConfig.fork = true;
               break;
            case 'v':
               appConfig.verbose = true;
               break;
            case 'h':
            case '?':
               exitApp(NULL, true, 0);
               break;
            default:
               exitApp("Unknown switch", true, -9);
         }
      }
      // Else update the device path/name 
      else
         appConfig.tty = argv[i];
   }

   if(appConfig.tty==NULL)
      exitApp("No serial device provided", true, -11);
}

/*
 * Display the application usage w/command line options and exit w/error
 */
local void displayUsage(FILE *output_stream)
{
   fprintf(output_stream, "Usage: serkey [OPTION]... serial_device\n\n\r"
          "serkey is a user mode serial keyboard driver for Linux. It utilizes the uinput\n\r"
          "kernel module and tio serial device I/O tool. Therefore, both must be installed\n\r"
          "and enabled. In addition, serkey must be run at a priviledge level capable of\n\r"
          "communicating with uinput. On most distributions, this is root level priviledges\n\r"
          "by default. The serial_device specifies the \\dev tty device connected to the \n\r"
          "keyboard.\n\n\r"
          "OPTIONS:\n\r"
          "  -b   <bps>\n\r"
          "       Set the baud rate in bits per second (bps) (default:300)\n\r"
          "  -p   odd|even|none|mark|space\n\r"
          "       Set the parity  (default:none)\n\r"
          "  -d   5|6|7|8|9\n\r"
          "       Set the number of data bits (default:8)\n\r"
          "  -s   1|2\n\r"
          "       Set the number of stop bits (default:1)\n\r"
          "  -k   kaypro|media_keys|ascii\n\r"
          "       Select the key mapping (default:kaypro)\n\r"
          "  -f   Fork and exit creating daemon process\n\r"
          "  -v   Verbose output to stdout/stderr\n\r"
          "  -h   Display this usage information\n\r");
}

/*
 * Display a message and exit the application with a given return code
 */
local void exitApp(char* error_str, bool display_usage, int return_code)
{
   FILE *output;

   // If the serial port has already been configured...
   if(ttyFd>0)
      closeSerial(ttyFd);

   // Is the return code an error...
   if(return_code)
      output = stderr;
   else
      output = stdout;

   // If an error string was provided...
   if(error_str)
      if(strlen(error_str))
         fprintf(output, "%s %s\n\r%s %s\n\r",  return_code?"Error:":"OK:", 
                                       error_str, 
                                       return_code?"Error Code -":"", 
                                       return_code?strerror(errno):"");

   if(display_usage == true)
      displayUsage(output);

   fflush(output);
   exit(return_code);
}

// Uinput interface functions *************************************************
/*
 * Emit an event to the uinput virtual device 
 */
local void emit(int fd, int type, int code, int val)
{
   struct input_event ie;

   ie.type = type;
   ie.code = code;
   ie.value = val;
   // timestamp values below are ignored
   ie.time.tv_sec = 0;
   ie.time.tv_usec = 0;

   int ret = write(fd, &ie, sizeof(ie));

   if(ret != sizeof(ie))
      exitApp("Failed to write to uintput\n\r", false, -12);
}

/*
 * Emit a key press to uinput
 */
local void emitKey(int fd, keymap_t *key)
{

   LOG("  Out - ");
   
   // If control key required...
   if(key->control)
   {
      LOG("Ctrl: Make ");
      // Control key make, report the event
      emit(fd, EV_KEY, KEY_LEFTCTRL, 1);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
   else
      LOG("Ctrl: N/A  ");
   // If shift key required...
   if(key->shift)
   {
      LOG("Shift: Make ");
      // Shift key make, report the event
      emit(fd, EV_KEY, KEY_LEFTSHIFT, 1);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
   else
      LOG("Shift: N/A  ");

   // If make/break required...
   if(key->makebreak)
   {
      LOG("MB: 1 Key %03d ",key->key);
      // Key make, report the event
      emit(fd, EV_KEY, key->key, 1);
      emit(fd, EV_SYN, SYN_REPORT, 0);
      // Key break, report the event
      emit(fd, EV_KEY, key->key, 0);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
   // Else just make or break according the MSB...
   else
   {
      LOG("MB: 0 Key %03d ",key->key);
      // Key make or break, report the event
      emit(fd, EV_KEY, key->key && 0x7f, (key->key && 0x80) >> 7);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }

   // If control key required...
   if(key->control)
   {
      LOG("CTRL: break");
      // Control key break, report the event
      emit(fd, EV_KEY, KEY_LEFTCTRL, 0);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
   // If shift key required...
   if(key->shift)
   {
      LOG("SHIFT: break");
      // Shift key break, report the event
      emit(fd, EV_KEY, KEY_LEFTSHIFT, 0);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }

   LOG("\n\r");
}

/*
 * Connect to the uinput kernel module
 */
local int connectUinput()
{
   /*
    * uinput setup and open a pipe to uinput
    */
   struct uinput_setup usetup;
   int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

   // If unable to open a pipe to uinput...
   if(fd == -1)
   {
      exitApp("Unable to open pipe to uinput. Make sure you have permission to access\n\r"
              "uinput virtual device. Try \"sudo serkey\" to run at root level permissions"
              , false, -17);
   }

   /*
    * The ioctls below will enable the device that is about to be
    * created. This includes "registering" all the possible key events
    */
   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   for(int i=0;i<256;++i)
   {
      if(keymap[appConfig.keymap][i].key != KEY_RESERVED)
         ioctl(fd, UI_SET_KEYBIT, keymap[appConfig.keymap][i].key);

      ioctl(fd,UI_SET_KEYBIT,KEY_LEFTSHIFT);
      ioctl(fd,UI_SET_KEYBIT,KEY_LEFTCTRL);
   }

   memset(&usetup, 0, sizeof(usetup));
   usetup.id.bustype = BUS_USB;
   usetup.id.vendor = 0x1234; /* sample vendor */
   usetup.id.product = 0x5678; /* sample product */
   strncpy(usetup.name, "serkey", sizeof(usetup.name));

   ioctl(fd, UI_DEV_SETUP, &usetup);
   ioctl(fd, UI_DEV_CREATE);

   return(fd);
}

// Serial Port Functions ******************************************************
/*
 * Get the current serial configuration
 */
local int getSerialConfig( int fd,                 // File descriptor
                           struct termios *config) // termios configuration
{
   int ret = tcgetattr(fd,config);
   return(ret);
}

/*
 * Set the current serial configuration
 */
local int setSerialConfig( int fd,                 // File descriptor
                           struct termios *config) // termios configuration
{
   int ret = tcsetattr(fd,TCSANOW,config);
   return(ret);
}

/*
 * Setup the serial port
 */
local int configSerial( int         fd,         // File descriptor for /dev/tty?
                        speed_t     speed,      // Baudrate B? [B50 to B115200]
                        parity_t    parity,     // Parity [PARITY_NONE | PARITY_ODD | PARITY_EVEN]
                        databits_t  dataBits,   // Number of data bits [DATABITS_5 | DATABITS_6 | DATABITS_7 | DATABITS_8]
                        stopbits_t  stopBits)   // Number of stop bits [STOPBITS_1 | STOPBITS_2]
{
   struct termios tty;

   // Set the input and output baudrate
   cfsetospeed(&tty, speed);
   cfsetispeed(&tty, speed);

   // Set the data bits
   tty.c_cflag &= ~CSIZE;
   tty.c_cflag |= dataBits;

   // Disable Ignore CR and CR/NL translations
   tty.c_iflag &= ~(INLCR | IGNCR | ICRNL | ISTRIP); // disable break processing
   tty.c_lflag = 0;        // no signaling chars, no echo,
                           // no canonical processing
   tty.c_oflag = 0;        // no remapping, no delays

   // Block until 1 character read
   tty.c_cc[VMIN]  = 1;
   tty.c_cc[VTIME] = 0;

   // Turn off xon/xoff ctrl
   tty.c_iflag &= ~(IXON | IXOFF | IXANY);
   // Ignore modem ctrls and enable read
   tty.c_cflag |= (CLOCAL | CREAD);    
   // Turn off rts/cts
   //tty.c_cflag &= ~CRTSCTS;

   // Set parity
   tty.c_cflag &= ~(PARENB | PARODD);
   if(parity == PARITY_ODD)
      tty.c_cflag |= (PARENB | PARODD);
   else if(parity == PARITY_EVEN)
      tty.c_cflag |= PARENB;

   // Set stop bits
   tty.c_cflag &= ~CSTOPB;
   tty.c_cflag |= (unsigned int)stopBits;

   return(setSerialConfig(fd, &tty));
}

/*
 * Open a tty serial device, save it's current config, and set the new config
 */
local int openSerial(char *tty,              // Path/Name of the tty device
                     speed_t     speed,      // Baudrate B? [B50 to B115200]
                     parity_t    parity,     // Parity [PARITY_NONE | PARITY_ODD | PARITY_EVEN]
                     databits_t  dataBits,   // Number of data bits [DATABITS_5 | DATABITS_6 | DATABITS_7 | DATABITS_8]
                     stopbits_t  stopBits)   // Number of stop bits [STOPBITS_1 | STOPBITS_2]
{
   int fd;

   // Open the file descriptor
   if((fd = open(tty, O_RDWR | O_NOCTTY))<0)
      exitApp("Unable to open to serial device",false,-1);

   // Get the current serial device configuration
   if(getSerialConfig(fd,&ttyConfig))
      exitApp("Unable to get the current serial device configuration",false,-1);

   // Setup the new serial device configuration
   if(configSerial(fd, speed, parity, dataBits, stopBits))
      exitApp("Unable to set the serial device configuration",false,-1);

   return(fd);
}

/*
 * Close a tty serial device and restore it's config
 */
local int closeSerial(int fd)    // File descriptor of serial device
{
   if(setSerialConfig(ttyFd,&ttyConfig))
      exitApp("Unable to reset the serial device configuration",false,-1);
   return(close(fd));
}

// Key Maps *******************************************************************
local keymap_t keymap[KEYMAPS][KEYS_PER_MAP] =
{
    {   // Kaypro key map--------------------------------------------------------------------------------------------------------------------------------------
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 0	NULL(Null character)			
        { .key = KEY_A, .control = true, .shift = false, .makebreak = true },           // 1	SOH	(Start of Header)
        { .key = KEY_B, .control = true, .shift = false, .makebreak = true },           // 2	STX	(Start of Text)			
        { .key = KEY_C, .control = true, .shift = false, .makebreak = true },           // 3	ETX	(End of Text)			
        { .key = KEY_D, .control = true, .shift = false, .makebreak = true },           // 4	EOT	(End of Transmission)			
        { .key = KEY_E, .control = true, .shift = false, .makebreak = true },           // 5	ENQ	(Enquiry)			
        { .key = KEY_F, .control = true, .shift = false, .makebreak = true },           // 6	ACK	(Acknowledgement)			
        { .key = KEY_G, .control = true, .shift = false, .makebreak = true },           // 7	BEL	(Bell)			
        { .key = KEY_BACKSPACE, .control = false, .shift = false, .makebreak = true },  // 8	BS	(Backspace)			
        { .key = KEY_TAB, .control = false, .shift = false, .makebreak = true },        // 9	HT	(Horizontal Tab)			
        { .key = KEY_LINEFEED, .control = false, .shift = false, .makebreak = true },   // 10	LF	(Line feed)			
        { .key = KEY_K, .control = true, .shift = false, .makebreak = true },           // 11	VT	(Vertical Tab)			
        { .key = KEY_L, .control = true, .shift = false, .makebreak = true },           // 12	FF	(Form feed)			
        { .key = KEY_ENTER, .control = false, .shift = false, .makebreak = true },      // 13	CR	(Carriage return)			
        { .key = KEY_N, .control = true, .shift = false, .makebreak = true },           // 14	SO	(Shift Out)			
        { .key = KEY_O, .control = true, .shift = false, .makebreak = true },           // 15	SI	(Shift In)			
        { .key = KEY_P, .control = true, .shift = false, .makebreak = true },           // 16	DLE	(Data link escape)			
        { .key = KEY_Q, .control = true, .shift = false, .makebreak = true },           // 17	DC1	(Device control 1)			
        { .key = KEY_R, .control = true, .shift = false, .makebreak = true },           // 18	DC2	(Device control 2)			
        { .key = KEY_S, .control = true, .shift = false, .makebreak = true },           // 19	DC3	(Device control 3)			
        { .key = KEY_T, .control = true, .shift = false, .makebreak = true },           // 20	DC4	(Device control 4)			
        { .key = KEY_U, .control = true, .shift = false, .makebreak = true },           // 21	NAK	(Negative acknowledgement)			
        { .key = KEY_V, .control = true, .shift = false, .makebreak = true },           // 22	SYN	(Synchronous idle)			
        { .key = KEY_W, .control = true, .shift = false, .makebreak = true },           // 23	ETB	(End of transmission block)			
        { .key = KEY_CANCEL, .control = false, .shift = false, .makebreak = true },     // 24	CAN	(Cancel)			
        { .key = KEY_Y, .control = true, .shift = false, .makebreak = true },           // 25	EM	(End of medium)			
        { .key = KEY_Z, .control = true, .shift = false, .makebreak = true },           // 26	SUB	(Substitute)			
        { .key = KEY_ESC, .control = false, .shift = false, .makebreak = true },        // 27	ESC	(Escape)			
        { .key = KEY_BACKSLASH, .control = true, .shift = false, .makebreak = true },   // 28	FS	(File separator)			
        { .key = KEY_RIGHTBRACE, .control = true, .shift = false, .makebreak = true },  // 29	GS	(Group separator)			
        { .key = KEY_6, .control = true, .shift = true, .makebreak = true },            // 30	RS	(Record separator)			
        { .key = KEY_MINUS, .control = true, .shift = true, .makebreak = true },        // 31	US	(Unit separator)			
        { .key = KEY_SPACE, .control = false, .shift = false, .makebreak = true },      // 32	 	(space)			
        { .key = KEY_1, .control = false, .shift = true, .makebreak = true },           // 33	!	(exclamation mark)			
        { .key = KEY_APOSTROPHE, .control = false, .shift = true, .makebreak = true },  // 34	"	(Quotation mark)			
        { .key = KEY_3, .control = false, .shift = true, .makebreak = true },           // 35	#	(Number sign)			
        { .key = KEY_4, .control = false, .shift = true, .makebreak = true },           // 36	$	(Dollar sign)			
        { .key = KEY_5, .control = false, .shift = true, .makebreak = true },           // 37	%	(Percent sign)			
        { .key = KEY_7, .control = false, .shift = true, .makebreak = true },           // 38	&	(Ampersand)			
        { .key = KEY_APOSTROPHE, .control = false, .shift = false, .makebreak = true }, // 39	'	(Apostrophe)			
        { .key = KEY_9, .control = false, .shift = true, .makebreak = true },           // 40	(	(round brackets or parentheses)			
        { .key = KEY_0, .control = false, .shift = true, .makebreak = true },           // 41	)	(round brackets or parentheses)			
        { .key = KEY_8, .control = false, .shift = true, .makebreak = true },           // 42	*	(Asterisk)			
        { .key = KEY_EQUAL, .control = false, .shift = true, .makebreak = true },       // 43	+	(Plus sign)			
        { .key = KEY_COMMA, .control = false, .shift = false, .makebreak = true },      // 44	,	(Comma)			
        { .key = KEY_MINUS, .control = false, .shift = false, .makebreak = true },      // 45	-	(Hyphen)			
        { .key = KEY_DOT, .control = false, .shift = false, .makebreak = true },        // 46	.	(Full stop , dot)			
        { .key = KEY_SLASH, .control = false, .shift = false, .makebreak = true },      // 47	/	(Slash)			
        { .key = KEY_0, .control = false, .shift = false, .makebreak = true },          // 48	0	(number zero)			
        { .key = KEY_1, .control = false, .shift = false, .makebreak = true },          // 49	1	(number one)			
        { .key = KEY_2, .control = false, .shift = false, .makebreak = true },          // 50	2	(number two)			
        { .key = KEY_3, .control = false, .shift = false, .makebreak = true },          // 51	3	(number three)			
        { .key = KEY_4, .control = false, .shift = false, .makebreak = true },          // 52	4	(number four)			
        { .key = KEY_5, .control = false, .shift = false, .makebreak = true },          // 53	5	(number five)			
        { .key = KEY_6, .control = false, .shift = false, .makebreak = true },          // 54	6	(number six)			
        { .key = KEY_7, .control = false, .shift = false, .makebreak = true },          // 55	7	(number seven)			
        { .key = KEY_8, .control = false, .shift = false, .makebreak = true },          // 56	8	(number eight)			
        { .key = KEY_9, .control = false, .shift = false, .makebreak = true },          // 57	9	(number nine)			
        { .key = KEY_SEMICOLON, .control = false, .shift = true, .makebreak = true },   // 58	:	(Colon)			
        { .key = KEY_SEMICOLON, .control = false, .shift = false, .makebreak = true },  // 59	;	(Semicolon)			
        { .key = KEY_COMMA, .control = false, .shift = true, .makebreak = true },       // 60	<	(Less-than sign )			
        { .key = KEY_EQUAL, .control = false, .shift = false, .makebreak = true },      // 61	=	(Equals sign)			
        { .key = KEY_DOT, .control = false, .shift = true, .makebreak = true },         // 62	>	(Greater-than sign ; Inequality) 			
        { .key = KEY_SLASH, .control = false, .shift = true, .makebreak = true },       // 63	?	(Question mark)			
        { .key = KEY_2, .control = false, .shift = true, .makebreak = true },           // 64	@	(At sign)			
        { .key = KEY_A, .control = false, .shift = true, .makebreak = true },           // 65	A	(Capital A )			
        { .key = KEY_B, .control = false, .shift = true, .makebreak = true },           // 66	B	(Capital B )			
        { .key = KEY_C, .control = false, .shift = true, .makebreak = true },           // 67	C	(Capital C )			
        { .key = KEY_D, .control = false, .shift = true, .makebreak = true },           // 68	D	(Capital D )			
        { .key = KEY_E, .control = false, .shift = true, .makebreak = true },           // 69	E	(Capital E )			
        { .key = KEY_F, .control = false, .shift = true, .makebreak = true },           // 70	F	(Capital F )			
        { .key = KEY_G, .control = false, .shift = true, .makebreak = true },           // 71	G	(Capital G )			
        { .key = KEY_H, .control = false, .shift = true, .makebreak = true },           // 72	H	(Capital H )			
        { .key = KEY_I, .control = false, .shift = true, .makebreak = true },           // 73	I	(Capital I )			
        { .key = KEY_J, .control = false, .shift = true, .makebreak = true },           // 74	J	(Capital J )			
        { .key = KEY_K, .control = false, .shift = true, .makebreak = true },           // 75	K	(Capital K )			
        { .key = KEY_L, .control = false, .shift = true, .makebreak = true },           // 76	L	(Capital L )			
        { .key = KEY_M, .control = false, .shift = true, .makebreak = true },           // 77	M	(Capital M )			
        { .key = KEY_N, .control = false, .shift = true, .makebreak = true },           // 78	N	(Capital N )			
        { .key = KEY_O, .control = false, .shift = true, .makebreak = true },           // 79	O	(Capital O )			
        { .key = KEY_P, .control = false, .shift = true, .makebreak = true },           // 80	P	(Capital P )			
        { .key = KEY_Q, .control = false, .shift = true, .makebreak = true },           // 81	Q	(Capital Q )			
        { .key = KEY_R, .control = false, .shift = true, .makebreak = true },           // 82	R	(Capital R )			
        { .key = KEY_S, .control = false, .shift = true, .makebreak = true },           // 83	S	(Capital S )			
        { .key = KEY_T, .control = false, .shift = true, .makebreak = true },           // 84	T	(Capital T )			
        { .key = KEY_U, .control = false, .shift = true, .makebreak = true },           // 85	U	(Capital U )			
        { .key = KEY_V, .control = false, .shift = true, .makebreak = true },           // 86	V	(Capital V )			
        { .key = KEY_W, .control = false, .shift = true, .makebreak = true },           // 87	W	(Capital W )			
        { .key = KEY_X, .control = false, .shift = true, .makebreak = true },           // 88	X	(Capital X )			
        { .key = KEY_Y, .control = false, .shift = true, .makebreak = true },           // 89	Y	(Capital Y )			
        { .key = KEY_Z, .control = false, .shift = true, .makebreak = true },           // 90	Z	(Capital Z )			
        { .key = KEY_LEFTBRACE, .control = false, .shift = false, .makebreak = true },  // 91	[	(square brackets or box brackets)			
        { .key = KEY_BACKSLASH, .control = false, .shift = false, .makebreak = true },  // 92	\	(Backslash)			
        { .key = KEY_RIGHTBRACE, .control = false, .shift = false, .makebreak = true }, // 93	]	(square brackets or box brackets)			
        { .key = KEY_6, .control = false, .shift = true, .makebreak = true },           // 94	^	(Caret or circumflex accent)			
        { .key = KEY_MINUS, .control = false, .shift = true, .makebreak = true },       // 95	_	(underscore , understrike , underbar or low line)			
        { .key = KEY_GRAVE, .control = false, .shift = false, .makebreak = true },      // 96	`	(Grave accent)			
        { .key = KEY_A, .control = false, .shift = false, .makebreak = true },          // 97	a	(Lowercase  a )			
        { .key = KEY_B, .control = false, .shift = false, .makebreak = true },          // 98	b	(Lowercase  b )			
        { .key = KEY_C, .control = false, .shift = false, .makebreak = true },          // 99	c	(Lowercase  c )			
        { .key = KEY_D, .control = false, .shift = false, .makebreak = true },          // 100	d	(Lowercase  d )			
        { .key = KEY_E, .control = false, .shift = false, .makebreak = true },          // 101	e	(Lowercase  e )			
        { .key = KEY_F, .control = false, .shift = false, .makebreak = true },          // 102	f	(Lowercase  f )			
        { .key = KEY_G, .control = false, .shift = false, .makebreak = true },          // 103	g	(Lowercase  g )			
        { .key = KEY_H, .control = false, .shift = false, .makebreak = true },          // 104	h	(Lowercase  h )			
        { .key = KEY_I, .control = false, .shift = false, .makebreak = true },          // 105	i	(Lowercase  i )			
        { .key = KEY_J, .control = false, .shift = false, .makebreak = true },          // 106	j	(Lowercase  j )			
        { .key = KEY_K, .control = false, .shift = false, .makebreak = true },          // 107	k	(Lowercase  k )			
        { .key = KEY_L, .control = false, .shift = false, .makebreak = true },          // 108	l	(Lowercase  l )			
        { .key = KEY_M, .control = false, .shift = false, .makebreak = true },          // 109	m	(Lowercase  m )			
        { .key = KEY_N, .control = false, .shift = false, .makebreak = true },          // 110	n	(Lowercase  n )			
        { .key = KEY_O, .control = false, .shift = false, .makebreak = true },          // 111	o	(Lowercase  o )			
        { .key = KEY_P, .control = false, .shift = false, .makebreak = true },          // 112	p	(Lowercase  p )			
        { .key = KEY_Q, .control = false, .shift = false, .makebreak = true },          // 113	q	(Lowercase  q )			
        { .key = KEY_R, .control = false, .shift = false, .makebreak = true },          // 114	r	(Lowercase  r )			
        { .key = KEY_S, .control = false, .shift = false, .makebreak = true },          // 115	s	(Lowercase  s )			
        { .key = KEY_T, .control = false, .shift = false, .makebreak = true },          // 116	t	(Lowercase  t )			
        { .key = KEY_U, .control = false, .shift = false, .makebreak = true },          // 117	u	(Lowercase  u )			
        { .key = KEY_V, .control = false, .shift = false, .makebreak = true },          // 118	v	(Lowercase  v )			
        { .key = KEY_W, .control = false, .shift = false, .makebreak = true },          // 119	w	(Lowercase  w )			
        { .key = KEY_X, .control = false, .shift = false, .makebreak = true },          // 120	x	(Lowercase  x )			
        { .key = KEY_Y, .control = false, .shift = false, .makebreak = true },          // 121	y	(Lowercase  y )			
        { .key = KEY_Z, .control = false, .shift = false, .makebreak = true },          // 122	z	(Lowercase  z )			
        { .key = KEY_LEFTBRACE, .control = false, .shift = true, .makebreak = true },   // 123	{	(curly brackets or braces)			
        { .key = KEY_BACKSLASH, .control = false, .shift = false, .makebreak = true },  // 124	|	(vertical-bar, vbar, vertical line or vertical slash)			
        { .key = KEY_RIGHTBRACE, .control = false, .shift = true, .makebreak = true },  // 125	}	(curly brackets or braces)			
        { .key = KEY_GRAVE, .control = false, .shift = true, .makebreak = true },       // 126	~	(Tilde ; swung dash)			
        { .key = KEY_DELETE, .control = false, .shift = false, .makebreak = true },     // 127	DEL	(Delete)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 128	Ç	(Majuscule C-cedilla )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 129	ü	(letter "u" with umlaut or diaeresis ; "u-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 130	é	(letter "e" with acute accent or "e-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 131	â	(letter "a" with circumflex accent or "a-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 132	ä	(letter "a" with umlaut or diaeresis ; "a-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 133	à	(letter "a" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 134	å	(letter "a"  with a ring)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 135	ç	(Minuscule c-cedilla)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 136	ê	(letter "e" with circumflex accent or "e-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 137	ë	(letter "e" with umlaut or diaeresis ; "e-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 138	è	(letter "e" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 139	ï	(letter "i" with umlaut or diaeresis ; "i-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 140	î	(letter "i" with circumflex accent or "i-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 141	ì	(letter "i" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 142	Ä	(letter "A" with umlaut or diaeresis ; "A-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 143	Å	(letter "A"  with a ring)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 144	É	(Capital letter "E" with acute accent or "E-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 145	æ	(Latin diphthong "ae")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 146	Æ	(Latin diphthong "AE")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 147	ô	(letter "o" with circumflex accent or "o-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 148	ö	(letter "o" with umlaut or diaeresis ; "o-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 149	ò	(letter "o" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 150	û	(letter "u" with circumflex accent or "u-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 151	ù	(letter "u" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 152	ÿ	(letter "y" with diaeresis)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 153	Ö	(letter "O" with umlaut or diaeresis ; "O-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 154	Ü	(letter "U" with umlaut or diaeresis ; "U-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 155	ø	(slashed zero or empty set)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 156	£	(Pound sign ; symbol for the pound sterling)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 157	Ø	(slashed zero or empty set)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 158	×	(multiplication sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 159	ƒ	(function sign ; f with hook sign ; florin sign )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 160	á	(letter "a" with acute accent or "a-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 161	í	(letter "i" with acute accent or "i-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 162	ó	(letter "o" with acute accent or "o-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 163	ú	(letter "u" with acute accent or "u-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 164	ñ	(letter "n" with tilde ; enye)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 165	Ñ	(letter "N" with tilde ; enye)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 166	ª	(feminine ordinal indicator )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 167	º	(masculine ordinal indicator)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 168	¿	(Inverted question marks)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 169	®	(Registered trademark symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 170	¬	(Logical negation symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 171	½	(One half)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 172	¼	(Quarter or  one fourth)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 173	¡	(Inverted exclamation marks)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 174	«	(Guillemets or  angle quotes)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 175	»	(Guillemets or  angle quotes)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 176	░				
        { .key = KEY_KP0, .control = false, .shift = false, .makebreak = true },        // 177	▒				
        { .key = KEY_KPDOT, .control = false, .shift = false, .makebreak = true },      // 178	▓				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 179	│	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 180	┤	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 181	Á	(Capital letter "A" with acute accent or "A-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 182	Â	(letter "A" with circumflex accent or "A-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 183	À	(letter "A" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 184	©	(Copyright symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 185	╣	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 186	║	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 187	╗	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 188	╝	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 189	¢	(Cent symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 190	¥	(YEN and YUAN sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 191	┐	(Box drawing character)			
        { .key = KEY_KP1, .control = false, .shift = false, .makebreak = true },        // 192	└	(Box drawing character)			
        { .key = KEY_KP2, .control = false, .shift = false, .makebreak = true },        // 193	┴	(Box drawing character)			
        { .key = KEY_KP3, .control = false, .shift = false, .makebreak = true },        // 194	┬	(Box drawing character)			
        { .key = KEY_KPENTER, .control = false, .shift = false, .makebreak = true },    // 195	├	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 196	─	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 197	┼	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 198	ã	(letter "a" with tilde or "a-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 199	Ã	(letter "A" with tilde or "A-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 200	╚	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 201	╔	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 202	╩	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 203	╦	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 204	╠	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 205	═	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 206	╬	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 207	¤	(generic currency sign )			
        { .key = KEY_KP4, .control = false, .shift = false, .makebreak = true },        // 208	ð	(lowercase "eth")			
        { .key = KEY_KP5, .control = false, .shift = false, .makebreak = true },        // 209	Ð	(Capital letter "Eth")			
        { .key = KEY_KP6, .control = false, .shift = false, .makebreak = true },        // 210	Ê	(letter "E" with circumflex accent or "E-circumflex")			
        { .key = KEY_KPCOMMA, .control = false, .shift = false, .makebreak = true },    // 211	Ë	(letter "E" with umlaut or diaeresis ; "E-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 212	È	(letter "E" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 213	ı	(lowercase dot less i)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 214	Í	(Capital letter "I" with acute accent or "I-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 215	Î	(letter "I" with circumflex accent or "I-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 216	Ï	(letter "I" with umlaut or diaeresis ; "I-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 217	┘	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 218	┌	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 219	█	(Block)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 220	▄				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 221	¦	(vertical broken bar )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 222	Ì	(letter "I" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 223	▀				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 224	Ó	(Capital letter "O" with acute accent or "O-acute")			
        { .key = KEY_KP7, .control = false, .shift = false, .makebreak = true },        // 225	ß	(letter "Eszett" ; "scharfes S" or "sharp S")			
        { .key = KEY_KP8, .control = false, .shift = false, .makebreak = true },        // 226	Ô	(letter "O" with circumflex accent or "O-circumflex")			
        { .key = KEY_KP9, .control = false, .shift = false, .makebreak = true },        // 227	Ò	(letter "O" with grave accent)			
        { .key = KEY_KPMINUS, .control = false, .shift = false, .makebreak = true },    // 228	õ	(letter "o" with tilde or "o-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 229	Õ	(letter "O" with tilde or "O-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 230	µ	(Lowercase letter "Mu" ; micro sign or micron)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 231	þ	(capital letter "Thorn")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 232	Þ	(lowercase letter "thorn")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 233	Ú	(Capital letter "U" with acute accent or "U-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 234	Û	(letter "U" with circumflex accent or "U-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 235	Ù	(letter "U" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 236	ý	(letter "y" with acute accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 237	Ý	(Capital letter "Y" with acute accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 238	¯	(macron symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 239	´	(Acute accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 240	¬	(Hyphen)			
        { .key = KEY_UP, .control = false, .shift = false, .makebreak = true },         // 241	±	(Plus-minus sign)			
        { .key = KEY_DOWN, .control = false, .shift = false, .makebreak = true },       // 242	‗	(underline or underscore)			
        { .key = KEY_LEFT, .control = false, .shift = false, .makebreak = true },       // 243	¾	(three quarters)			
        { .key = KEY_RIGHT, .control = false, .shift = false, .makebreak = true },      // 244	¶	(paragraph sign or pilcrow)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 245	§	(Section sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 246	÷	(The division sign ; Obelus)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 247	¸	(cedilla)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 248	°	(degree symbol )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 249	¨	(Diaeresis)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 250	•	(Interpunct or space dot)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 251	¹	(superscript one)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 252	³	(cube or superscript three)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 253	²	(Square or superscript two)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 254	■	(black square)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true }    // 255	nbsp	(non-breaking space or no-break space)
    },
    { // ASCII Keymap -----------------------------------------------------------------------------------------------------------------------------------------
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 0	NULL(Null character)			
        { .key = KEY_A, .control = true, .shift = false, .makebreak = false },           // 1	SOH	(Start of Header)
        { .key = KEY_B, .control = true, .shift = false, .makebreak = false },           // 2	STX	(Start of Text)			
        { .key = KEY_C, .control = true, .shift = false, .makebreak = false },           // 3	ETX	(End of Text)			
        { .key = KEY_D, .control = true, .shift = false, .makebreak = false },           // 4	EOT	(End of Transmission)			
        { .key = KEY_E, .control = true, .shift = false, .makebreak = false },           // 5	ENQ	(Enquiry)			
        { .key = KEY_F, .control = true, .shift = false, .makebreak = false },           // 6	ACK	(Acknowledgement)			
        { .key = KEY_G, .control = true, .shift = false, .makebreak = false },           // 7	BEL	(Bell)			
        { .key = KEY_H, .control = true, .shift = false, .makebreak = false },           // 8	BS	(Backspace)			
        { .key = KEY_I, .control = true, .shift = false, .makebreak = false },           // 9	HT	(Horizontal Tab)			
        { .key = KEY_J, .control = true, .shift = false, .makebreak = false },           // 10	LF	(Line feed)			
        { .key = KEY_K, .control = true, .shift = false, .makebreak = false },           // 11	VT	(Vertical Tab)			
        { .key = KEY_L, .control = true, .shift = false, .makebreak = false },           // 12	FF	(Form feed)			
        { .key = KEY_M, .control = true, .shift = false, .makebreak = false },           // 13	CR	(Carriage return)			
        { .key = KEY_N, .control = true, .shift = false, .makebreak = false },           // 14	SO	(Shift Out)			
        { .key = KEY_O, .control = true, .shift = false, .makebreak = false },           // 15	SI	(Shift In)			
        { .key = KEY_P, .control = true, .shift = false, .makebreak = false },           // 16	DLE	(Data link escape)			
        { .key = KEY_Q, .control = true, .shift = false, .makebreak = false },           // 17	DC1	(Device control 1)			
        { .key = KEY_R, .control = true, .shift = false, .makebreak = false },           // 18	DC2	(Device control 2)			
        { .key = KEY_S, .control = true, .shift = false, .makebreak = false },           // 19	DC3	(Device control 3)			
        { .key = KEY_T, .control = true, .shift = false, .makebreak = false },           // 20	DC4	(Device control 4)			
        { .key = KEY_U, .control = true, .shift = false, .makebreak = false },           // 21	NAK	(Negative acknowledgement)			
        { .key = KEY_V, .control = true, .shift = false, .makebreak = false },           // 22	SYN	(Synchronous idle)			
        { .key = KEY_W, .control = true, .shift = false, .makebreak = false },           // 23	ETB	(End of transmission block)			
        { .key = KEY_X, .control = true, .shift = false, .makebreak = false },           // 24	CAN	(Cancel)			
        { .key = KEY_Y, .control = true, .shift = false, .makebreak = false },           // 25	EM	(End of medium)			
        { .key = KEY_Z, .control = true, .shift = false, .makebreak = false },           // 26	SUB	(Substitute)			
        { .key = KEY_LEFTBRACE, .control = true, .shift = false, .makebreak = false },   // 27	ESC	(Escape)			
        { .key = KEY_BACKSLASH, .control = true, .shift = false, .makebreak = false },   // 28	FS	(File separator)			
        { .key = KEY_RIGHTBRACE, .control = true, .shift = false, .makebreak = false },  // 29	GS	(Group separator)			
        { .key = KEY_6, .control = true, .shift = true, .makebreak = false },            // 30	RS	(Record separator)			
        { .key = KEY_MINUS, .control = true, .shift = true, .makebreak = false },        // 31	US	(Unit separator)			
        { .key = KEY_SPACE, .control = false, .shift = false, .makebreak = false },      // 32	 	(space)			
        { .key = KEY_1, .control = false, .shift = true, .makebreak = false },           // 33	!	(exclamation mark)			
        { .key = KEY_APOSTROPHE, .control = false, .shift = true, .makebreak = false },  // 34	"	(Quotation mark)			
        { .key = KEY_3, .control = false, .shift = true, .makebreak = false },           // 35	#	(Number sign)			
        { .key = KEY_4, .control = false, .shift = true, .makebreak = false },           // 36	$	(Dollar sign)			
        { .key = KEY_5, .control = false, .shift = true, .makebreak = false },           // 37	%	(Percent sign)			
        { .key = KEY_7, .control = false, .shift = true, .makebreak = false },           // 38	&	(Ampersand)			
        { .key = KEY_APOSTROPHE, .control = false, .shift = false, .makebreak = false }, // 39	'	(Apostrophe)			
        { .key = KEY_9, .control = false, .shift = true, .makebreak = false },           // 40	(	(round brackets or parentheses)			
        { .key = KEY_0, .control = false, .shift = true, .makebreak = false },           // 41	)	(round brackets or parentheses)			
        { .key = KEY_8, .control = false, .shift = true, .makebreak = false },           // 42	*	(Asterisk)			
        { .key = KEY_EQUAL, .control = false, .shift = true, .makebreak = false },       // 43	+	(Plus sign)			
        { .key = KEY_COMMA, .control = false, .shift = false, .makebreak = false },      // 44	,	(Comma)			
        { .key = KEY_MINUS, .control = false, .shift = true, .makebreak = false },       // 45	-	(Hyphen)			
        { .key = KEY_DOT, .control = false, .shift = false, .makebreak = false },        // 46	.	(Full stop , dot)			
        { .key = KEY_SLASH, .control = false, .shift = false, .makebreak = false },      // 47	/	(Slash)			
        { .key = KEY_0, .control = false, .shift = false, .makebreak = false },          // 48	0	(number zero)			
        { .key = KEY_1, .control = false, .shift = false, .makebreak = false },          // 49	1	(number one)			
        { .key = KEY_2, .control = false, .shift = false, .makebreak = false },          // 50	2	(number two)			
        { .key = KEY_3, .control = false, .shift = false, .makebreak = false },          // 51	3	(number three)			
        { .key = KEY_4, .control = false, .shift = false, .makebreak = false },          // 52	4	(number four)			
        { .key = KEY_5, .control = false, .shift = false, .makebreak = false },          // 53	5	(number five)			
        { .key = KEY_6, .control = false, .shift = false, .makebreak = false },          // 54	6	(number six)			
        { .key = KEY_7, .control = false, .shift = false, .makebreak = false },          // 55	7	(number seven)			
        { .key = KEY_8, .control = false, .shift = false, .makebreak = false },          // 56	8	(number eight)			
        { .key = KEY_9, .control = false, .shift = false, .makebreak = false },          // 57	9	(number nine)			
        { .key = KEY_SEMICOLON, .control = false, .shift = true, .makebreak = false },   // 58	:	(Colon)			
        { .key = KEY_SEMICOLON, .control = false, .shift = false, .makebreak = false },  // 59	;	(Semicolon)			
        { .key = KEY_COMMA, .control = false, .shift = true, .makebreak = false },       // 60	<	(Less-than sign )			
        { .key = KEY_EQUAL, .control = false, .shift = false, .makebreak = false },      // 61	=	(Equals sign)			
        { .key = KEY_DOT, .control = false, .shift = true, .makebreak = false },         // 62	>	(Greater-than sign ; Inequality) 			
        { .key = KEY_SLASH, .control = false, .shift = true, .makebreak = false },       // 63	?	(Question mark)			
        { .key = KEY_2, .control = false, .shift = true, .makebreak = false },           // 64	@	(At sign)			
        { .key = KEY_A, .control = false, .shift = true, .makebreak = false },           // 65	A	(Capital A )			
        { .key = KEY_B, .control = false, .shift = true, .makebreak = false },           // 66	B	(Capital B )			
        { .key = KEY_C, .control = false, .shift = true, .makebreak = false },           // 67	C	(Capital C )			
        { .key = KEY_D, .control = false, .shift = true, .makebreak = false },           // 68	D	(Capital D )			
        { .key = KEY_E, .control = false, .shift = true, .makebreak = false },           // 69	E	(Capital E )			
        { .key = KEY_F, .control = false, .shift = true, .makebreak = false },           // 70	F	(Capital F )			
        { .key = KEY_G, .control = false, .shift = true, .makebreak = false },           // 71	G	(Capital G )			
        { .key = KEY_H, .control = false, .shift = true, .makebreak = false },           // 72	H	(Capital H )			
        { .key = KEY_I, .control = false, .shift = true, .makebreak = false },           // 73	I	(Capital I )			
        { .key = KEY_J, .control = false, .shift = true, .makebreak = false },           // 74	J	(Capital J )			
        { .key = KEY_K, .control = false, .shift = true, .makebreak = false },           // 75	K	(Capital K )			
        { .key = KEY_L, .control = false, .shift = true, .makebreak = false },           // 76	L	(Capital L )			
        { .key = KEY_M, .control = false, .shift = true, .makebreak = false },           // 77	M	(Capital M )			
        { .key = KEY_N, .control = false, .shift = true, .makebreak = false },           // 78	N	(Capital N )			
        { .key = KEY_O, .control = false, .shift = true, .makebreak = false },           // 79	O	(Capital O )			
        { .key = KEY_P, .control = false, .shift = true, .makebreak = false },           // 80	P	(Capital P )			
        { .key = KEY_Q, .control = false, .shift = true, .makebreak = false },           // 81	Q	(Capital Q )			
        { .key = KEY_R, .control = false, .shift = true, .makebreak = false },           // 82	R	(Capital R )			
        { .key = KEY_S, .control = false, .shift = true, .makebreak = false },           // 83	S	(Capital S )			
        { .key = KEY_T, .control = false, .shift = true, .makebreak = false },           // 84	T	(Capital T )			
        { .key = KEY_U, .control = false, .shift = true, .makebreak = false },           // 85	U	(Capital U )			
        { .key = KEY_V, .control = false, .shift = true, .makebreak = false },           // 86	V	(Capital V )			
        { .key = KEY_W, .control = false, .shift = true, .makebreak = false },           // 87	W	(Capital W )			
        { .key = KEY_X, .control = false, .shift = true, .makebreak = false },           // 88	X	(Capital X )			
        { .key = KEY_Y, .control = false, .shift = true, .makebreak = false },           // 89	Y	(Capital Y )			
        { .key = KEY_Z, .control = false, .shift = true, .makebreak = false },           // 90	Z	(Capital Z )			
        { .key = KEY_LEFTBRACE, .control = false, .shift = false, .makebreak = false },  // 91	[	(square brackets or box brackets)			
        { .key = KEY_BACKSLASH, .control = false, .shift = false, .makebreak = false },  // 92	\	(Backslash)			
        { .key = KEY_RIGHTBRACE, .control = false, .shift = false, .makebreak = false }, // 93	]	(square brackets or box brackets)			
        { .key = KEY_6, .control = false, .shift = true, .makebreak = false },           // 94	^	(Caret or circumflex accent)			
        { .key = KEY_MINUS, .control = false, .shift = true, .makebreak = false },       // 95	_	(underscore , understrike , underbar or low line)			
        { .key = KEY_GRAVE, .control = false, .shift = false, .makebreak = false },      // 96	`	(Grave accent)			
        { .key = KEY_A, .control = false, .shift = false, .makebreak = false },          // 97	a	(Lowercase  a )			
        { .key = KEY_B, .control = false, .shift = false, .makebreak = false },          // 98	b	(Lowercase  b )			
        { .key = KEY_C, .control = false, .shift = false, .makebreak = false },          // 99	c	(Lowercase  c )			
        { .key = KEY_D, .control = false, .shift = false, .makebreak = false },          // 100	d	(Lowercase  d )			
        { .key = KEY_E, .control = false, .shift = false, .makebreak = false },          // 101	e	(Lowercase  e )			
        { .key = KEY_F, .control = false, .shift = false, .makebreak = false },          // 102	f	(Lowercase  f )			
        { .key = KEY_G, .control = false, .shift = false, .makebreak = false },          // 103	g	(Lowercase  g )			
        { .key = KEY_H, .control = false, .shift = false, .makebreak = false },          // 104	h	(Lowercase  h )			
        { .key = KEY_I, .control = false, .shift = false, .makebreak = false },          // 105	i	(Lowercase  i )			
        { .key = KEY_J, .control = false, .shift = false, .makebreak = false },          // 106	j	(Lowercase  j )			
        { .key = KEY_K, .control = false, .shift = false, .makebreak = false },          // 107	k	(Lowercase  k )			
        { .key = KEY_L, .control = false, .shift = false, .makebreak = false },          // 108	l	(Lowercase  l )			
        { .key = KEY_M, .control = false, .shift = false, .makebreak = false },          // 109	m	(Lowercase  m )			
        { .key = KEY_N, .control = false, .shift = false, .makebreak = false },          // 110	n	(Lowercase  n )			
        { .key = KEY_O, .control = false, .shift = false, .makebreak = false },          // 111	o	(Lowercase  o )			
        { .key = KEY_P, .control = false, .shift = false, .makebreak = false },          // 112	p	(Lowercase  p )			
        { .key = KEY_Q, .control = false, .shift = false, .makebreak = false },          // 113	q	(Lowercase  q )			
        { .key = KEY_R, .control = false, .shift = false, .makebreak = false },          // 114	r	(Lowercase  r )			
        { .key = KEY_S, .control = false, .shift = false, .makebreak = false },          // 115	s	(Lowercase  s )			
        { .key = KEY_T, .control = false, .shift = false, .makebreak = false },          // 116	t	(Lowercase  t )			
        { .key = KEY_U, .control = false, .shift = false, .makebreak = false },          // 117	u	(Lowercase  u )			
        { .key = KEY_V, .control = false, .shift = false, .makebreak = false },          // 118	v	(Lowercase  v )			
        { .key = KEY_W, .control = false, .shift = false, .makebreak = false },          // 119	w	(Lowercase  w )			
        { .key = KEY_X, .control = false, .shift = false, .makebreak = false },          // 120	x	(Lowercase  x )			
        { .key = KEY_Y, .control = false, .shift = false, .makebreak = false },          // 121	y	(Lowercase  y )			
        { .key = KEY_Z, .control = false, .shift = false, .makebreak = false },          // 122	z	(Lowercase  z )			
        { .key = KEY_LEFTBRACE, .control = false, .shift = true, .makebreak = false },   // 123	{	(curly brackets or braces)			
        { .key = KEY_BACKSLASH, .control = false, .shift = false, .makebreak = false },  // 124	|	(vertical-bar, vbar, vertical line or vertical slash)			
        { .key = KEY_RIGHTBRACE, .control = false, .shift = true, .makebreak = false },  // 125	}	(curly brackets or braces)			
        { .key = KEY_GRAVE, .control = false, .shift = true, .makebreak = false },       // 126	~	(Tilde ; swung dash)			
        { .key = KEY_DELETE, .control = false, .shift = false, .makebreak = false },     // 127	DEL	(Delete)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 128	Ç	(Majuscule C-cedilla )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 129	ü	(letter "u" with umlaut or diaeresis ; "u-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 130	é	(letter "e" with acute accent or "e-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 131	â	(letter "a" with circumflex accent or "a-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 132	ä	(letter "a" with umlaut or diaeresis ; "a-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 133	à	(letter "a" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 134	å	(letter "a"  with a ring)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 135	ç	(Minuscule c-cedilla)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 136	ê	(letter "e" with circumflex accent or "e-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 137	ë	(letter "e" with umlaut or diaeresis ; "e-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 138	è	(letter "e" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 139	ï	(letter "i" with umlaut or diaeresis ; "i-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 140	î	(letter "i" with circumflex accent or "i-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 141	ì	(letter "i" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 142	Ä	(letter "A" with umlaut or diaeresis ; "A-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 143	Å	(letter "A"  with a ring)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 144	É	(Capital letter "E" with acute accent or "E-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 145	æ	(Latin diphthong "ae")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 146	Æ	(Latin diphthong "AE")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 147	ô	(letter "o" with circumflex accent or "o-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 148	ö	(letter "o" with umlaut or diaeresis ; "o-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 149	ò	(letter "o" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 150	û	(letter "u" with circumflex accent or "u-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 151	ù	(letter "u" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 152	ÿ	(letter "y" with diaeresis)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 153	Ö	(letter "O" with umlaut or diaeresis ; "O-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 154	Ü	(letter "U" with umlaut or diaeresis ; "U-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 155	ø	(slashed zero or empty set)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 156	£	(Pound sign ; symbol for the pound sterling)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 157	Ø	(slashed zero or empty set)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 158	×	(multiplication sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 159	ƒ	(function sign ; f with hook sign ; florin sign )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 160	á	(letter "a" with acute accent or "a-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 161	í	(letter "i" with acute accent or "i-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 162	ó	(letter "o" with acute accent or "o-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 163	ú	(letter "u" with acute accent or "u-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 164	ñ	(letter "n" with tilde ; enye)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 165	Ñ	(letter "N" with tilde ; enye)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 166	ª	(feminine ordinal indicator )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 167	º	(masculine ordinal indicator)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 168	¿	(Inverted question marks)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 169	®	(Registered trademark symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 170	¬	(Logical negation symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 171	½	(One half)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 172	¼	(Quarter or  one fourth)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 173	¡	(Inverted exclamation marks)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 174	«	(Guillemets or  angle quotes)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 175	»	(Guillemets or  angle quotes)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 176	░				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 177	▒				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 178	▓				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 179	│	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 180	┤	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 181	Á	(Capital letter "A" with acute accent or "A-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 182	Â	(letter "A" with circumflex accent or "A-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 183	À	(letter "A" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 184	©	(Copyright symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 185	╣	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 186	║	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 187	╗	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 188	╝	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 189	¢	(Cent symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 190	¥	(YEN and YUAN sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 191	┐	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 192	└	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 193	┴	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 194	┬	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 195	├	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 196	─	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 197	┼	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 198	ã	(letter "a" with tilde or "a-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 199	Ã	(letter "A" with tilde or "A-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 200	╚	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 201	╔	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 202	╩	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 203	╦	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 204	╠	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 205	═	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 206	╬	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 207	¤	(generic currency sign )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 208	ð	(lowercase "eth")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 209	Ð	(Capital letter "Eth")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 210	Ê	(letter "E" with circumflex accent or "E-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 211	Ë	(letter "E" with umlaut or diaeresis ; "E-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 212	È	(letter "E" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 213	ı	(lowercase dot less i)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 214	Í	(Capital letter "I" with acute accent or "I-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 215	Î	(letter "I" with circumflex accent or "I-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 216	Ï	(letter "I" with umlaut or diaeresis ; "I-umlaut")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 217	┘	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 218	┌	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 219	█	(Block)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 220	▄				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 221	¦	(vertical broken bar )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 222	Ì	(letter "I" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 223	▀				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 224	Ó	(Capital letter "O" with acute accent or "O-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 225	ß	(letter "Eszett" ; "scharfes S" or "sharp S")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 226	Ô	(letter "O" with circumflex accent or "O-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 227	Ò	(letter "O" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 228	õ	(letter "o" with tilde or "o-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 229	Õ	(letter "O" with tilde or "O-tilde")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 230	µ	(Lowercase letter "Mu" ; micro sign or micron)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 231	þ	(capital letter "Thorn")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 232	Þ	(lowercase letter "thorn")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 233	Ú	(Capital letter "U" with acute accent or "U-acute")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 234	Û	(letter "U" with circumflex accent or "U-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 235	Ù	(letter "U" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 236	ý	(letter "y" with acute accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 237	Ý	(Capital letter "Y" with acute accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 238	¯	(macron symbol)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 239	´	(Acute accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 240	¬	(Hyphen)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 241	±	(Plus-minus sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 242	‗	(underline or underscore)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 243	¾	(three quarters)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 244	¶	(paragraph sign or pilcrow)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 245	§	(Section sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 246	÷	(The division sign ; Obelus)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 247	¸	(cedilla)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 248	°	(degree symbol )			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 249	¨	(Diaeresis)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 250	•	(Interpunct or space dot)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 251	¹	(superscript one)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 252	³	(cube or superscript three)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 253	²	(Square or superscript two)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 254	■	(black square)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false }    // 255	nbsp	(non-breaking space or no-break space)
    },
    { // Media Keymap -----------------------------------------------------------------------------------------------------------------------------------------
        { .key = KEY_MUTE, .control = false, .shift = false, .makebreak = false },           // 0
        { .key = KEY_VOLUMEUP, .control = false, .shift = false, .makebreak = false },       // 1
        { .key = KEY_VOLUMEDOWN, .control = false, .shift = false, .makebreak = false },     // 2
        { .key = KEY_PLAYPAUSE, .control = false, .shift = false, .makebreak = false },      // 3
        { .key = KEY_NEXTSONG, .control = false, .shift = false, .makebreak = false },       // 4
        { .key = KEY_PREVIOUSSONG, .control = false, .shift = false, .makebreak = false },   // 5
        { .key = KEY_RECORD, .control = false, .shift = false, .makebreak = false },         // 6
        { .key = KEY_REWIND, .control = false, .shift = false, .makebreak = false },         // 7
        { .key = KEY_FORWARD, .control = false, .shift = false, .makebreak = false },        // 8
        { .key = KEY_PLAYCD, .control = false, .shift = false, .makebreak = false },         // 9
        { .key = KEY_PAUSECD, .control = false, .shift = false, .makebreak = false },        // 10
        { .key = KEY_STOPCD, .control = false, .shift = false, .makebreak = false },         // 11
        { .key = KEY_EJECTCD, .control = false, .shift = false, .makebreak = false },        // 12
        { .key = KEY_CLOSECD, .control = false, .shift = false, .makebreak = false },        // 13
        { .key = KEY_EJECTCLOSECD, .control = false, .shift = false, .makebreak = false },   // 14
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 15
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 16
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 17
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 18
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 19
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 20
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 21
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 22
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 23
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 24
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 25
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 26
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 27
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 28
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 29
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 30
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 31
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 32
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 33
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 34
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 35
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 36
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 37
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 38
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 39
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 40
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 41
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 42
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 43
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 44
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 45
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 46
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 47
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 48
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 49
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 50
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 51
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 52
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 53
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 54
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 55
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 56
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 57
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 58
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 59
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 60
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 61
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 62
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 63
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 64
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 65
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 66
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 67
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 68
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 69
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 70
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 71
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 72
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 73
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 74
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 75
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 76
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 77
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 78
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 79
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 80
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 81
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 82
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 83
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 84
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 85
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 86
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 87
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 88
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 89
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 90
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 91
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 92
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 93
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 94
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 95
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 96
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 97
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 98
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 99
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 100
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 101
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 102
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 103
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 104
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 105
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 106
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 107
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 108
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 109
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 110
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 111
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 112
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 113
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 114
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 115
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 116
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 117
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 118
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 119
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 120
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 121
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 122
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 123
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 124
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 125
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 126
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 127
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 128
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 129
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 130
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 131
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 132
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 133
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 134
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 135
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 136
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 137
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 138
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 139
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 140
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 141
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 142
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 143
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 144
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 145
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 146
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 147
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 148
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 149
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 150
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 151
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 152
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 153
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 154
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 155
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 156
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 157
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 158
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 159
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 160
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 161
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 162
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 163
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 164
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 165
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 166
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 167
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 168
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 169
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 170
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 171
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 172
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 173
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 174
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 175
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 176
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 177
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 178
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 179
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 180
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 181
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 182
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 183
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 184
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 185
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 186
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 187
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 188
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 189
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 190
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 191
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 192
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 193
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 194
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 195
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 196
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 197
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 198
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 199
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 200
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 201
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 202
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 203
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 204
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 205
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 206
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 207
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 208
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 209
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 210
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 211
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 212
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 213
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 214
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 215
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 216
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 217
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 218
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 219
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 220
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 221
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 222
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 223
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 224
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 225
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 226
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 227
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 228
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 229
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 230
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 231
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 232
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 233
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 234
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 235
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 236
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 237
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 238
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 239
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 240
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 241
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 242
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 243
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 244
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 245
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 246
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 247
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 248
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 249
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 250
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 251
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 252
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 253
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 254
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false }        // 255
   },
    { // Custom Keymap ---------------------------------------------------------------------------------------------------------------------------------------
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 0
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 1
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 2
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 3
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 4
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 5
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 6
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 7
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 8
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 9
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 10
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 11
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 12
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 13
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 14
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 15
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 16
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 17
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 18
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 19
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 20
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 21
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 22
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 23
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 24
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 25
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 26
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 27
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 28
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 29
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 30
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 31
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 32
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 33
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 34
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 35
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 36
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 37
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 38
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 39
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 40
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 41
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 42
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 43
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 44
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 45
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 46
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 47
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 48
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 49
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 50
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 51
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 52
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 53
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 54
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 55
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 56
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 57
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 58
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 59
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 60
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 61
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 62
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 63
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 64
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 65
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 66
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 67
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 68
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 69
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 70
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 71
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 72
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 73
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 74
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 75
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 76
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 77
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 78
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 79
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 80
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 81
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 82
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 83
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 84
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 85
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 86
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 87
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 88
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 89
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 90
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 91
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 92
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 93
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 94
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 95
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 96
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 97
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 98
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 99
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 100
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 101
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 102
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 103
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 104
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 105
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 106
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 107
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 108
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 109
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 110
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 111
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 112
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 113
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 114
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 115
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 116
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 117
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 118
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 119
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 120
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 121
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 122
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 123
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 124
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 125
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 126
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 127
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 128
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 129
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 130
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 131
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 132
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 133
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 134
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 135
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 136
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 137
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 138
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 139
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 140
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 141
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 142
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 143
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 144
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 145
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 146
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 147
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 148
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 149
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 150
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 151
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 152
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 153
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 154
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 155
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 156
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 157
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 158
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 159
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 160
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 161
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 162
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 163
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 164
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 165
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 166
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 167
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 168
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 169
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 170
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 171
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 172
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 173
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 174
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 175
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 176
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 177
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 178
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 179
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 180
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 181
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 182
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 183
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 184
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 185
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 186
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 187
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 188
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 189
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 190
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 191
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 192
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 193
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 194
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 195
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 196
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 197
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 198
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 199
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 200
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 201
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 202
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 203
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 204
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 205
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 206
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 207
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 208
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 209
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 210
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 211
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 212
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 213
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 214
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 215
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 216
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 217
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 218
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 219
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 220
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 221
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 222
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 223
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 224
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 225
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 226
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 227
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 228
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 229
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 230
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 231
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 232
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 233
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 234
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 235
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 236
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 237
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 238
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 239
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 240
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 241
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 242
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 243
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 244
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 245
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 246
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 247
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 248
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 249
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 250
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 251
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 252
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 253
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },       // 254
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false }        // 255
   }
};
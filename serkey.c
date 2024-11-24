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
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

// Macros *********************************************************************
// I've never liked how the static keyword is overloaded in C
// These macros make it more intuitive
#define local      static
#define persistent static

// Constants ******************************************************************
#define MAX_SOCKET_PATH    1024

// Data Types *****************************************************************
typedef enum
{
   KAYPRO,
   ASCII,
   MEDIA_KEYS
}keymaps_t;

typedef struct
{
    char key;
    bool control,shift,makebreak;
}keymap_t;

typedef struct CONFIG
{
   char *baudrate, *parity, *databits, *stopbits;
   keymaps_t keymap;
   char *socket_path,*tty;
}config_t;

// Globals ********************************************************************
local keymap_t keymap[3][256];  // Scroll to the bottom of the file for definition

// Local function prototypes **************************************************
local void parseCommandLine(int argc, char *argv[], config_t *config);
local void emit(int fd, int type, int code, int val);
local void emitKey(int fd, keymap_t *key);
local void displayUsage(FILE *ouput);
local void exitApp(char* error_str, bool display_usage, int return_code);
local void launchTio(config_t *config);
local int connectTio(config_t *config);
local int connectUinput(char *dev_name);

/*
 * Main Entry Point ***********************************************************
 */
int main(int argc, char *argv[])
{
   // Default configuration
   config_t config = {  .baudrate = "300",
                        .parity = "none",
                        .databits = "8",
                        .stopbits = "1",
                        .keymap = KAYPRO,
                        .socket_path = "/tmp/tio-kyb",
                        .tty = NULL};

   // Parse the command line and setup the config
   parseCommandLine(argc, argv, &config);

   // Launch the serial I/O application
   launchTio(&config);
   // Connect to the serial I/O application using a Unix domain socket
   int tio_socket = connectTio(&config);

   // Connect to the uinput kernel module
   int uinput_fd = connectUinput("/dev/uinput");

   // If successfully opened a pipe to tio app...
   if(tio_socket)
   {
      unsigned char  key;
      size_t         count;

      do
      {
         count = read(tio_socket, &key, sizeof(key));

         // If read a key from tio...
         if(count!=0)
         {
            // Display it to stdout
            fprintf(stdout," Key: %c Value: %03d\n\r", (char)key, key);
            fflush(stdout);

            // Send the mapped key code to uinput
            emitKey(uinput_fd, &keymap[config.keymap][key]);
         }
         else
         {
            if(errno!=0)
               exitApp("read returned an error", false, -1);
            else
               exitApp("read returned zero bytes", false, 0);
         }

      } while (key!=27);
      close(tio_socket);
   }
   else
      exitApp(strerror(errno), false, -1);

   /*
    * Give userspace some time to read the events before we destroy the
    * device with UI_DEV_DESTOY.
    */
   sleep(1);

   ioctl(uinput_fd, UI_DEV_DESTROY);
   close(uinput_fd);

   return 0;
}

// Local Functions ************************************************************
/*
 * Parse the application command line and set up the configuration
 */
local void parseCommandLine(int argc, char *argv[], config_t *config)
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
               if(baudrate)
                  config->baudrate = argv[i];
               // Else error...
               else
                  exitApp("Invalid Baudrate", true, -1);
               break;
            case 'p':
               ++i;
               // If valid parity setting...
               if(!strcmp(argv[i],"odd") ||
                  !strcmp(argv[i],"even") ||
                  !strcmp(argv[i],"none") ||
                  !strcmp(argv[i],"mark") ||
                  !strcmp(argv[i],"space"))
               {
                  config->parity=argv[i];
               }
               // Else error...
               else
                  exitApp("Invalid parity", true, -1);
               break;
            case 'd':
               databits = atoi(argv[++i]);
               // If valid data bits...
               if(databits >= 5 && databits <= 9)
                  config->databits = argv[i];
               // Else error...
               else 
                  exitApp("Invalid data bits", true, -1);
               break;
            case 's':
               stopbits = atoi(argv[++i]);
               // If valid stop bits...
               if(stopbits == 1 || stopbits == 2)
                  config->stopbits = argv[i];
               // Else error...
               else
                  exitApp("Invalid stop bits", true, -1);
               break;
            case 'k':
               ++i;
               // If kaypro key map setting...
               if(!strcmp(argv[i],"kaypro"))
                  config->keymap = KAYPRO;
               // Else if media_keys key map setting...
               else if(!strcmp(argv[i],"media_keys"))
                  config->keymap = MEDIA_KEYS;
               // Else if ascii key map setting...
               else if(!strcmp(argv[i],"ascii"))
                  config->keymap = ASCII;
               // Else error...
               else
                  exitApp("Invalid key map", true, -1);
               break;
            case 'h':
            case '?':
               exitApp(NULL, true, 0);
               break;
            default:
               exitApp("Unknown switch", true, -1);
         }
      }
      // Else if a profile has not been provided already... 
      else if(config->tty == NULL)
         config->tty = argv[i];
      // Else don't know what this is...
      else
         exitApp("Unknown parameter", true, -1);
   }

   if(config->tty==NULL)
      exitApp("No serial device provided", true, -1);
}

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

   write(fd, &ie, sizeof(ie));
}

/*
 * Emit a key press to uinput
 */
local void emitKey(int fd, keymap_t *key)
{
   // If control key required...
   if(key->control)
   {
      // Control key make, report the event
      emit(fd, EV_KEY, KEY_LEFTCTRL, 1);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
   // If shift key required...
   if(key->shift)
   {
      // Shift key make, report the event
      emit(fd, EV_KEY, KEY_LEFTSHIFT, 1);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }

   // If make/break required...
   if(key->makebreak)
   {
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
      // Key make or break, report the event
      emit(fd, EV_KEY, key->key && 0x7f, (key->key && 0x80) >> 7);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }

   // If control key required...
   if(key->control)
   {
      // Control key break, report the event
      emit(fd, EV_KEY, KEY_LEFTCTRL, 0);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
   // If shift key required...
   if(key->shift)
   {
      // Shift key break, report the event
      emit(fd, EV_KEY, KEY_LEFTSHIFT, 0);
      emit(fd, EV_SYN, SYN_REPORT, 0);
   }
}

/*
 * Display the application usage w/command line options and exit w/error
 */
local void displayUsage(FILE *output_stream)
{
   fprintf(output_stream, "Usage: kaykey [OPTION]... serial_device\n\n\r"
          "Kaykey is a user mode Kaypro keyboard driver for Linux. It utilizes the uinput\n\r"
          "kernel module and tio serial device I/O tool. Therefore, both must be installed\n\r"
          "and enabled. In addition, Kaykey must be run at a priviledge level capable of\n\r"
          "communicating with uinput. On most distributions, this is root level priviledges\n\r"
          "by default. The serial devie specifies the \\dev tty device connected to the \n\r"
          "keyboard.\n\n\r"
          "OPTIONS:\n\r"
          "  -f,  Provide configuration file name. ie. -f kaykey.conf\n\r"
          "       If no configuration filename is provided, the application will first look\n\r"
          "       for .kaykey.conf file in the current directory. If that file is not found,\n\r"
          "       it will look for it in ~/.congfig/kaykey/kaykey.conf.\n\r"
          "  -h   Display this usage information\n\r");
}

/*
 * Display a message and exit the application with a given return code
 */
local void exitApp(char* error_str, bool display_usage, int return_code)
{
   FILE *output;

   // Is the return code an error...
   if(return_code)
      output = stderr;
   else
      output = stdout;

   // If an error string was provided...
   if(error_str)
      if(strlen(error_str))
         fprintf(output, "Error: %s\n\r",error_str);

   if(display_usage == true)
      displayUsage(output);

   fflush(output);
   exit(return_code);
}

/*
 * Launch tio serial I/O tool to setup and connect to the appropriate serial port
 */
local void launchTio(config_t *config)
{
   // Fork this process
   pid_t pid=fork();
   // If this is the child process...
   if(pid==0)
   {
      // Redirect stdout to /dev/null
      int fd;
      // If opening /dev/null fails...
      if ((fd = open("/dev/null", O_WRONLY)) == -1)
         exitApp("Unable to open /dev/null",false,-1);
      // Redirect standard output to NULL
      dup2(fd, STDOUT_FILENO);
      close(fd);

      // Build the Socket Path string for the args
      char socket_path[MAX_SOCKET_PATH];
      strncpy(socket_path,"unix:",sizeof(socket_path) - 1);
      strncat(socket_path,config->socket_path,sizeof(socket_path) - 1);

      // If Tio failed to launch...
      if(execl("/usr/local/bin/tio","/usr/local/bin/tio","-b",config->baudrate,"-p",config->parity,"-d",config->databits,"-s",config->stopbits,"--mute","--socket",socket_path,config->tty,NULL) == -1)
         exitApp("Unable to launch tio", false, -1);
   }

   // Wait a second for the new instance of tio to start up
   sleep(1);

   printf("Launched tio\r\n");
}

/*
 * Connect to the tio application using a Unix domain socket and return the socket number
 */
local int connectTio(config_t *config)
{
   int tio_socket;
   struct sockaddr_un addr;

   // Create the socket
   tio_socket = socket(AF_UNIX, SOCK_STREAM, 0);

   // If creating a socket failed...
   if(tio_socket == -1)
      exitApp("Failed to create socket",false,-1);

   // Build the socket addr using the path name in config
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   strncpy(addr.sun_path, config->socket_path, sizeof(addr.sun_path) - 1);

   // If failure to connect...
   if(connect(tio_socket, (const struct sockaddr *)&addr, sizeof(addr)) == -1)
      exitApp(strerror(errno), false, -1);

   printf("Connected to tio\r\n");

   return(tio_socket);
}

/*
 * Connect to the uinput kernel module
 */
local int connectUinput(char *dev_name)
{
   /*
    * uinput setup and open a pipe to uinput
    */
   struct uinput_setup usetup;
   int fd = open(dev_name, O_WRONLY | O_NONBLOCK);

   // If unable to open a pipe to uinput...
   if(fd == -1)
   {
      exitApp("Unable to open pipe to uinput. Make sure you have permission to access\n\r"
              "uinput virtual device. Try \"sudo serkey\" to run at root level permissions\n\r"
              , false, -1);
   }

   /*
    * The ioctls below will enable the device that is about to be
    * created, to pass key events, in this case the space key.
    */
   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   ioctl(fd, UI_SET_KEYBIT, KEY_MUTE);

   memset(&usetup, 0, sizeof(usetup));
   usetup.id.bustype = BUS_USB;
   usetup.id.vendor = 0x1234; /* sample vendor */
   usetup.id.product = 0x5678; /* sample product */
   strcpy(usetup.name, "Example device");

   ioctl(fd, UI_DEV_SETUP, &usetup);
   ioctl(fd, UI_DEV_CREATE);

   /*
    * On UI_DEV_CREATE the kernel will create the device node for this
    * device. We are inserting a pause here so that userspace has time
    * to detect, initialize the new device, and can start listening to
    * the event, otherwise it will not notice the event we are about
    * to send. This pause is only needed in our example code!
    */
   sleep(1);

   printf("Connected to uintput\n\r");

   return(fd);
}

// Key Maps *******************************************************************
local keymap_t keymap[3][256] =
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
        { .key = KEY_H, .control = true, .shift = false, .makebreak = true },           // 8	BS	(Backspace)			
        { .key = KEY_I, .control = true, .shift = false, .makebreak = true },           // 9	HT	(Horizontal Tab)			
        { .key = KEY_J, .control = true, .shift = false, .makebreak = true },           // 10	LF	(Line feed)			
        { .key = KEY_K, .control = true, .shift = false, .makebreak = true },           // 11	VT	(Vertical Tab)			
        { .key = KEY_L, .control = true, .shift = false, .makebreak = true },           // 12	FF	(Form feed)			
        { .key = KEY_M, .control = true, .shift = false, .makebreak = true },           // 13	CR	(Carriage return)			
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
        { .key = KEY_X, .control = true, .shift = false, .makebreak = true },           // 24	CAN	(Cancel)			
        { .key = KEY_Y, .control = true, .shift = false, .makebreak = true },           // 25	EM	(End of medium)			
        { .key = KEY_Z, .control = true, .shift = false, .makebreak = true },           // 26	SUB	(Substitute)			
        { .key = KEY_LEFTBRACE, .control = true, .shift = false, .makebreak = true },   // 27	ESC	(Escape)			
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
        { .key = KEY_MINUS, .control = false, .shift = true, .makebreak = true },       // 45	-	(Hyphen)			
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
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 177	▒				
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 178	▓				
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
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 192	└	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 193	┴	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 194	┬	(Box drawing character)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 195	├	(Box drawing character)			
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
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 208	ð	(lowercase "eth")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 209	Ð	(Capital letter "Eth")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 210	Ê	(letter "E" with circumflex accent or "E-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 211	Ë	(letter "E" with umlaut or diaeresis ; "E-umlaut")			
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
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 225	ß	(letter "Eszett" ; "scharfes S" or "sharp S")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 226	Ô	(letter "O" with circumflex accent or "O-circumflex")			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 227	Ò	(letter "O" with grave accent)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 228	õ	(letter "o" with tilde or "o-tilde")			
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
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 241	±	(Plus-minus sign)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 242	‗	(underline or underscore)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 243	¾	(three quarters)			
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 244	¶	(paragraph sign or pilcrow)			
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
        { .key = KEY_MUTE, .control = false, .shift = false, .makebreak = false },          // 0
        { .key = KEY_VOLUMEUP, .control = false, .shift = false, .makebreak = false },      // 1
        { .key = KEY_VOLUMEDOWN, .control = false, .shift = false, .makebreak = false },    // 2
        { .key = KEY_PLAYPAUSE, .control = false, .shift = false, .makebreak = false },     // 3
        { .key = KEY_NEXTSONG, .control = false, .shift = false, .makebreak = false },      // 4
        { .key = KEY_PREVIOUSSONG, .control = false, .shift = false, .makebreak = false },  // 5
        { .key = KEY_RECORD, .control = false, .shift = false, .makebreak = false },        // 6
        { .key = KEY_REWIND, .control = false, .shift = false, .makebreak = false },        // 7
        { .key = KEY_FORWARD, .control = false, .shift = false, .makebreak = false },        // 8
        { .key = KEY_PLAYCD, .control = false, .shift = false, .makebreak = false },        // 9
        { .key = KEY_PAUSECD, .control = false, .shift = false, .makebreak = false },       // 10
        { .key = KEY_STOPCD, .control = false, .shift = false, .makebreak = false },        // 11
        { .key = KEY_EJECTCD, .control = false, .shift = false, .makebreak = false },       // 12
        { .key = KEY_CLOSECD, .control = false, .shift = false, .makebreak = false },       // 13
        { .key = KEY_EJECTCLOSECD, .control = false, .shift = false, .makebreak = false },  // 14
    }
};
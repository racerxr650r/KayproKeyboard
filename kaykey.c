#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Macros *********************************************************************
// I've never liked how the static keyword is overloaded in C
// These macros make it more intuitive
#define local      static
#define persistent static

// Local function prototypes **************************************************
local void emit(int fd, int type, int code, int val);
local void displayUsage(int exitCode);
local char* getConfigFile(void);

// Main ***********************************************************************
int main(int argc, char *argv[])
{
   /*
    * Parse the command line and apply. If there is an error, display the usage
    * and exit.
    */
   char *configFile = NULL;
   char *profile = NULL;

   // For each command line argument...
   for(int i=1;i<=argc-1;++i)
   {
      // If command line switch "-" character...
      if(argv[i][0]=='-')
         // Decode the command line switch and apply...
         switch(argv[i][1])
         {
            case 'f':
               if(isalnum(argv[i][3]))
                  configFile = &argv[i][3];
               else
                  displayUsage(-1);
               break;
            case 'h':
            case '?':
               displayUsage(0);
               break;
            default:
               displayUsage(-1);
         }
      // Else if a profile has not been provided already... 
      else if(!profile)
      {
         profile = argv[i];
      }
      // Else I don't know what this is...
      else
      {
         displayUsage(-1);
      }
   }

   // If configFile has not been specified...
   if(!configFile)
      configFile = getConfigFile();

   /*
    * Start tio serial I/O tool to setup and connect to the appropriate serial port
    */

   /*
    * uinput setup and open a pipe to uinput
    */
   struct uinput_setup usetup;
   int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

   // If unable to open a pipe to uinput...
   if(!fd)
   {
      printf("Error: Unable to open pipe to uinput. Make sure you have permission to access\n\r"
             "       uinput virtual device. Try \"sudo kaykey\" to run at root level\n\r"
             "       permissions.\n\r");
      exit(-1);
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

   /* Key press, report the event, send key release, and report again */
   emit(fd, EV_KEY, KEY_MUTE, 1);
   emit(fd, EV_SYN, SYN_REPORT, 0);
   emit(fd, EV_KEY, KEY_MUTE, 0);
   emit(fd, EV_SYN, SYN_REPORT, 0);

   /*
    * Give userspace some time to read the events before we destroy the
    * device with UI_DEV_DESTOY.
    */
   sleep(1);

   ioctl(fd, UI_DEV_DESTROY);
   close(fd);

   return 0;
}

// Local Functions ************************************************************
/*
 * Emit an event to the uinput virtual device 
 */
local void emit(int fd, int type, int code, int val)
{
   struct input_event ie;

   ie.type = type;
   ie.code = code;
   ie.value = val;
   /* timestamp values below are ignored */
   ie.time.tv_sec = 0;
   ie.time.tv_usec = 0;

   write(fd, &ie, sizeof(ie));
}

/*
 * Display the application usage w/command line options and exit w/error
 */
local void displayUsage(int exitCode)
{
   printf("Usage: kaykey [OPTION]... [configuration profile]\n\n\r"
          "Kaykey is a user mode Kaypro keyboard driver for Linux. It utilizes the uinput\n\r"
          "kernel module and tio serial device I/O tool. Therefore, both must be installed\n\r"
          "and enabled. In addition, Kaykey must be run at a priviledge level capable of\n\r"
          "communicating with uinput. On most distributions, this is root level priviledges\n\r"
          "by default. If a configuration profile is provided, it will be used to select\n\r"
          "appropriate setup from the configuration file.\n\n\r"
          "OPTIONS:\n\r"
          "  -f,  Provide configuration file name. ie. -f kaykey.conf\n\r"
          "       If no configuration filename is provided, the application will first look\n\r"
          "       for .kaykey.conf file in the current directory. If that file is not found,\n\r"
          "       it will look for it in ~/.congfig/kaykey/kaykey.conf.\n\r"
          "  -h   Display this usage information\n\r");
   exit(exitCode);
}

/*
 * Check for the default configuration files in predetermined order and return
 * the name of the appropriate one
 */
local char* getConfigFile(void)
{
   char *ret = NULL;
   int  fd;
   persistent char *localConf = ".kaykey.conf";
   persistent char *userConf  = "~/.config/kaykey/kaykey.conf"; 

   // If the .kaykey file does not exist...
   if((fd=open(localConf,O_RDONLY))==-1)
   {
      // If the ~/.config/kaykey/kaykey.conf file does not exist...
      if((fd=open(userConf,O_RDONLY))==-1)
      {
         printf("Error: Unable to open the %s or the %s\n\r"
                "       configuration files. Please provide a configuration file.\n\r", localConf, userConf);
         exit(-1);
      }
      // Else close the ~/.config/kaykey/kaykey.conf file...
      else
      {
         close(fd);
         ret = userConf;
      }
   }
   // Else close the .kaykey file...
   else
   {
      close(fd);
      ret = localConf;
   }

   return(ret);
}
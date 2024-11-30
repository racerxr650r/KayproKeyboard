# SerKey
Linux serial keyboard driver that supports the Kaypro keyboard and other custom key mappings

Utilizes the [uinput kernel module](https://kernel.org/doc/html/v4.12/input/uinput.html)
to implement a user mode application or daemon for the Kaypro keyboard. This has only been
tested on Raspberry PI OS.

## Download this repository

```console
sudo apt update
sudo apt install git
git clone https://github.com/racerxr650r/SerKey.git
```

## Build the application

```console
cd SerKey
make
```

Change to the SerKey directory and build the application

## Setup permissions to run from your user account

```console
make permissions
```

This will create a uinput group, add your user to it, and setup a udev rule
to make /dev/uinput read/writeable by your user/group.

## Run the application

```console
make run OPTIONS="-f" DEVICE="/dev/ttyAMA4"
```

Or...

```console
./build/serkey -f /dev/ttyAMA4
```

This will launch the application with the fork/daemon option using the ttyAMA4
serial device. Substitute the options and device you require. See the usage
section for details about the options.

## Install serkey

```console
make install OPTIONS="-b 300 -p none -d 8 -s 1 -k kaypro" DEVICE="/dev/ttyAMA4"
```

Installs the serkey application and documentation.

> :memo: **Note:** BINDIR (directory to install the binary file) and MANDIR 
(directory to install the man page documentation) can be defined from the 
make command line. This will replace the default values. The defaults for 
BINDIR and MANDIR should work for most Linux distributions

## Get usage information

```console
serkey -h
```

or...

```console
man serkey
```

man only works if you have installed serkey.

## Command line usage
```
USAGE: serkey [OPTION]... serial_device

User mode serial keyboard connected to serial device "serial_device"

OPTIONS:
  -b   <bps>
       Set the baud rate in bits per second (bps) (default:300)
  -p   odd|even|none
       Set the parity  (default:none)
  -d   5|6|7|8
       Set the number of data bits (default:8)
  -s   1|2
       Set the number of stop bits (default:1)
  -k   kaypro|media_keys|ascii
       Select the key mapping (default:kaypro)
  -f   Fork the process to run as a background process
  -v   Verbose mode to display status information and keystroke codes
  -h   Display this usage information
```

## Uninstall serkey

```console
make uninstall
```

Uninstalls the serkey application and documentation.

## Daemonize serkey

```console
make daemon OPTIONS="-b 300 -p none -d 8 -s 1 -k kaypro" DEVICE="/dev/ttyAMA4"
```

Install the serkey application and documentation and create a .service file to
launch serkey as a daemon using systemd. This file will use the OPTIONS and
DEVICE defined	in the makefile or the make command line

> :memo: **Note:** OPTIONS, DEVICE, and SYSDDIR can be defined from the make
command line. This will replace the default values. The default for SYSDDIR
should work for Linux distributions that use the systemd init system. This
includes Raspberry Pi OS, Debian, Ubuntu, MX Linux, etc.

## Uninstall the serkey Daemon

```console
make undaemon
```

Uninstall the serkey application and documentation stop the serkey daemon and
remove the .service file from the systemd configuration directory

# Adding a custom key map to serkey

At the bottom of the serkey.c source file, find the keymap data structure. This
data structure defines the uinput key mappings for each character received from
the serial port.

```C
// Key Maps *******************************************************************
local keymap_t keymap[3][256] =
{
    { // Kaypro keymap ------------------------------------------------------------
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true },   // 0	NULL(Null character)
        ...
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = true }    // 255	nbsp	(non-breaking space or no-break space)
    },
    { // ASCII Keymap --------------------------------------------------------------
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false },   // 0	NULL(Null character)			
        ...
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false }    // 255	nbsp	(non-breaking space or no-break space)
    },
    { // Media Keymap --------------------------------------------------------------
        { .key = KEY_MUTE, .control = false, .shift = false, .makebreak = false },           // 0
        ...
        { .key = KEY_RESERVED, .control = false, .shift = false, .makebreak = false }        // 255
   }
};
```

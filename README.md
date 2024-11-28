# SerKey
Linux serial keyboard driver that supports the Kaypro keyboard and other custom key mappings

Utilizes the [uinput kernel module](https://kernel.org/doc/html/v4.12/input/uinput.html) and [tio serial I/O device tool application](https://github.com/tio/tio) to implement a user mode driver for the Kaypro keyboard. This has only been tested on Raspberry PI OS.

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

## Run the application

```console
make run
```

Or...

```console
./build/serkey /dev/ttyAMA4
```

## Command line usage

USAGE: serkey [OPTION]... serial_device

User mode serial keyboard connected to serial device "serial_device"

OPTIONS:
  -b   <bps>
       Set the baud rate in bits per second (bps) (default:300)
  -p   odd|even|none|mark|space
       Set the parity  (default:none)
  -d   5|6|7|8|9
       Set the number of data bits (default:8)
  -s   1|2
       Set the number of stop bits (default:1)
  -k   kaypro|media_keys|ascii
       Select the key mapping (default:kaypro)
  -h   Display this usage information

## Install the application

```console
make install OPTIONS = "-b 300 -p none -d 8 -s 1 -k kaypro" DEVICE = "/dev/ttyAMA4"
```

Installing serkey using the makefile will do the following:

1. Build the serkey application, if it hasn't been already
2. Copy the serkey application to $(BINDIR) directory. BINDIR defaults to "/usr/local/bin"
3. Copy the serkey man page to the $(MANDIR) directory. MANDIR defaults to "/usr/local/man/man1"
4. Modify the systemd service file serkey.service with the provided $(OPTIONS) and $(DEVICE)
5. Copy the service file serkey.service to the $(SYSDDIR) directory. SYSDDIR defaults to "/etc/systemd/system"

> :memo: **Note:** BINDIR, MANDIR, OPTIONS, DEVICE, and SYSDDIR can be defined from the make command line. This will
replace the default value. The defaults for BINDIR, MANDIR, and SYSDDIR should work for Linux distributions that use
the systemd init system. This includes Raspberry Pi OS, Debian, Ubuntu, MX Linux, etc.

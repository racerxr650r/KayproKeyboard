# serkey application Makefile

# Makefile Targets
#          all:	compiles the source code
#        clean: removes all .hex, .elf, and .o files in the source code and 
#              	library directories
#      install:	installs the serkey application and documentation
#    uninstall:	uninstalls the serkey application and documentation
#   permission:	create a uinput group, add your user to it, and setup a udev
#				rule to make /dev/uinput read/writeable by your user/group
# unpermission:	Remove the udev rule putting /dev/uinput in the uinput group
#       daemon:	Create a .service file to launch serkey as a daemon using
#				systemd. This file will use the OPTIONS and DEVICE defined
#				in the makefile or the make command line
#     undaemon:	Stop the serkey daemon and remove the .service file from the
#				systemd configuration directory

# Build Variables +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Project name
PRJ = serkey
# Linux usr binaries directory
BINDIR =	/usr/local/bin
# Linux manual directory for the man command
MANDIR =	/usr/local/man/man1
# Build directory
BUILD_DIR = ./build
# Systemd services directory
SYSDDIR = /etc/systemd/system
# C compiler command
CC =		cc
# Build flags for c files
CFLAGS =	-O -I/usr/local/include -pedantic -Wall -Wpointer-arith -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wno-long-long
#LDFLAGS =	-s -L/usr/local/lib
#LIBS =		-lxml2
# serkey command line options
OPTIONS =
# serkey command line device
DEVICE = /dev/ttyAMA4

# Build Targets +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Build all target files
all:		build $(PRJ)
# Create the build directory
build:
	mkdir -p $(BUILD_DIR)
# Build the app from the .c source
$(PRJ):		$(PRJ).c
	$(CC) $(CFLAGS) $(PRJ).c -o $(BUILD_DIR)/$(PRJ)
# Run serkey with the provided args
run:		all
	$(BUILD_DIR)/$(PRJ) $(OPTIONS) $(DEVICE)
# Install the application
install:	all
	sudo cp $(BUILD_DIR)/$(PRJ) $(BINDIR)
	sudo cp $(PRJ).1 $(MANDIR)
# Uninstall the application
uninstall:
	sudo rm -f $(BINDIR)/$(PRJ)
	sudo rm -f $(MANDIR)/$(PRJ).1
# Setup permissions
permission:
	-sudo groupadd uinput
	-sudo usermod -a -G uinput $$(whoami)
	-sudo cp ./uinput.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules
# Remove the rule that puts /dev/uinput in the uinput group
unpermission:
	sudo rm -f /etc/udev/rules.d/uinput.rules
	sudo udevadm control --reload-rules
# Setup daemon launched by systemd
daemon:
	cp serkey.service.src $(PRJ).service
	echo ExecStart=$(BINDIR)/serkey $(OPTIONS) $(DEVICE) | tee -a $(PRJ).service
	sudo mv $(PRJ).service $(SYSDDIR)
	systemctl start $(PRJ)
	systemctl enable $(PRJ)
	systemctl status serkey
# Remove the daemon from systemd
undaemon:
	systemctl stop $(PRJ)
	systemctl disable $(PRJ)
	sudo rm -f $(SYSDDIR)/$(PRJ).service
# Clean up all generated files
clean:
	rm -rf $(BUILD_DIR)

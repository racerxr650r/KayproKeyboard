# serkey application Makefile

# Makefile Targets
#     all:     compiles the source code
#   clean:     removes all .hex, .elf, and .o files in the source code and 
#              library directories
# install:     installs the serkey application/documentation and sets up the 
#              Raspberry PI configuration files to load it at boot. Before 
#              installing be sure to update the serkey.service file
# unsintall:   uninstalls the serkey application and documentation

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
	sudo cp ./uinput.rules /etc/udev/rules.d/
	sudo udevadm control --reload-rules
	cp serkey.service.src $(PRJ).service
	echo ExecStart=$(BINDIR)/serkey $(OPTIONS) $(DEVICE) | tee -a $(PRJ).service
	sudo mv $(PRJ).service $(SYSDDIR)
	systemctl start $(PRJ)
	systemctl enable $(PRJ)
	systemctl status serkey
# Uninstall the application
uninstall:
	systemctl stop $(PRJ)
	systemctl disable $(PRJ)
	sudo rm -f /etc/udev/rules.d/uinput.rules
	sudo rm -f $(BINDIR)/$(PRJ)
	sudo rm -f $(MANDIR)/$(PRJ).1
	sudo rm -f $(SYSDDIR)/$(PRJ).service

# Setup permissions
permissions:
	-sudo groupadd uinput
	-sudo usermod -a -G uinput $$(whoami)

# Clean up all generated files
clean:
	rm -rf $(BUILD_DIR)

# serkey application Makefile

# Makefile Targets
#     all:     compiles the source code
#   clean:     removes all .hex, .elf, and .o files in the source code and 
#              library directories
# install:     installs the serkey application/documentation and sets up the 
#              Raspberry PI configuration files to load it at boot

# Build Variables +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Project name
PRJ = serkey
# Linux usr binaries directory
BINDIR =	/usr/local/bin
# Linux manual directory for the man command
MANDIR =	/usr/local/man/man1
# Build directory
BUILD_DIR = ./build/
# C compiler command
CC =		cc
# Build flags for c files
CFLAGS =	-O -I/usr/local/include -pedantic -Wall -Wpointer-arith -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wno-long-long
#LDFLAGS =	-s -L/usr/local/lib
#LIBS =		-lxml2

# Build Targets +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
# Build all target files
all:		build $(PRJ)
# Create the build directory
build:
	mkdir -p $(BUILD_DIR)
# Build the app from the .c source
$(PRJ):		$(PRJ).c
	$(CC) $(CFLAGS) $(PRJ).c -o $(BUILD_DIR)$(PRJ)
# Install the application
install:	all
	rm -f $(BINDIR)/$(PRJ)
	cp $(PRJ) $(BINDIR)
	rm -f $(MANDIR)/$(PRJ).1
	cp $(PRJ).1 $(MANDIR)
# Clean up all generated files
clean:
	rm -rf $(BUILD_DIR)

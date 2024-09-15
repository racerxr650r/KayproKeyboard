#!/bin/bash
# This script installs the dependencies for the Kapro keyboard user mode driver.
# These dependencies are the uinput kernel module and the tio serial device I/O
# tool. 
#
# NOTE: This script assumes the distribution includes the uinput module

# Install dependancies
sudo apt update
sudo apt install -y wget unzip

# Download and extract source files
pushd .
cd ..
wget https://github.com/tio/tio/archive/refs/tags/v3.7.zip
unzip v3.7.zip
cd tio-3.7

# Build and install tio
meson setup build
meson compile -C build
sudo meson install -C build

# Clean up
cd ..
rm -r tio-3.7
rm v3.7.zip
popd

# Intall the uinput kernel module
grep -qxF 'uinput' /etc/modules || sudo modprobe uinput
grep -qxF 'uinput' /etc/modules || echo 'uinput' | sudo tee -a /etc/modules
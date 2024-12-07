#!/bin/bash
# This script installs the dependencies for the Kapro keyboard user mode driver.
# These dependencies are the uinput kernel module and the tio serial device I/O
# tool. 
#
# NOTE: This script assumes the distribution includes the uinput module

# Intall the uinput kernel module
grep -qxF 'uinput' /etc/modules || sudo modprobe uinput
grep -qxF 'uinput' /etc/modules || echo 'uinput' | sudo tee -a /etc/modules

# Battery driver for the Acer Switch 11 laptop.

A kernel module that reads the battery and AC plug status and reports them to
the Linux kernel as a power supply.

## Table of Contents
- [Reasons for this module](#reasons-for-this-module)
- [Usage](#usage)
    - [Prerequisites](#prerequisites)
    - [Compile](#compile)
    - [Loading the module](#loading-the-module)
    - [Unloading the module](#unloading-the-module)
- [Notes](#notes)

## Reasons for this module
Neither the battery nor the mains plug of that laptop were correctly detected by
Linux, since the BIOS provides a (very) broken DSDT. Since I was not able to fix
the table, I wrote this kernel module in order to provide the battery
information.

See [this topic](https://bbs.archlinux.org/viewtopic.php?id=232640) in the Arch
Linux forums.

## Usage
Since this is not an official kernel module you need to compile and load it
yourself. Don't worry, it is not complicated.

Note that you need to compile the module on every kernel update!

### Prerequisites
You need a compiler and the Linux headers for your Kernel version installed.

For _pacman_ based systems:
```
# pacman -S linux-headers
```
For _APT_ based systems:
```
# apt-get install linux-headers-$(uname -r)
```

### Compile
It is as simple as cloning the repository and typing "make":
```
$ git clone https://github.com/jfrimmel/Acer-Switch-Battery-Module.git
$ cd Acer-Switch-Battery-Module
$ make
```
There shouldn't be any errors and a file "battery-module.ko" (along some others)
is created. This is your kernel module.

### Loading the module
It can be loaded using the following command:
```
# insmod battery-module.ko
```
Note that _modprobe_ is generally a better choice than _insmod_ since _modprobe_
resolves dependencies, but this module has no dependencies and so _insmod_ can
be used as well.

### Unloading the module
If you wish to remove the module at some point you simple execute the following
command:
```
# rmmod battery-module.ko
```
You don't need to do this under normal circumstances.

## Notes
If there is a battery detected without this module you should unload the driver
for it before loading this kernel module.

Such a driver is most likely the module _battery_ (for ACPI batteries) and _ac_
(for ACPI AC adapters). They could either unloaded using _rmmod_ or blacklisted.

To blacklist those modules execute the following command and reboot:
```
# echo 'blacklist battery' >> /etc/modprobe.d/blacklist.conf
# echo 'blacklist ac' >> /etc/modprobe.d/blacklist.conf
```

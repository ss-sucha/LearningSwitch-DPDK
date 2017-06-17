# LearningSwitch-DPDK

## Introduction
LearningSwitch-DPDK, a simple layer-2 learning and forwarding switch with virtual host support, utilizes DPDK to interface with NICs and virtual hosts.

This simple version, as an introduction to DPDK, focuses on the following aspects.
* DPDK interface to physical NICs
* DPDK interface to dynamic virtual hosts
* Layer-2 Ethernet address learning
* Hash-based lookup table
* Simple forwarding mechanism

This simple switch requires 2 logical cores (threads), 1 for virtual host driver, 1 for forwarding. Additional optimization is not included here, since it obstruct code simplicity and the focuses of this version.

## Compilation and Preparation
System requirements for LearningSwitch-DPDK are similar to the basic [DPDK requirements](http://dpdk.org/doc/guides/linux_gsg/sys_reqs.html).

Compilation steps are similar to DPDK sample applications and they can be found on [Compiling and Running Sample Applications](http://dpdk.org/doc/guides/linux_gsg/build_sample_apps.html), e.g., set path to the DPDK SDK, set target architecture, and run make.
```
$ export RTE_SDK=/PATH/TO/DPDK_SDK
$ export RTE_TARGET=TARGET_ARCHITECTURE
$ make
```
The `TARGET_ARCHITECTURE` is `x86_64-native-linuxapp-gcc` when a system is a common x86 (CPU) machine with 64-bit (Linux) operating system, and a standard gcc compiler. Please consult [Compiling the DPDK Target from Source](http://dpdk.org/doc/guides/linux_gsg/build_dpdk.html) for additional information.

Basic required module for DPDK such as `igb_uio.ko` is needed for LearningSwitch-DPDK. It can be loaded as follows:
```
$ modprobe uio
$ insmod $RTE_SDK/$RTE_TARGET/kmod/igb_uio.ko
```
Additional information can be found at [Loading Modules to Enable Userspace IO for DPDK](http://dpdk.org/doc/guides/linux_gsg/build_dpdk.html#loading-modules-to-enable-userspace-io-for-dpdk).

Every physical NIC used by LearningSwitch-DPDP must be binded to the DPDK kernel module. Please follow the instruction in [Binding and Unbinding Network Ports to/from the Kernel Modules](http://dpdk.org/doc/guides/linux_gsg/build_dpdk.html#binding-and-unbinding-network-ports-to-from-the-kernel-modules).

Hugepages are required by DPDK runtime environment and can be configured as the instruction in [Use of Hugepages in the Linux Environment](http://dpdk.org/doc/guides/linux_gsg/sys_reqs.html#use-of-hugepages-in-the-linux-environment).

## Usage
LearningSwitch-DPDK can be executed by providing standard DPDK arguments and virtual socket paths as an example below:
```
$ build/learning-switch -c 3 -m 256 -- -s /PATH/TO/SOCKET/FILE
```
Note that `-c 3 -m 256` is the [standard DPDK arguments](http://dpdk.org/doc/guides/linux_gsg/build_sample_apps.html#running-a-sample-application). The `-s /PATH/TO/SOCKET/FILE` is a virtual socket path for a virtual host. This `-s` (or `--socket-file`) option can be added several times if they are needed, e.g.,
```
$ build/learning-switch -c 3 -m 256 -- -s /PATH/TO/SOCKET/FILE1 -s /PATH/TO/SOCKET/FILE2
```
Each socket file is an interface to a virtual host such as a KVM instance.

## Acknowledgement
This project is the first attempt to learn DPDK by myself. I found the following online materials very useful.
* DPDK documentation: [Getting Started Guide](http://dpdk.org/doc/guides/linux_gsg/index.html), [Programmer’s Guide](http://dpdk.org/doc/guides/prog_guide/index.html)
* DPDK sample applications: [Basic Forwarding](http://dpdk.org/doc/guides/sample_app_ug/skeleton.html), 
[L2 Forwarding](http://dpdk.org/doc/guides/sample_app_ug/l2_forward_real_virtual.html), 
[Vhost](http://dpdk.org/doc/guides/sample_app_ug/vhost.html)
* DPDK libraries: [Hash](http://dpdk.org/doc/guides/prog_guide/hash_lib.html), [Vhost](http://dpdk.org/doc/guides/prog_guide/vhost_lib.html)


<p align="center">
  <a href="http://www.openairinterface.org/">
    <img src="./episci_new_logo.png" alt="EpiSci Logo" height="90"/>
  </a>
</p>
<h1 align="center">
5G Sidelink (SL) Mode 1 and Mode 2 Implementation in OpenAirInterface (OAI): An Overview
</h>

## 1. Scenario
In this tutorial, we describe how to configure and run a **5G NR sidelink (SL)** end-to-end setup with OAI gNB, UE, and OAI CN5G for 5G NR SL mode 1 and mode 2, including SRAP-based sidelink relay.


- Minimum system requirements:

  - Desktop/Server for OAI gNB, UE, and OAI CN5G
  - Operating System: Ubuntu 24.04 LTS Desktop
  - CPU: 16 cores x86_64 @ 3.5 GHz
  - RAM: 64 GB
  - Kernel: lowlatency

## 2. Overview of 5G SL Features

This implementation extends the **OpenAirInterface (OAI)** codebase with support for **5G NR sidelink (SL)**, enabling both mode 1 and mode 2 operations. In SL mode 1, device-to-device (D2D) communication is scheduled and managed with assistance from the network infrastructure, while in SL mode 2, UEs communicate directly in a fully distributed manner without relying on network support. Currently, the following features are implemented:

#### &emsp; ✅ **Key Features**
&emsp;&emsp; ◉ **SL Synchronization:**<br>&emsp;&emsp;&emsp;&emsp; Devices can achieve synchronization over the PC5 interface (Sidelink) either by using predefined synchronization resources or by utilizing the resources provided through a Configured Grant Type 1.<br>
&emsp;&emsp; ◉ **SL Configuration:**<br>&emsp;&emsp;&emsp;&emsp; Pre-configured Sidelink resource configuration for SL Mode 2, as well as Configured Grant Type 1 configuration for SL Mode 1, are supported to provide greater flexibility in networking scenarios.<br>
&emsp;&emsp; ◉ **Data Transmission and Reception:**<br>&emsp;&emsp;&emsp;&emsp; End-to-end transmission and reception of SL data packets, including SL-SCH and SL-PSCCH channel handling.<br>
&emsp;&emsp; ◉ **CSI Reporting:**<br>&emsp;&emsp;&emsp;&emsp; Support for basic Channel State Information (CSI) reporting mechanisms for better link adaptation.<br>
&emsp;&emsp; ◉ **Basic Scheduling:**<br>&emsp;&emsp;&emsp;&emsp; A basic Sidelink MAC scheduler has been implemented to manage time resource allocations in Mode 2, and it has been enhanced to support Mode 1 transmissions using Configured Grant Type 1 configurations.<br>
&emsp;&emsp; ◉ **Resource Pool Scheme:**<br>&emsp;&emsp;&emsp;&emsp; Static and pre-configured resource pools are supported to facilitate Mode 2 communication, while Configured Grant Type 1 is supported for resource pool configurations in SL Mode 1.<br>
&emsp;&emsp; ◉ **Data Feedback:**<br>&emsp;&emsp;&emsp;&emsp; Provides data feedback for 5G SL communication to share the reception status with the transmitter in both 5G SL modes.<br>
&emsp;&emsp; ◉ **Hybrid Automatic Repeat reQuest:**<br>&emsp;&emsp;&emsp;&emsp; Enhances reliability and throughput by combining error detection with retransmission and error correction.<br>
&emsp;&emsp; ◉ **UE-to-Network (U2N) Relay**<br>&emsp;&emsp;&emsp;&emsp; U2N Relay capabilities are supported to facilitate the communication between Remote UE and gNB via Relay UE.<br>
&emsp;&emsp; ◉ **UE Radio Resource Allocation via Configured Grant (CG) type 1**<br>&emsp;&emsp;&emsp;&emsp; Relay UE and Remote UE radio resources are allocated via RRC message from gNB.<br>

## 3. Update of 5G SL Features

### 3.1 Added Features

&emsp; The following features have been implemented and integrated into the OAI codebase to support 5G NR SL:

&emsp;&emsp; ◉ Full PHY and MAC channel support <br>
&emsp;&emsp;&emsp;&emsp;🔹 **PHY:** PSBCH, PSSCH, PSCCH, PSFCH <br>
&emsp;&emsp;&emsp;&emsp;🔹 **PHY ⇄ MAC:** SL-SCH, SL-BCH<br>
&emsp;&emsp;&emsp;&emsp;🔹 **MAC ⇄ RLC:** SBCCH, SCCH, STCH<br>
&emsp;&emsp; ◉ TX/RX data path support for SL mode 2<br>
&emsp;&emsp;&emsp;&emsp;🔹 CSI Reporting (basic support)<br>
&emsp;&emsp;&emsp;&emsp;🔹 CSI Reference Signals (CSI-RS)<br>
&emsp;&emsp;&emsp;&emsp;🔹 SINR Estimation<br>
&emsp;&emsp; ◉ Basic MAC scheduling for mode 2 operation<br>
&emsp;&emsp; ◉ Basic Configured Grant Type 1 based MAC scheduling for mode 1 operation<br>
&emsp;&emsp; ◉ Resource pool configuration (pre-configured/static)<br>
&emsp;&emsp; ◉ Dedicated Sidelink Resource pool configuration<br>
&emsp;&emsp; ◉ Dynamic MCS support (currently up to MCS 9 in B210 USRP)<br>
&emsp;&emsp; ◉ HARQ retransmission handling (basic)<br>
&emsp;&emsp; ◉ SL pre-configuration support (static configuration via .conf files)<br>
&emsp;&emsp; ◉ SL IP Traffic support (updates to PDCP, RLC, and SDAP layers)<br>
&emsp;&emsp; ◉ 5G Sidelink Relay Adaptation Protocol (SRAP) based U2N relay support<br>
&emsp;&emsp; ◉ 5G Sidelink RLC layer AM mode setup for RRC Signaling<br>
&emsp;&emsp; ◉ 5G Sidelink SL-SRB1 setup<br>
&emsp;&emsp; ◉ Control plane RRC message update for Sidelink Radio Resource Allocation<br>
&emsp;&emsp; ◉ 5G Sidelink SLSS ID update for synchronization via SSSB<br>
&emsp;&emsp; ◉ Separate PC5 and Uu entities for RLC, SRAP, PDCP layers at Relay UE<br>
&emsp;&emsp; ◉ USRP Support: SL mode 1 and mode 2 tested successfully on B210 only<br>
&emsp;&emsp; ◉ Sidelink HARQ Feedback Report to gNB to monitor sidelink communication status<br>

### 3.2 Missing Features or Features Needing Updates
&emsp; The following features are either missing or require further updates and debugging:

&emsp;&emsp;❌ Multiple PDU Support:<br>
&emsp;&emsp;&emsp;&emsp;Not yet implemented; needed for higher MCS values and throughput.<br>
&emsp;&emsp;❌ Multiple Subchannel Support:<br>
&emsp;&emsp;&emsp;&emsp;Currently limited to single subchannel operation; lacks logical channel prioritization.<br>
&emsp;&emsp;❌ Sensing Algorithm:<br>
&emsp;&emsp;&emsp;&emsp;No support for channel sensing (needed for advanced mode 2 and resource allocation decisions).<br>
&emsp;&emsp;❌ Advanced Resource Allocation:<br>
&emsp;&emsp;&emsp;&emsp;No support for additional resource allocation algorithms (Dynamic Grant (DG) and CG type 2).<br>
&emsp;&emsp;❌ Logical Channel Prioritization:<br>
&emsp;&emsp;&emsp;&emsp;Not currently implemented; needed for multiple logical channel management.<br>
&emsp;&emsp;❌ UE-to-UE Relay in SRAP:<br>
&emsp;&emsp;&emsp;&emsp; Not currently supported; only UE-to-Network relay is implemented.<br>
&emsp;&emsp;❌ Control Plane for SRAP:<br>
&emsp;&emsp;&emsp;&emsp; Not currently supported; only user plane of UE-to-Network mode is developed and validated.<br>

## 4. Test Features
&emsp; The current implementation has been tested with the following configuration:

&emsp;&emsp;✅ Working Setup:<br>
&emsp;&emsp;&emsp;&emsp; ◉ Two UE devices communicating over SL mode 2 using Ettus B210 SDRs; basic SL transmission and reception are confirmed functional in this setup<br>
&emsp;&emsp;&emsp;&emsp; ◉ Three node Relay scenario (Remote UE, Relay UE and gNB) is working on RFSim and B210s setup; At present, the implementation supports MCS indices up to 9 (MCS 0–9) only.<br>
&emsp;&emsp;❌ Unsupported or Non-Functional Setup:<br>
&emsp;&emsp;&emsp;&emsp; ◉ Ettus N310 devices: Current implementation does not work.<br>

## 5. Build 5G NR Sidelink

### 5.1 **Ubuntu 24.04 Build Support:**

&emsp;This branch includes Ubuntu 24.04 support with necessary build system modifications. Simply follow the standard build instructions:<br>
&emsp;&emsp;**Note:** This branch includes a fix for ASN.1 compiler installation. The build system will automatically install **ASN.1 compiler v0.9.29** (commit 998e7ea2) instead of the newer vlm_master branch. This is required because the codebase is currently compatible with v0.9.29 (June 2024), while newer versions (v1.2+, v1.4+) generate incompatible pointer structures instead of structs, which would require approximately 1500 code changes throughout the codebase.

### 5.2 **Build OAI:**
&emsp;Follow these steps to build OAI with support for 5G Sidelink and related features. If you use the default UHD version, ignore the lines starting with `export` in the following script:
```
$ git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git
$ cd ~/openairinterface5g
$ git fetch --tags
$ git clean -fdX
$ git checkout sl-5g-nr
$ source oaienv
$ cd cmake_targets
$ export BUILD_UHD_FROM_SOURCE=True
$ export UHD_VERSION=4.8.0.0
$ ./build_oai -C -I -w USRP --install-optional-packages   # Only necessary on fresh installs
$ ./build_oai --nrUE --gNB -w USRP -w SIMU
```

**🔔Note:** Building UHD from source is required because the ettusresearch PPA installs multiple UHD versions (4.6.0, 4.8.0, 4.10.0), but the default symlink `/usr/lib/x86_64-linux-gnu/libuhd.so` points to UHD 4.10.0, which requires C++17 and is incompatible with OAI's older C++ standard. By setting `BUILD_UHD_FROM_SOURCE=True` and `UHD_VERSION=4.8.0.0`, the build script compiles UHD 4.8.0.0 from source, ensuring compatibility. This branch also includes a cleanup fix in `build_helper` that uses `$SUDO rm -rf /tmp/uhd` to properly remove temporary files created during the UHD build.

#### 5.2.1 **For Active Development and Faster Build Times:**

&emsp;If you are actively developing and want to speed up the build process, you can directly build only the executables:
```
$ cd ~/openairinterface5g/cmake_targets/ran_build/build
$ make nr-softmodem nr-uesoftmodem rfsimulator -j$(nproc)
```

#### 5.2.2 **Enabling Address Sanitizer in RFSim:**
&emsp;If you want to use AddressSanitizer (ASan) during softmodem execution in RFSim, add the following flag in the build_oai argument option:
```
 --sanitize-address
```
**🔔Note:** If you encounter a DEADLYSIGNAL error from AddressSanitizer (ASan) during OAI compilation, apply the following workaround:
```
$ sudo sysctl vm.mmap_rnd_bits=28
```
&emsp;&emsp;**Verification (after running `./build_oai -I`):**
```bash
$ /opt/asn1c/bin/asn1c -version
# Should output: ASN.1 Compiler, v0.9.29
```

### 5.3 **USRP hardware access control:**
**🔔Note:** After OAI compilation, apply the following for USRP hardware access:

1. Copy the USRP hardware access rules into your system configuration
```
sudo cp /usr/local/lib/uhd/utils/uhd-usrp.rules /etc/udev/rules.d/
```

2. Reload the device manager rules to apply the change instantly
```
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### 5.4 **Troubleshooting Build and Runtime Issues:**

#### 5.4.1 Missing libparams_libconfig.so

If you encounter the following error when launching the softmodem:
```
[CONFIG] Error calling dlopen(libparams_libconfig.so): libparams_libconfig.so: cannot open shared object file: No such file or directory
```

This error has two possible causes:

**Cause 1: Library not built**

Build the library manually:
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
make -j6 params_libconfig
```

Verify the library exists:
```
ls -la ~/openairinterface5g/cmake_targets/ran_build/build/libparams_libconfig.so
```

**Cause 2: LD_LIBRARY_PATH not set correctly (most common in remote SSH execution)**

Even if the library exists, the softmodem may not find it. Use:

```bash
LD_LIBRARY_PATH=/path/to/build sudo -E /path/to/nr-uesoftmodem [options]
```

**🔔Note:** This library is required for configuration file parsing. Without proper LD_LIBRARY_PATH, the softmodem will crash on startup with a segmentation fault in `initNamedTpool`.

## 6. EpiSci's 5G Sidelink Mode 1

### 6.1 **5G SL Relay**
&emsp; To enable relay scenario support in our system, we have implemented the Sidelink Relay Adaptation Protocol (SRAP). The SRAP supports two types of relaying modes:<br>
&emsp;&emsp; ◉ UE-to-Network (U2N)<br>
&emsp;&emsp; ◉ UE-to-UE (U2U)<br>
Currently, only the U2N mode is implemented, which enables a Relay UE to forward traffic from a Remote UE to the gNB. The SRAP implementation code is available under `openair2/LAYER2/nr_srap`, which provides the following support:<br>
&emsp;&emsp; ◉ structures and functions to define the SRAP entity.<br>
&emsp;&emsp; ◉ addition and removal of SRAP headers.<br>
&emsp;&emsp; ◉ processing of the received pdu.<br>
&emsp;&emsp; ◉ forwarding of the received messages.<br>
&emsp;&emsp; ◉ passing of the PDU to lower layers.<br>
&emsp;&emsp; ◉ passing of the SDU to upper layers.<br>

### 6.2 **5G SL Radio Resource Allocation**
&emsp; To allocate Relay UE Radio Resources and Remote UE Radio Resources from the gNB, we implemented SL-SRB1 control message handling along with CG type 1 resource allocation. The gNB delivers the allocated resource configuration to the UE through an RRC Reconfiguration control message, enabling the UE to apply the configuration for sidelink communication.

For the implementation of CG type 1 resource allocation, we applied the following update:<br>

&emsp;&emsp; ◉ creation of RLC AM entity for SL-SRB1.<br>
&emsp;&emsp; ◉ RRCReconfiguration message from gNB to UE.<br>
&emsp;&emsp; ◉ UEAssistanceInformation message from UE to gNB.<br>
&emsp;&emsp; ◉ parsing of configuration message and adaptation.<br>
&emsp;&emsp; ◉ RRC message exchange through SL-SRB1 entity over PC5 interface.<br>

Based on the received Configured Grant Type 1 configurations, the SL MAC scheduler determines the transmission opportunities and allocates the corresponding sidelink resources for UE data transmission.

### 6.3 **Pre-requisite: Core Network**

&emsp; To test IP traffic using `ping` in 5G SL mode 1, the OAI Core Network must first be launched as a prerequisite.<br>

&emsp; Once the Core Network is running, the gNB, Relay UE (SyncRef UE), and Remote UE (Nearby UE) can be started either on the same machine or on separate machines. The following sections demonstrate how to test 5G SL mode 1 using RF simulator and USRP hardware.

### 6.4 **RFSim Test:**

&emsp; RFSim in the OAI codebase is a radio frequency (RF) simulation module that enables end-to-end testing without requiring physical RF hardware. It simulates the wireless channel and signal propagation, allowing complete testing of 4G/5G network components entirely in software. This makes RFSim particularly valuable for CI/CD pipelines, development, and validation environments, where rapid and repeatable testing is essential.

#### 6.4.1 **Launching Commands:**

The following shows the command line to launch soft modems (gNB, Relay UE, and Remote UE).
In case of local test (all machines 1, 2, 3 are identically the same host), replace both `<MACHINE 1 IP Address>` and `<MACHINE 3 IP Address>` to `127.0.0.1` in the following.
In the same way, if all machines 1, 2, 3 are different hosts, replace `<MACHINE 1 IP Address>` and `<MACHINE 3 IP Address>` to gNB host IP address and Remote UE host IP address respectively.

&emsp; ***gNB in Terminal 1 of Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf \
--gNBs.[0].min_rxtxtime 6 --sa --sl-mode 1 --rfsim \
--rfsimulator.serveraddr server --rfsimulator.serverport 4048 \
 --relay-type 1 --remote-ue-id 1 2>&1 | tee ~/result_gNB.log
```
&emsp; ***Relay UE in Terminal 1 of Machine 2:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
 -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
--sa --sl-mode 1 --sync-ref --rfsim \
--rfsimulator.serveraddr <MACHINE 1 IP Address> --rfsimulator.serverport 4048 \
--rfsimulator.serveraddrsl <MACHINE 3 IP Address> --rfsimulator.serverportsl 4148 \
--relay-type 1 --is-relay-ue 1 2>&1 | tee ~/result_nrUE_syncref.log
```
&emsp; ***Remote UE in Terminal 1 of Machine 3:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa --sl-mode 2 --rfsim \
--rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
--relay-type 1 2>&1 | tee ~/result_nearby.log
```

Run `ping` command on the Remote UE terminal of Machine 3.
```
ping -I oaitun_ue2 8.8.8.8
```

To perform full system testing - including CSI reporting and PSFCH feedback - the commands remain the same. The only required step is to update the UE configuration files, as outlined below.

### 6.5 **USRP Test:**

The OTA USRP testing was conducted using B210 devices. This section demonstrates how to test 5G SL mode 1 on USRP hardware.

#### 6.5.1 **USRP Setup:**

The following UHD commands can be used to verify that the USRP devices are ready for deployment and to retrieve essential information such as their serial numbers and addresses.

```
uhd_find_devices # This will find all USRPs

uhd_usrp_probe # This will probe the USRP and will ensure the status is ready
```

The USRPs can be connected through either cable or over-the-air medium. In the case of cable connectivity, an attenuator can be used in a lab environment to simulate real-world signal loss conditions.

#### 6.5.2 **Setting of attenuation:**

In order to set the attenuation for each channel you have to run this command on the host where the attenuator is connected. In this test mode, we assume the attenuator is connected to the gNB.
```
curl http://169.254.10.10/:CHAN:<channel number>:SETATT:<attenuation in dB>
```
for example: to set attenuation of channels 1 to 4 to 30 dB:
```
curl http://169.254.10.10/:CHAN:1:2:3:4:SETATT:30
```
In order to read current attenuation of each channel,
```
curl http://169.254.10.10/:ATT?
```

#### 6.5.3 **Running of SL Mode 1 on B210s:**

The following shows the command lines to launch soft modems (gNB, Relay UE, and Remote UE) on B210s.
Note, the serial field may need to be changed to match the USRPs:

&emsp; ***gNB in Terminal 1 of Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf \
--gNBs.[0].min_rxtxtime 6 --sa  --sl-mode 1 -E \
--ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif \
--relay-type 1 --remote-ue-id 1 2>&1 | tee ~/result_gNB.log
```
&emsp; ***Relay UE in Terminal 1 of Machine 2:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
 -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
--sa -E --sl-mode 1 --sync-ref \
--ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif \
--usrp-args 'serial=<Relay UE B210_Serial_Number in Uu interface>,type=b200' \
--usrp-args-sl 'serial=<Relay UE B210_Serial_Number in PC5 interface>,type=b200' \
--relay-type 1 --is-relay-ue 1 2>&1 | tee ~/result_nrUE_syncref.log
```
As an example, we can set it as following with updated serial fields:

```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
 -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
--sa -E --sl-mode 1 --sync-ref \
--ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif \
--usrp-args 'serial=340EA03,type=b200' \
--usrp-args-sl 'serial=340EA3B,type=b200' \
--relay-type 1 --is-relay-ue 1 2>&1 | tee ~/result_nrUE_syncref.log
```

&emsp; ***Remote UE in Terminal 1 of Machine 3:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa -E --sl-mode 2 \
--ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif \
--relay-type 1 2>&1 | tee ~/result_nearby.log
```

Run `ping` command on the Remote UE terminal of Machine 3.
```
ping -I oaitun_ue2 8.8.8.8
```

### 6.6 **Launching Sidelink Mode 1 Test via Script:**

&emsp; To launch the test via script, navigate to the test script directory as follows:
```
cd ~/openairinterface5g/doc/episys/test_script
```

&emsp; To launch the 5G SL mode 1 test via test script, select test profile and then select tests among the `slmode1_basic_tests` in [run_sl_test_config.sh](./test_script/run_sl_test_config.sh) file. For the details of setting, refer to the [README_sl_test.md](./test_script/README_sl_test.md) file. After setting was done, apply the following:
```
./run_sl_test.sh
```
After test was done, the summary will be displayed. For the detail, navigate to the `latest` folder and check the files created in the `latest` folder.

### 6.7 **Performance Test using Iperf3:**

iperf3 is a command-line tool used to measure the maximum achievable bandwidth on IP networks.
If iperf3 is not available in your system, install it via `sudo apt update && sudo apt install iperf3`.
If `iperf3 --version` shows an older version than 3.16, you may install iperf 3.16 or newer version using the following.
```
git clone https://github.com/esnet/iperf.git && \
cd iperf && git checkout 3.16 && ./bootstrap.sh && ./configure && make && make install
```

#### 6.7.1  Remote UE to UPF

##### 6.7.1.1  Launching iperf3 server

Select one between two options.

Option 1: UPF (Inside case of UPF docker)
```
docker exec -it oai-upf bash
```

Use the following command to run iperf3 server:
```
iperf3 -s -B <UPF IP address> -p 5001 -i 1
```

For example,
```
iperf3 -s -B 192.168.70.134 -p 5001 -i 1
```

Option 2: UPF (Outside case of UPF docker; in case of iperf3 installed in docker)
```
docker exec -it oai-upf bash -c 'iperf3 -s -B <UPF IP address> -p 5001 -i 1' | tee iperf_output.log
```

For example,
```
docker exec -it oai-upf bash -c 'iperf3 -s -B 192.168.70.134 -p 5001 -i 1' | tee iperf_output.log
```

##### 6.7.1.2  Launching iperf3 client

On the Remote UE, use the following command to run iperf3 client:
```
iperf3 -u -c <UPF IP address> --bind-dev <TUN interface name> -p 5001 -i 1 -b 1M
```

For example,
```
iperf3 -u -c 192.168.70.134 --bind-dev oaitun_ue2 -p 5001 -i 1 -b 1M
```

#### 6.7.2  UPF to Remote UE

##### 6.7.2.1  Launching iperf3 server

On the Remote UE, use the following command to run iperf3 server:
```
iperf3 -s --bind-dev <TUN interface name> -p 5001 -i 1
```

For example,
```
iperf3 -s --bind-dev oaitun_ue2 -p 5001 -i 1
```

##### 6.7.2.2  Launching iperf3 client

Select one between the following two options.

Option 1: UPF (Inside case of UPF docker)
```
docker exec -it oai-upf bash
```

Use the following command to run iperf3 client:
```
iperf3 -u -c <Remote UE IP address> -B  <UPF IP address> -p 5001 -i 1 -b 1M
```

For example,
```
iperf3 -u -c 10.0.0.100 -B 192.168.70.134 -p 5001 -i 1 -b 1M
```

Option 2: UPF (Outside case of UPF docker; in case of iperf3 installed in docker)
```
docker exec -it oai-upf bash -c 'iperf3 -u -c <Remote UE IP address> -B <UPF IP address>  -p 5001 -i 1 -b 1M' | tee iperf_output.log
```

For example,
```
docker exec -it oai-upf bash -c 'iperf3 -u -c 10.0.0.100 -B 192.168.70.134 -p 5001 -i 1 -b 1M' | tee iperf_output.log
```

### 6.8 **Running Video Stream:**

In the following, we assume that UPF IP address = 192.168.70.134 and Remote UE IP address (`oaitun_ue2`) = 10.0.0.100.

#### 6.8.1 Receiver (UPF in Core Network)

Apply the following environment setting for video.
```
xhost +local:docker
```

Add a port to expose of oai-upf service in docker-compose.yaml file:
```
 - 8090/udp
```

Launch docker compose as following:
```
docker compose up -d
```

After the docker process is activated, launch ffplay using one of the two options.

Option 1 (In the host shell)
```
docker exec -it oai-upf bash -c 'ffplay -fflags nobuffer -flags low_delay udp://0.0.0.0:8090'
```

Option 2 (Inside docker)
```
ffplay -flags low_delay -i udp://192.168.70.134:8090
```

After video streaming is done, apply the following.
```
xhost -local:docker
```

#### 6.8.2 Transmitter (Remote UE)

In the following, we assume that the file to transmit is located at ~/Videos/file_name.mp4.
There are two options for streaming on the transmitter side. One is video streaming using video file and the other is camera streaming. Select one between two options. In this section, we assume that the file to transmit is located at ~/Videos/file_name.mp4.

Option 1. Video file streaming in default setting
```
ffmpeg -re -stream_loop -1 -i ~/Videos/file_name.mp4 -f mpegts \
       "udp://192.168.70.134:8090?localaddr=$(ip -4 addr show oaitun_ue2 | grep -oP '(?<=inet\s)\d+(\.\d+){3}')&pkt_size=1316"
```

Option 2. USB camera streaming in 500 Kbps bandwidth
```
ffmpeg -f v4l2 -i /dev/video2 -b:v 500k -vcodec libx264 -preset ultrafast -tune zerolatency -f mpegts \
       "udp://192.168.70.134:8090?localaddr=$(ip -4 addr show oaitun_ue2 | grep -oP '(?<=inet\s)\d+(\.\d+){3}')&pkt_size=1316"
```

## 7. EpiSci's 5G Sidelink Mode 2

### 7.1 **RFSim Test:**

&emsp; RFSim in the OAI codebase is a radio frequency simulation module that enables end-to-end testing without physical RF hardware. By simulating the wireless channel and signal propagation, it allows complete testing of 4G/5G network components entirely in software. This makes RFSim ideal for CI/CD pipelines, development, and validation environments.

#### 7.1.1 **Test Environment: (SL Mode 2)**

&emsp; To test IP traffic using `ping` in 5G SL mode 2, the SyncRef UE and the Nearby UE are recommended to run on separate machines. Currently, IP traffic is also supported for both processes to run on the same machine in RFSim.

The following commands demonstrate how to test 5G SL mode 2 with two UEs using the RF simulator.

#### 7.1.2 **Commands:**

&emsp; ***SyncRef UE on Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
--sa --sl-mode 2 --sync-ref --rfsim --thread-pool -1,-1 \
--rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4048
```
&emsp; ***Nearby UE on Machine 2:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa --sl-mode 2 --rfsim --thread-pool -1,-1 \
--rfsimulator.serveraddrsl <MACHINE 1 IP Address> --rfsimulator.serverportsl 4048
```

Find the destination IP address for `ping` on the tun interface `oaitun_ue1` on the SyncRef UE terminal of Machine 1 as follows.
```
ifconfig oaitun_ue1 | awk '/inet / {print $2}' | sed 's/addr://'
```

Let's assume that the destination IP address is 10.0.0.1.
Run `ping` command on the Nearby UE terminal of Machine 2.
```
ping -I oaitun_ue2 10.0.0.1
```

**🔔Note:** Following errors can be seen when PSFCH is enabled (sl_PSFCH_period = 1, 2, 3) in the configurations files; we are working to fix this issue.
```
[NR_PHY]   [UE] SLSCH 0 in error: Setting NAK for SFN/SF 254/19 (pid 5, ndi 0, status 0, round 0, RV 0, prb_start 0, subchannel_size 50, TBS 656) r 0
[PDCP]   discard NR PDU rcvd_count=9, entity->rx_deliv 10,sdu_in_list 0
```

To perform full system testing (including CSI Reporting and PSFCH feedback), the commands remain unchanged - you only need to update the UE configuration files as outlined below.

#### 7.1.3 **Changing Configurations (CSI Reporting and PSFCH Period):**
&emsp; To change CSI Reporting and PSFCH configurations for sidelink testing, modify the following configuration files:

SyncRef UE Configuration File:
```
$HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf
```
Nearby UE Configuration File:
```
$HOME/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf
```
In each file, update the following variables values provided in given Table 1:

&emsp; ◉ sl_CSI_Acquisition<br>
&emsp; ◉ sl_TxResPools → sl_PSFCH_period<br>
&emsp; ◉ sl_RxResPools → sl_PSFCH_period<br>

<h3>Table 1: Configuration Table</h3>
<table>
  <thead>
    <tr>
      <th>Configuration</th>
      <th>sl_CSI_Acquisition</th>
      <th>sl_PSFCH_period (Tx/Rx Pools)</th>
    </tr>
  </thead>
  <tbody>
  <tr>
    <td>CSI Disabled 0</td>
    <td style="text-align: center;">1</td>
    <td style="text-align: center;">0/0</td>
  </tr>
  <tr>
    <td>CSI Disabled 1</td>
    <td style="text-align: center;">1</td>
    <td style="text-align: center;">1/1</td>
  </tr>
  <tr>
    <td>CSI Disabled 2</td>
    <td style="text-align: center;">1</td>
    <td style="text-align: center;">2/2</td>
  </tr>
    <tr>
    <td>CSI Disabled 4</td>
    <td style="text-align: center;">1</td>
    <td style="text-align: center;">3/3</td>
  </tr>
    <tr>
    <td>CSI Enabled 0</td>
    <td style="text-align: center;">0</td>
    <td style="text-align: center;">0/0</td>
  </tr>
  <tr>
    <td>CSI Enabled 1</td>
    <td style="text-align: center;">0</td>
    <td style="text-align: center;">1/1</td>
  </tr>
  <tr>
    <td>CSI Enabled 2</td>
    <td style="text-align: center;">0</td>
    <td style="text-align: center;">2/2</td>
  </tr>
    <tr>
    <td>CSI Enabled 4</td>
    <td style="text-align: center;">0</td>
    <td style="text-align: center;">3/3</td>
  </tr>
  <tbody>
</table>

**🔔Note:** Ensure the **sl_CSI_Acquisition** and **sl_PSFCH_period** values are set consistently across both UEs for a valid test.

**🔔Note:** Changing configurations (CSI Reporting and PSFCH Period) applies to USRP test in the same way as RFSim test.

### 7.2 **USRP Test:**

The OTA USRP testing was conducted using two B210s. This section demonstrates how to test 5G SL mode 2 on USRP hardware.

#### 7.2.1 **USRP Setup:**

The following UHD commands can be used to verify that the USRP devices are ready for deployment and to retrieve essential information such as their serial numbers and addresses.

```
uhd_find_devices # This will find all USRPs
uhd_usrp_probe # This will probe the USRP and will ensure the status is ready
```

The USRPs can be connected through either cable or over-the-air medium. In the case of cable connectivity, an attenuator can be used in a lab environment to simulate real-world signal loss conditions.

#### 7.2.2 **Setting of attenuation:**

In order to set the attenuation for each channel you have to run this command on the host where the attenuator is connected. In this test mode, we assume the attenuator is connected to the SyncRef UE.
```
curl http://169.254.10.10/:CHAN:<channel number>:SETATT:<attenuation in dB>
```
for example: to set attenuation of channels 1 to 4 to 30 dB:
```
curl http://169.254.10.10/:CHAN:1:2:3:4:SETATT:30
```
In order to read current attenuation of each channel,
```
curl http://169.254.10.10/:ATT?
```

#### 7.2.3 **Running of SL Mode 2 on B210s:**

SSH to Machine 1. Note, the serial field may need to be changed to match the USRPs:
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
--sa -E --sl-mode 2 --sync-ref --ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif \
--thread-pool -1,-1
```

SSH to Machine 2. Note, the serial field may need to be changed to match the USRPs:

```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa -E --sl-mode 2 --ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif \
--thread-pool -1,-1
```

Find the destination IP address for `ping` on the tun interface `oaitun_ue2` on the terminal of Machine 2 as follows.
```
ifconfig oaitun_ue2 | awk '/inet / {print $2}' | sed 's/addr://'
```

Let's assume that the destination IP address is 10.0.0.100.
Run `ping` command on the terminal of Machine 1.
```
ping -I oaitun_ue1 10.0.0.100
```

### 7.3 **Launching Sidelink Mode 2 Test via Script:**

&emsp; To launch the test via script, navigate to the test script directory as follows:
```
cd ~/openairinterface5g/doc/episys/test_script
```

&emsp; To launch the 5G SL mode 2 test via test script, select test profile and then select tests among the `slmode2_basic_tests` and `slmode2_csi_psfch_tests` in [run_sl_test_config.sh](./test_script/run_sl_test_config.sh) file. For the details of setting, refer to the [README_sl_test.md](./test_script/README_sl_test.md) file. After setting was done, apply the following:
```
./run_sl_test.sh
```
After test was done, the summary will be displayed. For the detail, navigate to the `latest` folder and check the files created in the `latest` folder.

### 7.4 **Performance Test using Iperf3:**

iperf3 is a command-line tool used to measure the maximum achievable bandwidth on IP networks.
If iperf3 is not available in your system, install it via `sudo apt update && sudo apt install iperf3`.
If `iperf3 --version` shows an older version than 3.16, you may install iperf 3.16 or newer version using the following.
```
git clone https://github.com/esnet/iperf.git && \
cd iperf && git checkout 3.16 && ./bootstrap.sh && ./configure && make && make install
```

Find the UE IP addresses as follows:

SyncRef UE IP address:
```
ifconfig oaitun_ue1 | awk '/inet / {print $2}' | sed 's/addr://'
```

Nearby UE IP address:
```
ifconfig oaitun_ue2 | awk '/inet / {print $2}' | sed 's/addr://'
```

In the following, we assume that SyncRef UE IP address = 10.0.0.1 and Nearby UE IP address = 10.0.0.100.

#### 7.4.1  Nearby UE to SyncRef UE

##### 7.4.1.1  Launching iperf3 server

On the SyncRef UE, use the following command to run iperf3 server:
```
iperf3 -s -B <SyncRef UE IP address> -p 5001 -i 1
```

For example,
```
iperf3 -s -B 10.0.0.1 -p 5001 -i 1
```

##### 7.4.1.2  Launching iperf3 client

On the Nearby UE, use the following command to run iperf3 client:
```
iperf3 -u -c <SyncRef UE IP address> -B <Nearby UE IP address> -p 5001 -i 1 -b 1M
```

For example,
```
iperf3 -u -c 10.0.0.1 -B 10.0.0.100 -p 5001 -i 1 -b 1M
```

#### 7.4.2  SyncRef UE to Nearby UE

##### 7.4.2.1  Launching iperf3 server

On the Nearby UE, use the following command to run iperf3 server:
```
iperf3 -s -B <Nearby UE IP address> -p 5001 -i 1
```

For example,
```
iperf3 -s -B 10.0.0.100 -p 5001 -i 1
```

##### 7.4.2.2  Launching iperf3 client

On the SyncRef UE, use the following command to run iperf3 client:
```
iperf3 -u -c <Nearby UE IP address> -B  <SyncRef UE IP address> -p 5001 -i 1 -b 1M
```

For example,
```
iperf3 -u -c 10.0.0.100 -B 10.0.0.1 -p 5001 -i 1 -b 1M
```

### 7.5 **Running Video Stream:**

We assume that SyncRef UE IP address (`oaitun_ue1`) = 10.0.0.1 and Nearby UE IP address (`oaitun_ue2`) = 10.0.0.100.

#### 7.5.1 Receiver (SyncRef UE)

```
ffplay -flags low_delay -i udp://10.0.0.1:1234
```

#### 7.5.2 Transmitter (Nearby UE)

In the following, we assume that the file to transmit is located at ~/Videos/file_name.mp4.
There are two options for streaming on the transmitter side. One is video streaming using video file and the other is camera streaming. Select one between two options. In this section, we assume that the file to transmit is located at ~/Videos/file_name.mp4.

Option 1. Video file streaming in default setting
```
ffmpeg -re -stream_loop -1 -i ~/Videos/file_name.mp4 -f mpegts \
       "udp://10.0.0.1:1234?localaddr=$(ip -4 addr show oaitun_ue2 | grep -oP '(?<=inet\s)\d+(\.\d+){3}')&pkt_size=1316"
```

Option 2. USB camera streaming in 500 Kbps bandwidth
```
ffmpeg -f v4l2 -i /dev/video2 -b:v 500k -vcodec libx264 -preset ultrafast -tune zerolatency -f mpegts \
       "udp://10.0.0.1:1234?localaddr=$(ip -4 addr show oaitun_ue2 | grep -oP '(?<=inet\s)\d+(\.\d+){3}')&pkt_size=1316"
```

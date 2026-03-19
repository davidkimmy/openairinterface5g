
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
&emsp;&emsp; ◉ **Data Feedback:**<br>&emsp;&emsp;&emsp;&emsp; Provided a data feedback for 5G SL communication to share the reception status with the transmitter in 5G SL both modes.<br>
&emsp;&emsp; ◉ **Hybrid Automatic Repeat reQuest:**<br>&emsp;&emsp;&emsp;&emsp; Enhances reliability and throughput by combining error detection with retransmission and error correction.<br>
&emsp;&emsp; ◉ **UE-to-Network (U2N) Relay**<br>&emsp;&emsp;&emsp;&emsp; U2N Relay capabilities are supported to facilitate the communication between Remote UE and gNB via Relay UE.<br>
&emsp;&emsp; ◉ **UE Radio Resource Allocation via Configured Grant (CG) type 1**<br>&emsp;&emsp;&emsp;&emsp; Relay UE and Remote UE radio resouces are allocated via RRC message from gNB.<br>


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
&emsp;&emsp; ◉ Dynamic MCS support (currently up to MCS 9)<br>
&emsp;&emsp; ◉ HARQ retransmission handling (basic)<br>
&emsp;&emsp; ◉ SL pre-configuration support (static configuration via .conf files)<br>
&emsp;&emsp; ◉ SL IP Traffic support (updates to PDCP, RLC, and SDAP layers)<br>
&emsp;&emsp; ◉ 5G Sidelink Relay Adaptation Protocol (SRAP) based U2N relay support<br>
&emsp;&emsp; ◉ 5G Sidelink RLC layer AM mode setup for RRC Signaling<br>
&emsp;&emsp; ◉ 5G Sidelink SL-SRB1 setup<br>
&emsp;&emsp; ◉ Control plane RRC message update for Sidelink Radio Resource Allocation<br>
&emsp;&emsp; ◉ 5G Sidelink SLSS ID update for synchronization via SSSB<br>
&emsp;&emsp; ◉ Separate PC5 and Uu entities for RLC, SRAP, PDCP layers at Relay UE<br>


### 3.2 Missing Features or Features Needing Updates
&emsp; The following features are either missing or require further updates and debugging:

&emsp;&emsp;❌ Multiple PDU Support:<br>
&emsp;&emsp;&emsp;&emsp;Not yet implemented; needed for higher MCS values and throughput.<br>
&emsp;&emsp;❌ Multiple Subchannel Support:<br>
&emsp;&emsp;&emsp;&emsp;Currently limited to single subchannel operation; lacks logical channel prioritization.<br>
&emsp;&emsp;❌ Sensing Algorithm:<br>
&emsp;&emsp;&emsp;&emsp;No support for channel sensing (needed for advanced mode 2 and resource allocation decisions).<br>
&emsp;&emsp;❌ USRP Support:<br>
&emsp;&emsp;&emsp;&emsp;SL mode 2 tested successfully on B210 only.<br>
&emsp;&emsp;❌ Sidelink HARQ Feedback Report to gNB:<br>
&emsp;&emsp;&emsp;&emsp;gNB expects HARQ Feedback Report to monitor sidelink communication status.<br>
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
&emsp;&emsp;&emsp;&emsp; ◉ Two UE devices communicating over SL mode 2 using Ettus B210 SDRs basic SL transmission and reception are confirmed functional in this setup<br>
&emsp;&emsp;&emsp;&emsp; ◉ Three node Relay scenario (Remote UE, Relay UE and gNB) is working on RFSIM and B210s setup; At present, the implementation supports MCS indices up to 9 (MCS 0–9) only.<br>
&emsp;&emsp;❌ Unsupported or Non-Functional Setup:<br>
&emsp;&emsp;&emsp;&emsp; ◉ Ettus N310 devices: Current implementation does not work. Debugging in work.<br>


## 5. Build 5G NR Sidelink
### 5.1 **Build OAI:**
&emsp;Follow these steps to build OAI with support for 5G Sidelink and related features:
```
$ git clone https://gitlab.eurecom.fr/oai/openairinterface5g.git
$ cd ~/openairinterface5g
$ git fetch --tags
$ git clean -fdX
$ git checkout nasa-sl-release-1.0
$ source oaienv
$ cd cmake_targets
$ ./build_oai -C -I --install-optional-packages   # Only necessary on fresh installs
$ ./build_oai --nrUE --gNB -w USRP -w SIMU
```

#### 5.1.1 **For Active Development and Faster Build Times:**

&emsp;If you are actively developing and want to speed up the build process, you can directly build only the executables:
```
$ cd ~/openairinterface5g/cmake_targets/ran_build/build
$ make nr-softmodem nr-uesoftmodem rfsimulator -j$(nproc)
```

#### 5.1.2 **Enabling Address Sanitizer in RFSIM:**
&emsp;If you want to use AddressSanitizer (ASan) during softmodem execution in RFSIM, add the following flag in the build_oai argument option:
```
 --sanitize-address
```
**🔔Note:** If you encounter a DEADLYSIGNAL error from AddressSanitizer (ASan) during OAI compilation, apply the following workaround:
```
$ sudo sysctl vm.mmap_rnd_bits=28
```

## 6. EpiSci's 5G Sidelink Mode 1
### 6.1 **5G SL Relay**

&emsp; To enable relay scenario support in our system, we have implemented the Sidelink Relay Adaptation Protocol (SRAP). The SRAP supports two types of relaying modes:<br>
&emsp;&emsp; ◉ UE-to-Network (U2N)<br>
&emsp;&emsp; ◉ UE-to-UE (U2U)<br>
Currently, only the U2N mode is implemented, which enables a Relay UE to forward traffic from a Remote UE to the gNB. The code of SRAP implementation is available under `openair2/LAYER2/nr_srap`, which provides following support:<br>
&emsp;&emsp; ◉ structures and functions to define the SRAP entity.<br>
&emsp;&emsp; ◉ addition and removal of SRAP headers.<br>
&emsp;&emsp; ◉ processing of the received pdu.<br>
&emsp;&emsp; ◉ forwarding of the received messages.<br>
&emsp;&emsp; ◉ passing of the PDU to lower layers.<br>
&emsp;&emsp; ◉ passing of the SDU to upper layers.<br>

### 6.2 **5G SL Radio Resource Allocation**
&emsp; To allocate Relay UE Radio Resources and Remote UE Radio Resources from the gNB, we implemented SL-SRB1 control message handling along with CG type 1 resource allocation. The gNB delivers the allocated resource configuration to the UE through an RRC Reconfiguration control message, enabling the UE to apply the configuration for sidelink communication.

For the implementation of CG type 1 resource allocation, we applied the following update::<br>

&emsp;&emsp; ◉ creation of RLC AM entity for SL-SRB1.<br>
&emsp;&emsp; ◉ RRCReconfiguration message from gNB to UE.<br>
&emsp;&emsp; ◉ UEAssistanceInformation message from UE to gNB.<br>
&emsp;&emsp; ◉ parsing of configuration message and adoptation.<br>
&emsp;&emsp; ◉ RRC message exchange through SL-SRB1 entity over PC5 interface.<br>

Based on the received Configured Grant Type 1 configurations, the SL MAC scheduler determines the transmission opportunities and allocates the corresponding sidelink resources for UE data transmission.

### 6.3 **Pre-requisite: Core Network**

&emsp; To test IP traffic using `ping` in 5G SL mode 1, the OAI Core Network must first be launched as a prerequisite. In this SRAP release, we used a proprietary 5G Core Network. For details on setting up and launching this core network, refer to the [OAI SRAP CN5G](./oai-srap-cn5g.md) installation guide.<br>

&emsp; Once the Core Network is running, the gNB, Relay UE (SyncRef UE), and Remote UE (Nearby UE) can be started either on the same machine or on separate machines. The following commands demonstrate how to test 5G SL mode 1 in an RF simulator using a single machine with one gNB, one Relay UE, and one Remote UE.

### 6.4 **Running on RF Simulator:**

&emsp; RFSim in the OAI codebase is a radio frequency (RF) simulation module that enables end-to-end testing without requiring physical RF hardware. It simulates the wireless channel and signal propagation, allowing complete testing of 4G/5G network components entirely in software. This makes RFSim particularly valuable for CI/CD pipelines, development, and validation environments, where rapid and repeatable testing is essential.

#### 6.4.1 **Launching Commands:**

&emsp; ***gNB in Terminal 1 of Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf \
--gNBs.[0].min_rxtxtime 6 --sa --rfsim \
--rfsimulator.serveraddr server --rfsimulator.serverport 4048 \
 --relay-type 1 --remote-ue-id 1  --ip-demo 1 2>&1 | tee ~/result_gNB.log
```
&emsp; ***SyncRef UE in Terminal 2 of Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
 -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
--sa --sl-mode 1 --sync-ref --rfsim \
--rfsimulator.serveraddr <MACHINE 1 IP Address> --rfsimulator.serverport 4048 \
--rfsimulator.serveraddrsl <MACHINE 1 IP Address> --rfsimulator.serverportsl 4148 \
--relay-type 1 --is-relay-ue 1 2>&1 | tee ~/result_nrUE_syncref.log
```
&emsp; ***Nearby UE in Terminal 3 of Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa --sl-mode 2 --rfsim \
--rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4148 \
--relay-type 1 2>&1 | tee ~/result_nearby.log
```
Run `ping` command on the Nearby UE terminal of Machine 1.
```
ping -I oaitun_ue2 8.8.8.8
```

To perform full system testing - including CSI reporting and PSFCH feedback - the commands remain the same. The only required step is to update the UE configuration files, as outlined below.

#### 6.4.2 **Launching GUI:**

&emsp; To launch the GUI, the required packages must be installed. Use the following command:
```
cd ~/openairinterface5g/ci-scripts/slgui
pip install -r requirements.txt
```

&emsp; To launch the 5G SL mode 1 system using the GUI, execute the following command:
```
./batch.sh
```
After launching the GUI, navigate to the `Run` tab and click the `Start` button on each GUI window. The processes will run for the specified duration.

To stop any running process, click the `Stop` button in the corresponding GUI window. Closing the GUI windows improperly may not terminate the running processes.

## 7. EpiSci's 5G Sidelink Mode 2
### 7.1 **Running on RF Simulator:**

&emsp; RFSim in the OAI codebase is a radio frequency simulation module that enables end-to-end testing without physical RF hardware. By simulating the wireless channel and signal propagation, it allows complete testing of 4G/5G network components entirely in software. This makes RFSim ideal for CI/CD pipelines, development, and validation environments.

#### 7.1.1 **Test Environment: (SL Mode 2)**

&emsp; To test IP traffic using `ping` in 5G SL mode 2, the SyncRef UE and the Nearby UE must run on separate machines. Currently, IP traffic is not supported when both processes run on the same machine.

The following commands demonstrate how to test 5G SL mode 2 with two UEs using the RF simulator.

#### 7.1.2 **Commands:**

&emsp; ***SyncRef UE on Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
--sa --sl-mode 2 --sync-ref --rfsim --thread-pool -1,-1,-1,-1 \
--rfsimulator.serveraddrsl server --rfsimulator.serverportsl 4048
```
&emsp; ***Nearby UE on Machine 2:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa --sl-mode 2 --rfsim --thread-pool -1,-1,-1,-1 \
--rfsimulator.serveraddrsl <MACHINE 1 IP Address> --rfsimulator.serverportsl 4048
```
Run `ping` command on the second terminal of Machine 2.
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

<h>Table 1: Configuration Table</h3>
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

**🔔Note:** Ensure the **sl_CSI_Acquisition** and **sl_PSFCH_period** values are set consistently across both UEs for valid test.

### 7.2&emsp;**Over-the-air (OTA) USRP Testing:**
The OTA USRP testing was conducted using two B210s. The commands below were used for testing SL mode 2 on the B210s.

The following UHD commands can be used to verify that the USRP devices are ready for deployment and to retrieve essential information such as their serial numbers and addresses.

```
uhd_find_devices # This will find all USRPs
uhd_usrp_probe # This will probe the USRP and will ensure the status is ready
```

The USRPs can be connected through either cable or over-the-air medium. In a case of cable connectivity, an attenuator can be used in lab environment to simulate real-world signal loss conditions.



#### 7.2.1 **Setting of attenuation:**

In order to set the attenuation for each channel you have to run this command
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


#### 7.2.2 **Running of SL Mode 1 on B210s**

SSH to Machine 1. Note, the serial field may need to be changed to match the USRPs:

&emsp; ***gNB in Terminal 1 of Machine 1:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210_relay_ue.conf \
--gNBs.[0].min_rxtxtime 6 --sa -E \
--usrp-args 'serial=340E9AE,type=b200' \
--ue-txgain 10 --ue-rxgain 100 \
--relay-type 1 --remote-ue-id 1  --ip-demo 1 2>&1 | tee ~/result_gNB.log
```
&emsp; ***SyncRef UE in Terminal 1 of Machine 2:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
 -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 \
--sa -E --sl-mode 1 --sync-ref \
--ue-txgain 10 --ue-rxgain 100 \
--usrp-args 'serial=340EA03,type=b200' \
--usrp-args-sl 'serial=340EA3B,type=b200' \
--relay-type 1 --is-relay-ue 1 2>&1 | tee ~/result_nrUE_syncref.log
```
&emsp; ***Nearby UE in Terminal 1 of Machine 3:***
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa -E --sl-mode 2 \
--ue-txgain 10 --ue-rxgain 100 \
--usrp-args-sl 'serial=3271246,type=b200' \
--relay-type 1 2>&1 | tee ~/result_nearby.log
```
Run `ping` command on the Nearby UE terminal of Machine 3.
```
ping -I oaitun_ue2 8.8.8.8
```
In case of using gNB tun interface for video streaming, append --noS1 flag at the end of gNB's command.

#### 7.2.3 **Running of SL Mode 2 on B210s**

SSH to Machine 1. Note, the serial field may need to be changed to match the USRPs:
```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa -E --sl-mode 2 --ue-txgain 10 --ue-rxgain 100 --usrp-args-sl "serial=3150361,type=b200" \
--thread-pool -1,-1
```

SSH to Machine 2. Note, the serial field may need to be changed to match the USRPs:

```
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH -E \
./nr-uesoftmodem -O ../../../targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
--sa -E --sl-mode 2 --sync-ref --ue-txgain 10 --ue-rxgain 100 --usrp-args-sl "serial=3150384,type=b200" \
--thread-pool -1,-1
```
Run `ping` command on the second terminal of Machine 2.
```
ping -I oaitun_ue1 10.0.0.2
```

#### 7.2.4 **Running Video Stream on B210s**

Transmitter
```
ffmpeg -re -stream_loop -1 -i ~/Videos/file_name.mp4 -f mpegts \
       "udp://10.0.1.1:1234?localaddr=10.0.0.100&pkt_size=1316"
```

Receiver
```
ffplay -flags low_delay -i udp://10.0.1.1:1234
```

#### 7.2.5 **Performance Test using Iperf3**

##### 7.2.5.1  Remote UE to UPF

Remote UE
```
iperf3 -u -c <UPF_IP address> -B <Remote UE IP address> -p 5001 -i 1 -b 1M
iperf3 -u -c 192.168.70.134 -B 10.0.0.100 -p 5001 -i 1 -b 1M
```

UPF (Inside case of UPF docker)
```
iperf3 -s -B <UPF_IP address> -p 5001 -i 1

iperf3 -s -B 192.168.70.134 -p 5001 -i 1
```

UPF (Outside case of UPF docker; in case of iperf3 installed in docker)
```
docker exec -it oai-upf bash -c 'iperf3 -s -B <UPF_IP address> -p 5001 -i 1' | tee iperf_output.log

docker exec -it oai-upf bash -c 'iperf3 -s -B 192.168.70.134 -p 5001 -i 1' | tee iperf_output.log
```

##### 7.2.5.2  UPF to Remote UE

UPF (Inside case of UPF docker)
```
iperf3 -u -c <Remote UE IP address> -B  <UPF_IP address> -p 5001 -i 1 -b 1M

iperf3 -u -c 10.0.0.100 -B 192.168.70.134 -p 5001 -i 1 -b 1M
```

or

UPF (Outside case of UPF docker; in case of iperf3 installed in docker)
```
docker exec -it oai-upf bash -c 'iperf3 -u -c <Remote UE IP address> -B <UPF_IP address>  -p 5001 -i 1 -b 1M' | tee iperf_output.log

docker exec -it oai-upf bash -c 'iperf3 -u -c 10.0.0.100 -B 192.168.70.134 -p 5001 -i 1 -b 1M' | tee iperf_output.log
```

Remote UE
```
iperf3 -s -B <Remote UE IP address> -p 5001 -i 1

iperf3 -s -B 10.0.0.100 -p 5001 -i 1
```

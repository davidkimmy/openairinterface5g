# Chat Application
This application is composed of two scripts, chat_server.py and chat_client.py.
chat_server.py provides server role, and chat_client.py provides client role.
This application has only been tested and ran with OAI in sidelink mode 2 but can be utilized for plain TCP/IP communications as well.

## Prerequisites
- Tkinter is a default package in the standard Python interface to the Tcl/Tk GUI toolkit:
 `pip install tk`

## Usage ##
### Run Chat Client with OAI ###
#### Get openairinterface5g source code ####
```bash
git clone https://gitlab.applied.dev/old_episci/csrf/5g/openairinterface5g ~/openairinterface5g
cd ~/openairinterface5g
git checkout episys/sl-eurecom2
```

#### Install OAI dependencies ####
```bash
cd ~/openairinterface5g/cmake_targets
./build_oai -I
```

#### Build OAI gNB and OAI nrUE ####
```bash
cd ~/openairinterface5g/cmake_targets
./build_oai -w SIMU --nrUE --gNB -w USRP
```

#### Run OAI nrUEs ####
After connecting the USRPs to the two host machines, open a new terminal on each machine. Note, you can validate the USRPs are properly connected with the `uhd_find_devices` and `uhd_usrp_probe` commands. Run the following commands in each terminal:

SyncRef UE terminal (terminal 1):
```bash
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH \
./nr-uesoftmodem -O ~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_sync_ref.conf \
--sa --sl-mode 2 --sync-ref --ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif --mcs 9
```

Nearby UE terminal (terminal 1):
```bash
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo -E LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH \
./nr-uesoftmodem -O ~/openairinterface5g/targets/PROJECTS/NR-SIDELINK/CONF/sl_ue1.conf \
--sa --sl-mode 2 --ue-txgain 20 --ue-rxgain 110 --device.name oai_usrpdevif --mcs 9
```

### Check tun/tap Interface ###
After launching the OAI commands, find tun/tap IP address of interface named `oaitun_ue1` using the `ifconfig` command on the SyncRef machine. Note, for validation you should see the `oaitun_ue1` IP address appropriately set as `10.0.0.1`. The SyncRef UE is the server.

### Launch chat script ###
Open two new terminals on the SyncRef (terminal 2 and termianl 3) and one new terminal on the Nearby UE (terminal 2).

#### Launch chat_server.py
SyncRef UE terminal (terminal 2):
```bash
cd ~/openairinterface5g/ci-scripts
python3 ./chat_server.py
```

#### Launch chat_client.py
Run `./chat_client.py` on the SyncRef and Nearby UE:

SyncRef UE terminal (terminal 3):
```bash
cd ~/openairinterface5g/ci-scripts
python3 ./chat_client.py
```
Nearby UE terminal (terminal 2):
```bash
cd ~/openairinterface5g/ci-scripts
python3 ./chat_client.py
```

### Test chatting ###
#### Enter Server IP address ####
After launching the scripts, you are prompted to enter the server IP address and user ID in the dialog box for each chat client.

To use OAI tun/tap interface, enter the IP address dialog box found. See the ### Check tun/tap Interface ### step if you are unsure where to get the IP address. Note, in this test, it is `10.0.0.1`.

#### Chatting ####
After the connection is established between the server and clients, you can then begin using the chat services. You can send text messages or files using the client chatting windows.
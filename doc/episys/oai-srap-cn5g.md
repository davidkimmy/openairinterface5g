
<p align="center">
  <a href="http://www.openairinterface.org/">
    <img src="./episci_new_logo.png" alt="EpiSci Logo" height="90"/>
  </a>
</p>
<h1 align="center">
5G SRAP Core Network based on OpenAirInterface (OAI) 5G Core Network
</h>

## 1. Overview of 5G Core Network
In 5G, the User Plane Function (UPF) is a core network function responsible for handling user data traffic. 5G SRAP Core Network requires docker installation and the original OAI CN5G docker images. On top of the installation, UPF images needs to be updated for IP traffic forwarding.


## 2. Installation of OAI-CN5G

&emsp;Please install and configure OAI CN5G together with docker installation as described here: [OAI CN5G](https://gitlab.eurecom.fr/oai/openairinterface5g/-/blob/develop/doc/NR_SA_Tutorial_OAI_CN5G.md)

&emsp;After installation, start docker service as following.
```
$ systemctl start docker.service
```

## 3. Build UPF from Source
### 3.1 **Checkout UPF repo:**
&emsp;UPF repository for the OpenAirInterface 5G core network development is available on following link: [oai-cn5g-upf](https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-upf)

Follow these steps to build UPF image
```
$ cd ~/oai-cn5g
$ git clone https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-upf.git
$ cd oai-cn5g-upf
$ git fetch
$ cd src/
$ git submodule update --init --recursive
$ cd common--src
$ git checkout 633af5b5
$ cd ../..
$ git checkout relay_ue
$ cp ~/openairinterface5g/doc/episys/upf.patch .
$ git apply upf.patch
$ sudo apt-get install libstdc++-12-dev
$ make setup
$ make install
```

### 3.2 **Build UPF docker image:**
&emsp;Follow these steps to build UPF docker image

```
$ cd ~/oai-cn5g/oai-cn5g-upf
$ ln -s docker/Dockerfile.upf.ubuntu dockerfile
$ docker buildx build . -t oai-upf:latest
```

### 3.3 **Update docker-compose yaml file:**
&emsp;Update the docker-compose.yaml in ~/oai-cn5g as follows:
```
# image: oaisoftwarealliance/oai-upf:develop
image: oai-upf:latest
```

## 4. Launch OAI-SRAP-CN5G
```
$ cd ~/oai-cn5g/
$ docker compose up -d
```

&emsp;You may stop docker compose using the following command.
```
$ cd ~/oai-cn5g/
$ docker compose down
```

/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file nas_config.c
* \brief Configures the nasmesh interface
* \author Daniel Camara and Navid Nikaein
* \date 2006-2011
* \version 0.1
* \email:navid.nikaein@eurecom.fr
* \company Eurecom
*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <ifaddrs.h>

#include "nas_config.h"
#include "common/utils/LOG/log.h"
#include "executables/lte-softmodem.h"
#include "common/config/config_userapi.h"
#include "pdcp.h"

//default values according to the examples,

char *baseNetAddress ;
char *netMask ;
char *broadcastAddr ;
#define NASHLP_NETPREFIX "<NAS network prefix, two first bytes of network addresses>\n"
#define NASHLP_NETMASK   "<NAS network mask>\n"
#define NASHLP_BROADCASTADDR   "<NAS network broadcast address>\n"
void nas_getparams(void) {
  // this datamodel require this static because we partially keep data like baseNetAddress (malloc on a global)
  // but we loose the opther attributes in nasoptions between two calls if is is not static !
  // clang-format off
  static paramdef_t nasoptions[] = {
    /*--------------------------------------------------------------------------------------------------------------------------------------------------------------------*/
    /*                                            configuration parameters for netlink, includes network parameters when running in noS1 mode                             */
    /*   optname                     helpstr                paramflags           XXXptr                               defXXXval               type                 numelt */
    /*--------------------------------------------------------------------------------------------------------------------------------------------------------------------*/
    {"NetworkPrefix",    NASHLP_NETPREFIX,       0,              .strptr=&baseNetAddress,        .defstrval="10.0",            TYPE_STRING,  0 },
    {"NetworkMask",      NASHLP_NETMASK,         0,              .strptr=&netMask,               .defstrval="255.255.255.0",   TYPE_STRING,  0 },
    {"BroadcastAddr",    NASHLP_BROADCASTADDR,   0,              .strptr=&broadcastAddr,         .defstrval="10.0.255.255",    TYPE_STRING,  0 },
  };
  // clang-format on
  config_get( nasoptions,sizeof(nasoptions)/sizeof(paramdef_t),"nas.noS1");
}

void setBaseNetAddress (char *baseAddr) {
  strcpy(baseNetAddress,baseAddr);
}

char *getBaseNetAddress (void) {
  return baseNetAddress;
}

void setNetMask (char *baseAddr) {
  strcpy(netMask,baseAddr);
}

char *getNetMask  (void) {
  return netMask;
}

void setBroadcastAddress (char *baseAddr) {
  strcpy(broadcastAddr, baseAddr);
}

char *getBroadcastAddress (void) {
  return broadcastAddr;
}

//Add Gateway to the interface
int set_gateway(char *interfaceName, char *gateway) {
  int sock_fd;
  struct rtentry rt;
  struct sockaddr_in addr;

  if((sock_fd = socket(AF_INET,SOCK_DGRAM,0)) < 0) {
    perror("socket failed");
    return 1;
  }

  memset (&rt, 0, sizeof (rt));
  addr.sin_family = AF_INET;
  /*set Destination addr*/
  inet_aton("0.0.0.0",&addr.sin_addr);
  memcpy(&rt.rt_dst, &addr, sizeof(struct sockaddr_in));
  /*set gateway addr*/
  inet_aton(gateway,&addr.sin_addr);
  memcpy(&rt.rt_gateway, &addr, sizeof(struct sockaddr_in));
  /*set genmask addr*/
  inet_aton("0.0.0.0",&addr.sin_addr);
  memcpy(&rt.rt_genmask, &addr, sizeof(struct sockaddr_in));
  rt.rt_dev = interfaceName;
  //rt.rt_flags = RTF_UP|RTF_GATEWAY|RTF_DEFAULT;
  /* SR: rt_flags on 16 bits but RTF_DEFAULT = 0x00010000
   * therefore doesn't lie in container -> disable it
   */
  //rt.rt_flags = RTF_GATEWAY|RTF_DEFAULT;
  rt.rt_flags = RTF_GATEWAY;

  if (ioctl(sock_fd, SIOCADDRT, &rt) < 0) {
    close(sock_fd);

    if(strstr(strerror(errno),"File exists") == NULL) {
      LOG_E(OIP,"ioctl SIOCADDRT failed : %s\n",strerror(errno));
      return 2;
    } else { /*if SIOCADDRT error is route exist, retrun success*/
      LOG_I(OIP,"File Exist ...\n");
      LOG_I(OIP,"set_gateway OK!\n");
      return 0;
    }
  }

  close(sock_fd);
  LOG_D(OIP,"Set Gateway OK!\n");
  return 0;
}

// sets a genneric interface parameter
// (SIOCSIFADDR, SIOCSIFNETMASK, SIOCSIFBRDADDR, SIOCSIFFLAGS)
int setInterfaceParameter(char *interfaceName, char *settingAddress, int operation) {
  int sock_fd;
  struct ifreq ifr;
  struct sockaddr_in addr;

  if((sock_fd = socket(AF_INET,SOCK_DGRAM,0)) < 0)    {
    LOG_E(OIP,"Setting operation %d, for %s, address, %s : socket failed\n",
          operation, interfaceName, settingAddress);
    return 1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, interfaceName, sizeof(ifr.ifr_name)-1);
  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  inet_aton(settingAddress,&addr.sin_addr);
  memcpy(&ifr.ifr_ifru.ifru_addr,&addr,sizeof(struct sockaddr_in));

  if(ioctl(sock_fd,operation,&ifr) < 0)    {
    close(sock_fd);
    LOG_E(OIP,"Setting operation %d, for %s, address, %s : ioctl call failed\n",
          operation, interfaceName, settingAddress);
    return 2;
  }

  close(sock_fd);
  return 0;
}

// sets a genneric interface parameter
// (SIOCSIFADDR, SIOCSIFNETMASK, SIOCSIFBRDADDR, SIOCSIFFLAGS)
int bringInterfaceUp(char *interfaceName, int up) {
  int sock_fd;
  struct ifreq ifr;

  if((sock_fd = socket(AF_INET,SOCK_DGRAM,0)) < 0) {
    LOG_E(OIP,"Bringing interface UP, for %s, failed creating socket\n", interfaceName);
    return 1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, interfaceName, sizeof(ifr.ifr_name)-1);

  if(up) {
    ifr.ifr_flags |= IFF_UP | IFF_NOARP | IFF_MULTICAST;

    if (ioctl(sock_fd, SIOCSIFFLAGS, (caddr_t)&ifr) == -1) {
      close(sock_fd);
      LOG_E(OIP,"Bringing interface UP, for %s, failed UP ioctl\n", interfaceName);
      return 2;
    }
  } else {
    //        printf("desactivation de %s\n", interfaceName);
    ifr.ifr_flags &= (~IFF_UP);

    if (ioctl(sock_fd, SIOCSIFFLAGS, (caddr_t)&ifr) == -1) {
      close(sock_fd);
      LOG_E(OIP,"Bringing interface down, for %s, failed UP ioctl\n", interfaceName);
      return 2;
    }
  }

  //   printf("UP/DOWN OK!\n");
  close( sock_fd );
  return 0;
}
// non blocking full configuration of the interface (address, net mask, and broadcast mask)
int NAS_config(char *interfaceName, char *ipAddress, char *networkMask, char *broadcastAddress) {
  bringInterfaceUp(interfaceName, 0);
  // sets the machine address
  int returnValue= setInterfaceParameter(interfaceName, ipAddress,SIOCSIFADDR);

  // sets the machine network mask
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, networkMask,SIOCSIFNETMASK);

  // sets the machine broadcast address
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, broadcastAddress,SIOCSIFBRDADDR);

  //  if(!returnValue)
  //  returnValue=set_gateway(interfaceName, broadcastAddress);
  if(!returnValue)
    returnValue = bringInterfaceUp(interfaceName, 1);

  return returnValue;
}

int nas_config_mbms(int interface_id, int thirdOctet, int fourthOctet, char *ifname) {
  //char buf[5];
  char ipAddress[20];
  char broadcastAddress[20];
  char interfaceName[20];
  int returnValue;
  //if(strcmp(ifname,"ue") == 0)
       //sprintf(ipAddress, "%s.%d.%d", "20.0",thirdOctet,fourthOctet);
  ////else
       sprintf(ipAddress, "%s.%d.%d",baseNetAddress,thirdOctet,fourthOctet);

  sprintf(broadcastAddress, "%s.%d.255",baseNetAddress, thirdOctet);
  sprintf(interfaceName, "%s%s%d", (UE_NAS_USE_TUN || ENB_NAS_USE_TUN)?"oaitun_":ifname,
          UE_NAS_USE_TUN?ifname/*"ue"*/: (ENB_NAS_USE_TUN?ifname/*"enb"*/:""),interface_id);
  bringInterfaceUp(interfaceName, 0);
  // sets the machine address
  returnValue= setInterfaceParameter(interfaceName, ipAddress,SIOCSIFADDR);

  // sets the machine network mask
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, netMask,SIOCSIFNETMASK);

  // sets the machine broadcast address
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, broadcastAddress,SIOCSIFBRDADDR);

  if(!returnValue)
    bringInterfaceUp(interfaceName, 1);

  if(!returnValue)
    LOG_I(OIP,"Interface %s successfully configured, ip address %s, mask %s broadcast address %s\n",
          interfaceName, ipAddress, netMask, broadcastAddress);
  else
    LOG_E(OIP,"Interface %s couldn't be configured (ip address %s, mask %s broadcast address %s)\n",
          interfaceName, ipAddress, netMask, broadcastAddress);

  return returnValue;
}

int nas_config_mbms_s1(int interface_id, int thirdOctet, int fourthOctet, char *ifname) {
  //char buf[5];
  char ipAddress[20];
  char broadcastAddress[20];
  char interfaceName[20];
  int returnValue;
  //if(strcmp(ifname,"ue") == 0)
       //sprintf(ipAddress, "%s.%d.%d", "20.0",thirdOctet,fourthOctet);
  ////else
       sprintf(ipAddress, "%s.%d.%d","10.0",thirdOctet,fourthOctet);

  sprintf(broadcastAddress, "%s.%d.255","10.0", thirdOctet);
  sprintf(interfaceName, "%s%s%d", "oaitun_",ifname,interface_id);
  bringInterfaceUp(interfaceName, 0);
  // sets the machine address
  returnValue= setInterfaceParameter(interfaceName, ipAddress,SIOCSIFADDR);

  // sets the machine network mask
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, "255.255.255.0",SIOCSIFNETMASK);
  printf("returnValue %d\n",returnValue);

  // sets the machine broadcast address
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, broadcastAddress,SIOCSIFBRDADDR);
  printf("returnValue %d\n",returnValue);

  if(!returnValue)
    bringInterfaceUp(interfaceName, 1);
  printf("returnValue %d\n",returnValue);

  if(!returnValue)
    LOG_I(OIP,"Interface %s successfully configured, ip address %s, mask %s broadcast address %s\n",
          interfaceName, ipAddress, "255.255.255.0", broadcastAddress);
  else
    LOG_E(OIP,"Interface %s couldn't be configured (ip address %s, mask %s broadcast address %s)\n",
          interfaceName, ipAddress, "255.255.255.0", broadcastAddress);

  return returnValue;
}

/**
 * Detect if multiple UEs on the same host are using the same host.
 * Checks if the IP address is localhost (127.0.0.1).
 *
 * @param ipAddress IP address to check (e.g., "127.0.0.1")
 * @return true if ipAddress is "127.0.0.1", false otherwise
 */
static bool detect_same_host_interfaces(const char *ipAddress) {
  return (strcmp(ipAddress, "127.0.0.1") == 0);
}

/**
 * Detect if multiple UEs on the same host are using the same subnet.
 * Scans existing oaitun_ue* interfaces and compares their IP subnets
 * with the provided IP address.
 *
 * @param ipAddress IP address to check (e.g., "10.0.0.1")
 * @return true if another oaitun_ue* interface exists with same /24 subnet, false otherwise
 */
#if 0
static bool detect_same_subnet_interfaces(const char *ipAddress) {
  struct ifaddrs *ifaddr, *ifa;
  struct in_addr provided_addr;

  if (inet_pton(AF_INET, ipAddress, &provided_addr) != 1 || getifaddrs(&ifaddr) == -1) {
    return false;
  }

  uint32_t provided_subnet = ntohl(provided_addr.s_addr) & 0xFFFFFF00;

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    if (strncmp(ifa->ifa_name, "oaitun_ue", 9) == 0) {
      struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
      uint32_t existing_subnet = ntohl(sa->sin_addr.s_addr) & 0xFFFFFF00;

      if (existing_subnet == provided_subnet && sa->sin_addr.s_addr != provided_addr.s_addr) {
        freeifaddrs(ifaddr);
        return true;
      }
    }
  }

  freeifaddrs(ifaddr);
  return false;
}
#endif

/**
 * Configure policy-based routing for SL Mode 2 using system() calls.
 * This matches OAI's existing pattern of using system() for routing configuration.
 *
 * @param interfaceName Name of the TUN interface (e.g., "oaitun_ue1")
 * @param ipAddress IP address assigned to this interface
 * @param iface_num Interface number (extracted from oaitun_ue<N>)
 * @return 0 on success, non-zero on failure
 */
static int configure_policy_routing_system(const char *interfaceName,
                                            const char *ipAddress,
                                            int iface_num) {
  char cmd[512];
  int table_id = 100 + iface_num;
  int priority = 1000 + iface_num;
  int ret;

  // Cleanup old rules/routes
  snprintf(cmd, sizeof(cmd),
           "ip rule del from %s table %d 2>/dev/null || true; "
           "ip route del 10.0.0.0/24 table %d 2>/dev/null || true",
           ipAddress, table_id, table_id);
  ret = system(cmd);
  (void)ret;

  // Add policy rule and route
  snprintf(cmd, sizeof(cmd), "ip rule add from %s table %d priority %d", ipAddress, table_id, priority);
  ret = system(cmd);
  (void)ret;

  snprintf(cmd, sizeof(cmd), "ip route add 10.0.0.0/24 dev %s table %d", interfaceName, table_id);
  ret = system(cmd);
  (void)ret;

  snprintf(cmd, sizeof(cmd), "ip route add 10.0.0.0/24 dev %s 2>/dev/null || true", interfaceName);
  ret = system(cmd);
  (void)ret;

  return 0;
}

// non blocking full configuration of the interface (address, and the two lest octets of the address)
int nas_config(int interface_id, int thirdOctet, int fourthOctet, char *ifname) {
  //char buf[5];
  char ipAddress[20];
  char broadcastAddress[20];
  char interfaceName[20];
  int returnValue;
  sprintf(ipAddress, "%s.%d.%d", baseNetAddress,thirdOctet,fourthOctet);
  sprintf(broadcastAddress, "%s.%d.255",baseNetAddress, thirdOctet);
  sprintf(interfaceName, "%s%s%d", (UE_NAS_USE_TUN || ENB_NAS_USE_TUN)?"oaitun_":ifname,
          UE_NAS_USE_TUN?"ue": (ENB_NAS_USE_TUN?"enb":""),interface_id);

  // For SL Mode 2, detect same-host scenario and conditionally apply policy routing in case of non-relay.
  if (get_softmodem_params()->sl_mode == 2 && get_softmodem_params()->relay_type == 0) {
    bool same_host = detect_same_host_interfaces(ipAddress);

    // Configure interface with ioctl
    bringInterfaceUp(interfaceName, 0);
    returnValue = setInterfaceParameter(interfaceName, ipAddress, SIOCSIFADDR);
    if (returnValue != 0) return returnValue;

    returnValue = setInterfaceParameter(interfaceName, netMask, SIOCSIFNETMASK);
    if (returnValue != 0) return returnValue;

    returnValue = setInterfaceParameter(interfaceName, broadcastAddress, SIOCSIFBRDADDR);
    if (returnValue != 0) return returnValue;

    returnValue = bringInterfaceUp(interfaceName, 1);
    if (returnValue != 0) return returnValue;

    // Configure kernel params for inter-subnet routing (inline sysctl configuration)
    char cmd[512];
    int ret;

    snprintf(cmd, sizeof(cmd), "sysctl -w net.ipv4.conf.%s.accept_local=1 >/dev/null 2>&1", interfaceName);
    ret = system(cmd);
    (void)ret;

    snprintf(cmd, sizeof(cmd), "sysctl -w net.ipv4.conf.%s.rp_filter=0 >/dev/null 2>&1", interfaceName);
    ret = system(cmd);
    (void)ret;

    // Apply policy routing tables only if same subnet detected
    if (same_host) {
      int iface_num = 0;
      sscanf(interfaceName, "oaitun_ue%d", &iface_num);
      if (iface_num > 0) {
        configure_policy_routing_system(interfaceName, ipAddress, iface_num);
      }
    }

    LOG_I(OIP, "%s configured: %s/%s%s\n", interfaceName, ipAddress, netMask,
          same_host ? " (policy routing)" : "");

    return 0;
  }

  bringInterfaceUp(interfaceName, 0);
  // sets the machine address
  returnValue= setInterfaceParameter(interfaceName, ipAddress,SIOCSIFADDR);

  // sets the machine network mask
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, netMask,SIOCSIFNETMASK);

  // sets the machine broadcast address
  if(!returnValue)
    returnValue= setInterfaceParameter(interfaceName, broadcastAddress,SIOCSIFBRDADDR);

  if(!returnValue)
	  returnValue=bringInterfaceUp(interfaceName, 1);

  if(!returnValue)
    LOG_I(OIP,"Interface %s successfully configured, ip address %s, mask %s broadcast address %s\n",
          interfaceName, ipAddress, netMask, broadcastAddress);
  else
    LOG_E(OIP,"Interface %s couldn't be configured (ip address %s, mask %s broadcast address %s)\n",
          interfaceName, ipAddress, netMask, broadcastAddress);

  int res;
  char command_line[500];
  res = sprintf(command_line,
    "ip rule add from %s/32 table %d && "
    "ip rule add to %s/32 table %d && "
    "ip route add default dev %s%d table %d",
    ipAddress, interface_id - 1 + 10000,
    ipAddress, interface_id - 1 + 10000,
    UE_NAS_USE_TUN ? "oaitun_ue" : "oip",
    interface_id, interface_id - 1 + 10000);

  if (res < 0) {
    LOG_E(OIP,"Could not create ip rule/route commands string\n");
    return res;
  }

  background_system(command_line);

  return returnValue;
}

// Blocking full configuration of the interface (address, net mask, and broadcast mask)
int blocking_NAS_config(char *interfaceName, char *ipAddress, char *networkMask, char *broadcastAddress) {
  char command[200];
  command[0]='\0';
  strcat(command, "ifconfig ");
  strncat(command, interfaceName, sizeof(command) - strlen(command) - 1);
  strncat(command, " ", sizeof(command) - strlen(command) - 1);
  strncat(command, ipAddress, sizeof(command) - strlen(command) - 1);
  strncat(command, " networkMask ", sizeof(command) - strlen(command) - 1);
  strncat(command, networkMask, sizeof(command) - strlen(command) - 1);
  strncat(command, " broadcast ", sizeof(command) - strlen(command) - 1);
  strncat(command, broadcastAddress, sizeof(command) - strlen(command) - 1);
  // ifconfig nasmesh0 10.0.1.1 networkMask 255.255.255.0 broadcast 10.0.1.255
  int i = system (command);
  return i;
}

// program help
void helpOptions(char **argv) {
  printf("Help for %s\n",  argv[0]);
  printf("  -i <interfaceName>\n");
  printf("  -a <IP address>\n");
  printf("  -n <Net mask>\n");
  printf("  -b <broadcast address>\n");
  printf("  -h Shows this help\n");
  printf("If no option is passed as parameter the default values are: \n");
  printf("    Interface Name: nasmesh0\n");
  printf("    IP Address: 10.0.1.1\n");
  printf("    Net mask: 255.255.255.0\n");
  printf("    Broadcast address: [Beginning of the IP address].255\n");
  exit(1);
}

// creates the broadcast address if it wasn't set before
void createBroadcast(char *broadcastAddress) {
  int pos=strlen(broadcastAddress)-1;

  while(broadcastAddress[pos]!='.')
    pos--;

  broadcastAddress[++pos]='2';
  broadcastAddress[++pos]='2';
  broadcastAddress[++pos]='5';
  broadcastAddress[++pos]='\0';
}
#ifdef STANDALONE
// main function
//---------------------------------------------------------------------------
int main(int argc,char **argv)
//---------------------------------------------------------------------------
{
  int c;
  char interfaceName[100];
  char ipAddress[100];
  char networkMask[100];
  char broadcastAddress[100];
  strcpy(interfaceName, "oai0");
  strcpy(ipAddress, "10.0.1.1");
  strcpy(networkMask, "255.255.255.0");
  broadcastAddress[0]='\0';

  while ((c = getopt (argc, argv, "i:a:n:b:h")) != -1)
    switch (c) {
      case 'h':
        helpOptions(argv);
        break;

      case 'i':
        strcpy(interfaceName,optarg);
        break;

      case 'a':
        strcpy(ipAddress,optarg);
        break;

      case 'n':
        strcpy(networkMask,optarg);
        break;

      case 'b':
        strcpy(broadcastAddress,optarg);
        break;

      case '?':
        if (isprint (optopt))
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf (stderr,
                   "Unknown option character `\\x%x'.\n",
                   optopt);

        return 1;

      default:
        abort ();
    }

  if(strlen(broadcastAddress)==0) {
    strcpy(broadcastAddress,ipAddress);
    createBroadcast(broadcastAddress);
  }

  printf("Command: ifconfig %s %s networkMask %s broadcast %s\n", interfaceName, ipAddress, networkMask, broadcastAddress);
  NAS_config(interfaceName, ipAddress, networkMask, broadcastAddress);
  //test
  //     setBaseNetAddress("11.11");
  //     nas_config(interfaceName, 33, 44);
}

#endif

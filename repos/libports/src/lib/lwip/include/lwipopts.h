/*
 * \brief  Configuration file for LwIP, adapt it to your needs.
 * \author Stefan Kalkowski
 * \author Emery Hemingway
 * \date   2009-11-10
 *
 * See lwip/src/include/lwip/opt.h for all options
 */

/*
 * Copyright (C) 2009-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef __LWIP__LWIPOPTS_H__
#define __LWIP__LWIPOPTS_H__

/* Genode includes */
#include <base/fixed_stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Use lwIP without OS-awareness
 */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0

#define LWIP_DNS                    1  /* DNS support */
#define LWIP_DHCP                   1  /* DHCP support */
#define LWIP_SOCKET                 0  /* LwIP socket API */
#define LWIP_NETIF_LOOPBACK         1  /* Looping back to same address? */
#define LWIP_STATS                  0  /* disable stating */
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_TCP_TIMESTAMPS         1
#define SO_REUSE                    1
#define TCP_LISTEN_BACKLOG          1
#define TCP_MSS                     1460
#define TCP_WND                     (80 * TCP_MSS)
#define TCP_SND_BUF                 (80 * TCP_MSS)
#define LWIP_WND_SCALE              3
#define TCP_RCV_SCALE               2
#define TCP_SND_QUEUELEN            ((8 * (TCP_SND_BUF) + (TCP_MSS - 1))/(TCP_MSS))

/*
 * Narrow the ephemeral TCP port range to sixteen ports.
 *
 * lwIP's default is IANA's whole dynamic range, 49152-65535. That is the
 * right default and it makes one thing impossible: forwarding inbound
 * connections to a port the stack picks for itself.
 *
 * Passive-mode FTP needs exactly that. The server binds a data socket, the
 * stack chooses the port, and the client is told to connect to it -- so every
 * port the stack might choose has to be reachable in advance. Under qemu that
 * means a hostfwd rule per port, and 16384 rules is not a configuration.
 *
 * Sixteen is plenty for the workload (one data connection at a time) and
 * short enough to enumerate in a run script. Constraining the range this way
 * is ordinary practice for a host behind NAT rather than a workaround
 * peculiar to this port -- it is what FTP server documentation has always
 * told people to do.
 *
 * The range must sit OUTSIDE 49152-65535, and that is the whole reason it is
 * 0x5000 here rather than a shortened slice of lwIP's default. nic_router's
 * NAT port allocator owns exactly that span --
 * repos/os/src/server/nic_router/port_allocator.h: "FIRST_PORT = 49152,
 * NR_OF_PORTS = 16384" -- so a tcp-forward rule for a port inside it
 * collides with the allocator. The symptom is not an error message: the
 * router comes up, reports both domains, and then never grants the client
 * its Nic session, so the guest sits in DHCP forever and nothing downstream
 * ever starts. lwIP's default ephemeral range IS that span, which is why
 * passive-mode FTP through nic_router cannot work until lwIP is moved out
 * of it.
 *
 * All three macros have to be defined together: lwIP's tcp.c wraps them in
 * one "#ifndef TCP_LOCAL_PORT_RANGE_START", so defining the start alone
 * leaves the other two undefined. The default TCP_ENSURE_LOCAL_PORT_RANGE is
 * a mask-and-add that assumes the range runs to 0xffff, and with a short
 * range it can seed tcp_port past the end -- where tcp_new_port's "== END"
 * wrap test never fires and the stack walks out of the range entirely. The
 * modulo below cannot do that.
 */
#define TCP_LOCAL_PORT_RANGE_START  0x5000                 /* 20480 */
#define TCP_LOCAL_PORT_RANGE_END    0x500f                 /* 20495 */
#define TCP_ENSURE_LOCAL_PORT_RANGE(port) \
    ((u16_t)(TCP_LOCAL_PORT_RANGE_START + \
             ((port) % (u16_t)(TCP_LOCAL_PORT_RANGE_END - \
                               TCP_LOCAL_PORT_RANGE_START + 1))))

#define LWIP_NETIF_STATUS_CALLBACK  1  /* callback function used for interface changes */
#define LWIP_NETIF_LINK_CALLBACK    1  /* callback function used for link-state changes */

#define ARP_QUEUEING                1  /* queue packets during address resolutions, important for UDP */


/***********************************
 ** Checksum calculation settings **
 ***********************************/

/* checksum calculation for outgoing packets can be disabled if the hardware supports it */
#define LWIP_CHECKSUM_ON_COPY       1  /* calculate checksum during memcpy */

/*********************
 ** Memory settings **
 *********************/

#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1
/* MEM_ALIGNMENT > 4 e.g. for x86_64 are not supported, see Genode issue #817 */
#define MEM_ALIGNMENT               4

#define DEFAULT_ACCEPTMBOX_SIZE   128
#define TCPIP_MBOX_SIZE           128

#define RECV_BUFSIZE_DEFAULT        (512*1024)

#define PBUF_POOL_SIZE             96

#define MEMP_NUM_SYS_TIMEOUT        16
#define MEMP_NUM_TCP_PCB           128

#ifndef MEMCPY
#define MEMCPY(dst,src,len)             genode_memcpy(dst,src,len)
#endif

#ifndef MEMMOVE
#define MEMMOVE(dst,src,len)            genode_memmove(dst,src,len)
#endif

/********************
 ** Debug settings **
 ********************/

/* #define LWIP_DEBUG */
/* #define DHCP_DEBUG      LWIP_DBG_ON */
/* #define ETHARP_DEBUG    LWIP_DBG_ON */
/* #define NETIF_DEBUG     LWIP_DBG_ON */
/* #define PBUF_DEBUG      LWIP_DBG_ON */
/* #define API_LIB_DEBUG   LWIP_DBG_ON */
/* #define API_MSG_DEBUG   LWIP_DBG_ON */
/* #define SOCKETS_DEBUG   LWIP_DBG_ON */
/* #define ICMP_DEBUG      LWIP_DBG_ON */
/* #define INET_DEBUG      LWIP_DBG_ON */
/* #define IP_DEBUG        LWIP_DBG_ON */
/* #define IP_REASS_DEBUG  LWIP_DBG_ON */
/* #define RAW_DEBUG       LWIP_DBG_ON */
/* #define MEM_DEBUG       LWIP_DBG_ON */
/* #define MEMP_DEBUG      LWIP_DBG_ON */
/* #define SYS_DEBUG       LWIP_DBG_ON */
/* #define TCP_DEBUG       LWIP_DBG_ON */


/*
   ----------------------------------
   ---------- DHCP options ----------
   ----------------------------------
*/

#define LWIP_DHCP_CHECK_LINK_UP         1


/*
   ----------------------------------------------
   ---------- Sequential layer options ----------
   ----------------------------------------------
*/
/* no Netconn API */
#define LWIP_NETCONN                    0


/*
   ---------------------------------------
   ---------- IPv6 options ---------------
   ---------------------------------------
*/

#define LWIP_IPV6                       1
#define IPV6_FRAG_COPYHEADER            1

#ifdef __cplusplus
}
#endif

#endif /* __LWIP__LWIPOPTS_H__ */

/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <ctype.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <SystemConfiguration/SCPrivate.h>

#include "configthreads_common.h"
#include "globals.h"
#include "ip6config_utils.h"

CFArrayRef 
SCNetworkReachabilityCopyResolvedAddress(SCNetworkReachabilityRef target,
                                            int * error_num);

typedef struct {
    SCNetworkReachabilityRef	target;
    struct in6_addr	our_6to4_addr;
    int			our_prefixLen;
    int			addr_flags;
    struct in6_addr	our_relay;
    struct in6_addr	prefixmask;
    struct in6_addr	netaddr;
} Service_6to4_t;

#define TEN_NET		0x0a000000	/* 10.0.0.0 */
#define IN_TEN_NET(i)	(((u_int32_t)(i) & 0xff000000) == TEN_NET)
#define ONE_NINE_TWO_NET	0xc0a80000	/* 192.168.0.0 */
#define IN_ONE_NINE_TWO_NET(i)	(((u_int32_t)(i) & 0xffff0000) == ONE_NINE_TWO_NET)
#define ONE_SEVEN_TWO_NET_START	0xac100000	/* 172.16.0.0 */
#define ONE_SEVEN_TWO_NET_END	0xac1f0000	/* 172.31.0.0 */
#define IN_ONE_SEVEN_TWO_NET(i)	\
    ((((u_int32_t)(i) & 0xfff00000) >= ONE_SEVEN_TWO_NET_START) \
        && (((u_int32_t)(i) & 0xfff00000) <= ONE_SEVEN_TWO_NET_END))
#define IS_NOT_ROUTABLE_IP(i) \
    ((IN_TEN_NET(i)) || (IN_ONE_NINE_TWO_NET(i)) || (IN_ONE_SEVEN_TWO_NET(i)))

static boolean_t
stf_get_valid_ip4(ip6config_method_data_t * method_data, struct in_addr * ip4_addr)
{
    int		i, count = method_data->stf_data.n_ip4;
    boolean_t	found = FALSE;
    
    if (method_data->stf_data.ip4_addrs_list != NULL) {
        for (i = 0; i < count; i++) {
            if (IN_LINKLOCAL(method_data->stf_data.ip4_addrs_list[i].s_addr)
                    || IS_NOT_ROUTABLE_IP(method_data->stf_data.ip4_addrs_list[i].s_addr)) {
                my_log(LOG_DEBUG, "6to4: not a good address");
                continue;
            }
            
            *ip4_addr = method_data->stf_data.ip4_addrs_list[i];
            found = TRUE;
            break;
        }
    }
    
    return (found);
}

static void
stf_construct_6to4_address(struct in_addr * ip4_addr, struct in6_addr * ip6_addr, boolean_t relay)
{
    char	str[64];
    uint32_t	tmp_addr = ntohl((uint32_t)ip4_addr->s_addr);

    /*	Constructing 6to4 address:
     *		- start with 2002 prefix
     *		- add each byte of ip4 addr like:
     *			%02x%02x:%02x%02x 0102:0304 (1.2.3.4)
     *		- append v6_net (1) if not relay
     *		- append hostbits6 (::1) if not relay
     *	So, for example:
     *		ip4 addr:	1.2.3.4
     *		ip6 addr:	2002:0102:0304:0001:0000:0000:0000:0001
     */
     
    ip6_addr->s6_addr[0] = 0x20;
    ip6_addr->s6_addr[1] = 0x02;
    
    ip6_addr->s6_addr[2] = ((tmp_addr & 0xff000000) >> 24);
    ip6_addr->s6_addr[3] = ((tmp_addr & 0x00ff0000) >> 16);
    ip6_addr->s6_addr[4] = ((tmp_addr & 0x0000ff00) >> 8);
    ip6_addr->s6_addr[5] = (tmp_addr & 0x000000ff);
    
    ip6_addr->s6_addr[6] = 0x00;
    if (relay == FALSE) {
        ip6_addr->s6_addr[7] = 0x01;
    }
    else {
        ip6_addr->s6_addr[7] = 0x00;
    }
    ip6_addr->s6_addr[8] = 0x00;
    ip6_addr->s6_addr[9] = 0x00;
    ip6_addr->s6_addr[10] = 0x00;
    ip6_addr->s6_addr[11] = 0x00;
    ip6_addr->s6_addr[12] = 0x00;
    ip6_addr->s6_addr[13] = 0x00;
    ip6_addr->s6_addr[14] = 0x00;
    if (relay == FALSE) {
        ip6_addr->s6_addr[15] = 0x01;
    }
    else {
        ip6_addr->s6_addr[15] = 0x00;
    }

    inet_ntop(AF_INET6, (const void *)ip6_addr, str, sizeof(str));
    my_log(LOG_DEBUG, "6to4: constructed address: %s", str);
    return;
}

#define DEFAULT_6TO4_PREFIXLEN 16

static void
stf_configure_address(Service_t * service_p, void * event_data)
{
    Service_6to4_t *		stf = (Service_6to4_t *)service_p->private;
    start_event_data_t *	evdata = ((start_event_data_t *)event_data);
    ip6config_method_data_t *	stf_config_data = evdata->config.data;
    struct in_addr		ip4_addr;

    /* get first routable address */
    if (stf_get_valid_ip4(stf_config_data, &ip4_addr) == FALSE) {
        my_log(LOG_ERR, "ip6config: bad or no ip4 address for 6to4");
        return;
    }
    
    /* create 6to4 address */
    stf_construct_6to4_address(&ip4_addr, &stf->our_6to4_addr, FALSE);
    
    stf->our_prefixLen = DEFAULT_6TO4_PREFIXLEN;
    
    return;
}

static void
stf_resolve_callback(SCNetworkReachabilityRef target, 
                    SCNetworkConnectionFlags flags, 
                    void *info)
{
    Service_t *		service_p = (Service_t *)info;
    Service_6to4_t *	stf = service_p->private;
    CFArrayRef		relay_addrs = NULL;
    int			i, n, err, found = 0;
    
    if ((flags & kSCNetworkFlagsReachable) 
            && !(flags & kSCNetworkFlagsConnectionRequired)) {
        relay_addrs = SCNetworkReachabilityCopyResolvedAddress(target,
                                                                &err);
    }

    if (relay_addrs == NULL) {
        my_log(LOG_DEBUG, "STF_RESOLVE: relay address not reachable.");
        return;
    }
    
    SCLog(G_verbose, LOG_INFO, CFSTR("STF_RESOLVE: relays: %@"), relay_addrs);
    
    n = CFArrayGetCount(relay_addrs);
    
    /* First try finding any v6 addresses.
     * Only try finding v4 if there are no v6
     */
    for (i = 0; i < n; i++) {
        CFDataRef	data = NULL;
        struct sockaddr	*sa;
        
        data = CFArrayGetValueAtIndex(relay_addrs, i);
        if (data == NULL) {
            my_log(LOG_DEBUG, "STF_RESOLVE: array member %d contains no data", i);
            continue;
        }
        
        sa = (struct sockaddr *)CFDataGetBytePtr(data);
        if (sa->sa_family == AF_INET6) {
            char	buf[64];
            struct sockaddr_in6 * sa6 = (struct sockaddr_in6 *)sa;
            
            /* this is a v6 address, use it */
            inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
            my_log(LOG_DEBUG, "STF_RESOLVE: returned addr is IPv6: %s", buf);
            memcpy(&stf->our_relay, &sa6->sin6_addr, sizeof(struct in6_addr));
            found = 1;
            break;
        }
    }
    
    if (!found) {
        for (i = 0; i < n; i++) {
            CFDataRef	data = NULL;
            struct sockaddr	*sa;
            struct in6_addr	tmp6_addr = IN6ADDR_ANY_INIT;
            
            data = CFArrayGetValueAtIndex(relay_addrs, i);
            if (data == NULL) {
                my_log(LOG_DEBUG, "STF_RESOLVE: array member %d contains no data", i);
                continue;
            }
            
            sa = (struct sockaddr *)CFDataGetBytePtr(data);
            if (sa->sa_family == AF_INET) {
                struct sockaddr_in *	sa4 = (struct sockaddr_in *)sa;
                char			buf1[64], buf2[64];
                
                /* this is a v4 address, convert it */
                stf_construct_6to4_address(&sa4->sin_addr, &tmp6_addr, TRUE);
                inet_ntop(AF_INET, &sa4->sin_addr, buf1, sizeof(buf1));
                inet_ntop(AF_INET6, &tmp6_addr, buf2, sizeof(buf2));
                my_log(LOG_DEBUG, "STF_RESOLVE: returned addr is IPv4: %s %s", buf1, buf2);
                memcpy(&stf->our_relay, &tmp6_addr, sizeof(struct in6_addr));
                found = 1;
                break;
            }
        }
    }
    
    if (!found) {
        my_log(LOG_DEBUG, "stf_resolve_callback: no v6 or v4 addresses found!!!");
    }
    else {
        if (!IN6_IS_ADDR_UNSPECIFIED(&stf->our_6to4_addr)) {
            service_publish_clear(service_p);
            (void)service_set_address(service_p, &stf->our_6to4_addr,
                stf->our_prefixLen);
        }
    }
    
    if (relay_addrs) {
        my_CFRelease(&relay_addrs);
    }
    SCNetworkReachabilityUnscheduleFromRunLoop(stf->target, 
                                                CFRunLoopGetCurrent(), 
                                                kCFRunLoopDefaultMode);
    my_CFRelease(&stf->target);

    return;
}

static void
stf_resolve_hostname(char * address, Service_t * service_p)
{
    Service_6to4_t *		stf = service_p->private;
    SCNetworkReachabilityRef	target = NULL;
    SCNetworkReachabilityContext context = { 0, NULL, NULL, NULL, NULL };
    
    if (address == NULL) {
        my_log(LOG_DEBUG, "stf_resolve_hostname: address == NULL");
        return;
    }
        
    context.info = service_p;
    
    target = SCNetworkReachabilityCreateWithName(NULL, address);
    
    if (target == NULL) {
        my_log(LOG_DEBUG, "stf_resolve_hostname: error creating target");
        return;
    }
    
    if (SCNetworkReachabilitySetCallback(target, stf_resolve_callback, &context)) {
        SCNetworkReachabilityScheduleWithRunLoop(target, 
                                                CFRunLoopGetCurrent(), 
                                                kCFRunLoopDefaultMode);
    }
    
    stf->target = target;
    
    return;
}

static void
stf_get_relay_address(Service_t * service_p, void * event_data)
{
    Service_6to4_t *		stf = (Service_6to4_t *)service_p->private;
    start_event_data_t *	evdata = ((start_event_data_t *)event_data);
    ip6config_method_data_t *	stf_config_data = evdata->config.data;
    relay_address_t *		relay_info = &stf_config_data->stf_data.relay_address;
    
    my_log(LOG_DEBUG, "stf_get_relay_address: relay address type is %s", 
            relay_address_type_string(relay_info->addr_type));
    
    switch(relay_info->addr_type) {
        case relay_address_type_ipv6_e: {
            memcpy(&stf->our_relay, &relay_info->relay_address_u.ip6_relay_addr, 
                    sizeof(struct in6_addr));
            break;
        }
        case relay_address_type_ipv4_e: {
            stf_construct_6to4_address(&relay_info->relay_address_u.ip4_relay_addr, 
                                    &stf->our_relay, TRUE);
            break;
        }
        case relay_address_type_dns_e: {
            my_log(LOG_DEBUG, "stf_get_relay_address: resolving hostname");
            stf_resolve_hostname(relay_info->relay_address_u.dns_relay_addr, 
                                    service_p);
            break;
        }
        default: {
            my_log(LOG_DEBUG, "stf_get_relay_address: no relay address type given");
            break;
        }
    }

    return;
}

__private_extern__ ip6config_status_t
stf_thread(Service_t * service_p, IFEventID_t evid, void * event_data)
{
    interface_t *	if_p = service_interface(service_p);
    Service_6to4_t *	stf = (Service_6to4_t *)service_p->private;
    ip6config_status_t	status = ip6config_status_success_e;
    
    switch (evid) {
        case IFEventID_start_e: {            
            if (if_flags(if_p) & IFF_LOOPBACK) {
                status = ip6config_status_invalid_operation_e;
                break;
            }

            my_log(LOG_DEBUG, "STF_THREAD %s: STARTING", if_name(if_p));
    
            if (stf) {
                my_log(LOG_DEBUG, "STF_THREAD %s: re-entering start state",
                        if_name(if_p));
                status = ip6config_status_internal_error_e;
                break;
            }
    
            stf = calloc(1, sizeof(*stf));
            if (stf == NULL) {
                my_log(LOG_ERR, "STF_THREAD %s: calloc failed",
                    if_name(if_p));
                status = ip6config_status_allocation_failed_e;
                break;
            }
    
            service_p->private = stf;
        
            stf_configure_address(service_p, event_data);
            stf_get_relay_address(service_p, event_data);
            
            if (!IN6_IS_ADDR_UNSPECIFIED(&stf->our_relay)) {
                (void)service_set_address(service_p, &stf->our_6to4_addr,
                                        stf->our_prefixLen);
            }
            
            break;
        }
        case IFEventID_stop_e: {
            /*	Remove address in kernel
             *	Remove route in kernel
             */
            
            my_log(LOG_DEBUG, "STF_THREAD %s: STOPPING", if_name(if_p));
            
            if (stf == NULL) {
                my_log(LOG_DEBUG, "STF_THREAD %s: private data is NULL",
                        if_name(if_p));
                status = ip6config_status_internal_error_e;
                break;
            }
            
            /* remove 6to4 address and route */
            service_remove_address(service_p);
            service_publish_clear(service_p);
            
            if (stf->target) {
                SCNetworkReachabilityUnscheduleFromRunLoop(stf->target, 
                                                            CFRunLoopGetCurrent(), 
                                                            kCFRunLoopDefaultMode);
                my_CFRelease(&stf->target);
            }
                
            free(stf);
            service_p->private = NULL;
            break;
        }
        case IFEventID_change_e: {
            /* Relay address changed */
            change_event_data_t *	evdata = ((change_event_data_t *)event_data);

            my_log(LOG_DEBUG, "STF_THREAD %s: CHANGE", if_name(if_p));
            
            if (stf == NULL) {
                my_log(LOG_DEBUG, "STF_THREAD %s: private data is NULL",
                        if_name(if_p));
                status = ip6config_status_internal_error_e;
                break;
            }
            
            if (stf->target) {
                SCNetworkReachabilityUnscheduleFromRunLoop(stf->target, 
                                                            CFRunLoopGetCurrent(), 
                                                            kCFRunLoopDefaultMode);
                my_CFRelease(&stf->target);
            }
            
            service_publish_clear(service_p);
            
            bzero(&stf->our_relay, sizeof(struct in6_addr));
            stf_get_relay_address(service_p, event_data);

            if (!IN6_IS_ADDR_UNSPECIFIED(&stf->our_relay)) {
                /* set new route */
                memcpy(&service_p->info.router, &stf->our_relay, sizeof(struct in6_addr));
                service_publish_success(service_p);
            }
            else {
                /* Remove existing address. It will be 
                 * added later if the hostname resolves.
                 */
                service_remove_address(service_p);
            }
            
            evdata->needs_stop = FALSE;
            break;
        }
        case IFEventID_state_change_e: {
            int	i;
            ip6_addrinfo_list_t * ip6_addrs = ((ip6_addrinfo_list_t *)event_data);

            my_log(LOG_DEBUG, "STF_THREAD %s: STATE_CHANGE", if_name(if_p));
            
            if (stf == NULL) {
                my_log(LOG_DEBUG, "STF_THREAD %s: private data is NULL",
                        if_name(if_p));
                status = ip6config_status_internal_error_e;
                break;
            }
                        
            /* go through the address list; if addr is not autoconf and
             * not linklocal then deal with it
             */
                
            for (i = 0; i < ip6_addrs->n_addrs; i++) {
                ip6_addrinfo_t	new_addr = ip6_addrs->addr_list[i];
                
                if (!IN6_IS_ADDR_LINKLOCAL(&new_addr.addr) 
                        && !(new_addr.flags & IN6_IFF_AUTOCONF)) {
                    
                    /* first copy the info into service_p */
                    memcpy(&service_p->info.addr, &new_addr.addr, 
                                sizeof(struct in6_addr));
                    service_p->info.prefixlen = new_addr.prefixlen;
                    service_p->info.addr_flags = new_addr.flags;

                    /* fill in new prefix and netaddr */
                    prefixLen2mask(&service_p->info.prefixmask, 
                                    service_p->info.prefixlen);
                    network_addr(&service_p->info.addr, 
                                    &service_p->info.prefixmask,
                                    &service_p->info.netaddr);

                    /* update private data */
                    memcpy(&stf->our_6to4_addr, &service_p->info.addr, 
                                sizeof(struct in6_addr));
                    stf->our_prefixLen = service_p->info.prefixlen;
                    stf->addr_flags = service_p->info.addr_flags;
                    memcpy(&stf->prefixmask, &service_p->info.prefixmask, 
                                sizeof(struct in6_addr));
                    memcpy(&stf->netaddr, &service_p->info.netaddr, 
                                sizeof(struct in6_addr));
                    
                    /* set route */
                    /* only publish if the router has been set */
                    if (!IN6_IS_ADDR_UNSPECIFIED(&stf->our_relay)) {
                        memcpy(&service_p->info.router, &stf->our_relay, sizeof(struct in6_addr));
                        service_publish_success(service_p);
                    }
                    
                    break;
                }
            }

            break;
        }
        case IFEventID_ipv4_primary_change_e: {
            struct in6_addr	old_addr;
            
            /*	Primary service has changed */
            my_log(LOG_DEBUG, "STF_THREAD: ipv4_primary_change");
            
            memcpy(&old_addr, &stf->our_6to4_addr, sizeof(struct in6_addr));
            bzero( &stf->our_6to4_addr,  sizeof(struct in6_addr));
            stf_configure_address(service_p, event_data);
            
            if (!IN6_ARE_ADDR_EQUAL(&old_addr, &stf->our_6to4_addr)) {
                /* remove 6to4 address */
                service_remove_address(service_p);
                service_publish_clear(service_p);
                
                /* if the 6to4 address is not specified then don't set it, and
                 * if the relay address is not set then either we're still
                 * waiting for it to resolve or it's not valid, so don't 
                 * set the address
                 */
                if (!IN6_IS_ADDR_UNSPECIFIED(&stf->our_6to4_addr) &&
                    !IN6_IS_ADDR_UNSPECIFIED(&stf->our_relay)) {
                    (void)service_set_address(service_p, &stf->our_6to4_addr,
                        stf->our_prefixLen);
                }
            }
            
            break;
        }
        default: {
            break;
        }
    } /* switch */

    return (status);
}

/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/* -----------------------------------------------------------------------------
 *
 *  Theory of operation :
 *
 *  plugin to add a generic socket support to pppd, instead of tty.
 *
----------------------------------------------------------------------------- */


/* -----------------------------------------------------------------------------
  Includes
----------------------------------------------------------------------------- */

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <netdb.h>
#include <utmp.h>
#include <pwd.h>
#include <setjmp.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <net/dlil.h>

#include <net/if.h>
#include <CoreFoundation/CFBundle.h>

#define APPLE 1

#include "../../../Family/ppp_defs.h"
#include "../../../Family/if_ppp.h"
#include "../../../Family/ppp_domain.h"
#include "../PPPoE-extension/PPPoE.h"
#include "../../../Helpers/pppd/pppd.h"
#include "../../../Helpers/pppd/fsm.h"
#include "../../../Helpers/pppd/lcp.h"


/* -----------------------------------------------------------------------------
 Definitions
----------------------------------------------------------------------------- */

#define MODE_CONNECT	"connect"
#define MODE_LISTEN	"listen"
#define MODE_ANSWER	"answer"

#define PPPOE_NKE	"PPPoE.kext"

// PPPoE error codes (bits 8..15 of last cause key)
#define EXIT_PPPoE_NOSERVER  		1
#define EXIT_PPPoE_NOSERVICE  		2
#define EXIT_PPPoE_NOAC 		3
#define EXIT_PPPoE_NOACSERVICE 		4
#define EXIT_PPPoE_CONNREFUSED 		5

/* -----------------------------------------------------------------------------
 Forward declarations
----------------------------------------------------------------------------- */
void pppoe_process_extra_options();
void pppoe_check_options();
int pppoe_connect(int *errorcode);
void pppoe_disconnect();
void pppoe_close();
void pppoe_cleanup();
int pppoe_establish_ppp(int);
void pppoe_wait_input();
void pppoe_disestablish_ppp(int);
void pppoe_link_down(void *arg, int p);

static int pppoe_dial();
static int pppoe_listen();
static void closeall();
static u_long load_kext(char *kext);

/* -----------------------------------------------------------------------------
 PPP globals
----------------------------------------------------------------------------- */


static int 	sockfd = -1;			/* socket file descriptor */
static CFBundleRef 	bundle = 0;		/* our bundle ref */

/* option variables */
static char 	*mode = MODE_CONNECT;		/* connect mode by default */
static bool 	loopback = 0;			/* loop back mode */
static bool 	noload = 0;			/* don't load the kernel extension */
static char	*service = NULL; 		/* service selection to use */
static char	*access_concentrator = NULL; 	/* access concentrator to connect to */
static int	retrytimer = 0; 		/* retry timer (default is 3 seconds) */
static int	connecttimer = 65; 		/* bump the connection timer from 20 to 65 seconds */
static bool	linkdown = 0; 			/* flag set when we receive link down event */

extern int kill_link;

/* option descriptors */
option_t pppoe_options[] = {
    { "pppoeservicename", o_string, &service,
      "PPPoE service to choose" },
    { "pppoeacname", o_string, &access_concentrator,
      "PPPoE Access Concentrator to connect to" },
    { "pppoeloopback", o_bool, &loopback,
      "Configure PPPoE in loopback mode, for single machine testing", 1 },
    { "nopppoeload", o_bool, &noload,
      "Don't try to load the PPPoE kernel extension", 1 },
    { "pppoemode", o_string, &mode,
      "Configure configuration mode [connect, listen, answer]" },
    { "pppoeconnecttimer", o_int, &connecttimer,
      "Connect timer for outgoing call (default 65 seconds)" },
    { "pppoeretrytimer", o_int, &retrytimer,
      "Retry timer for outgoing call (default 3 seconds)" },
    { NULL }
};


/* -----------------------------------------------------------------------------
plugin entry point, called by pppd
----------------------------------------------------------------------------- */
int start(CFBundleRef ref)
{
 
    bundle = ref;
    CFRetain(bundle);
        
    // hookup our socket handlers
    bzero(the_channel, sizeof(struct channel));
    the_channel->options = pppoe_options;
    the_channel->process_extra_options = pppoe_process_extra_options;
    the_channel->wait_input = pppoe_wait_input;
    the_channel->check_options = pppoe_check_options;
    the_channel->connect = pppoe_connect;
    the_channel->disconnect = pppoe_disconnect;
    the_channel->cleanup = pppoe_cleanup;
    the_channel->close = pppoe_close;
    the_channel->establish_ppp = pppoe_establish_ppp;
    the_channel->disestablish_ppp = pppoe_disestablish_ppp;
    // use the default config functions
    the_channel->send_config = generic_send_config;
    the_channel->recv_config = generic_recv_config;
    
    add_notifier(&link_down_notifier, pppoe_link_down, 0);
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void pppoe_link_down(void *arg, int p)
{
    linkdown = 1;
}

/* ----------------------------------------------------------------------------- 
work out which device we are using and read its options file
----------------------------------------------------------------------------- */
void pppoe_process_extra_options()
{
    int 		error = EXIT_OPTION_ERROR;
    int 		len, s;
    struct ifreq 	ifr;

    if (!device)
        device = "en0";
        
    s = socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT);
    if (s >= 0) {
        
        len = strlen(device);
        if (len <= sizeof(ifr.ifr_name)) {

            bzero(&ifr, sizeof(ifr));
            bcopy(device, ifr.ifr_name, len);
            if (ioctl(s, SIOCGIFFLAGS, (caddr_t) &ifr) >= 0) {
                // ensure that the device is UP
                ifr.ifr_flags |= IFF_UP;
                if (ioctl(s, SIOCSIFFLAGS, (caddr_t) &ifr) >= 0)
                    error = 0;
            }
        }
        
        close(s);
    }
        
    if (!strcmp(mode, MODE_ANSWER)) {
        // make sure we get a file descriptor > 2 so that pppd can detach and close 0,1,2
        sockfd = dup(0);
    }

    if (error)
        exit(error);
}

/* ----------------------------------------------------------------------------- 
do consistency checks on the options we were given
----------------------------------------------------------------------------- */
void pppoe_check_options()
{
}

/* ----------------------------------------------------------------------------- 
called back everytime we go out of select, and data needs to be read
the hook is called and has a chance to get data out of its file descriptor
in the case of PPPoE, we are not supposed to get data on the socket
if our socket gets awaken, that's because is has been closed
----------------------------------------------------------------------------- */
void pppoe_wait_input()
{
   
    if (sockfd != -1 && is_ready_fd(sockfd)) {
        // looks like we have been disconnected...
        // the status is updated only if link is not already down
        if (linkdown == 0) {
            notice("PPPoE hangup");
            status = EXIT_HANGUP;
        }
        remove_fd(sockfd);
        hungup = 1;
        lcp_lowerdown(0);	/* PPPoE link is no longer available */
        link_terminated(0);
    }
}

/* ----------------------------------------------------------------------------- 
get the socket ready to start doing PPP.
That is, open the socket and run the connector
----------------------------------------------------------------------------- */
int pppoe_connect(int *errorcode)
{
    char 	dev[32], name[MAXPATHLEN]; 
    int 	err = 0;  
    CFURLRef	url;
    
    sprintf(dev, "socket[%d:%d]", PF_PPP, PPPPROTO_PPPOE);
    strlcpy(ppp_devnam, dev, sizeof(ppp_devnam));

    hungup = 0;
    kill_link = 0;
    linkdown = 0;

    if (strcmp(mode, MODE_ANSWER)) {
        /* open the socket */
        sockfd = socket(PF_PPP, SOCK_DGRAM, PPPPROTO_PPPOE);
        if (sockfd < 0) {
            if (!noload) {
                if (url = CFBundleCopyBundleURL(bundle)) {
                    name[0] = 0;
                    CFURLGetFileSystemRepresentation(url, 0, name, MAXPATHLEN - 1);
                    CFRelease(url);
                    strcat(name, "/");
                    if (url = CFBundleCopyBuiltInPlugInsURL(bundle)) {
                        CFURLGetFileSystemRepresentation(url, 0, name + strlen(name), 
                            MAXPATHLEN - strlen(name) - strlen(PPPOE_NKE) - 1);
                        CFRelease(url);
                        strcat(name, "/");
                        strcat(name, PPPOE_NKE);
                        if (!load_kext(name))
                            sockfd = socket(PF_PPP, SOCK_DGRAM, PPPPROTO_PPPOE);
                    }	
                }
            }
            if (sockfd < 0) {
                error("Failed to open PPPoE socket: %m");
                status = EXIT_OPEN_FAILED;
                return -1;
            }
        }
    }

    if (loopback || debug) {
        u_int32_t 	flags;
        flags = (loopback ? PPPOE_FLAG_LOOPBACK : 0)
            + (debug ? PPPOE_FLAG_DEBUG : 0);
        if (setsockopt(sockfd, PPPPROTO_PPPOE, PPPOE_OPT_FLAGS, &flags, 4)) {
            error("PPPoE can't set PPPoE flags...\n");
            return errno;
        }
        if (loopback) 
            info("PPPoE loopback activated...\n");
    }

    if (connecttimer) {
        u_int16_t 	timer = connecttimer;
        if (setsockopt(sockfd, PPPPROTO_PPPOE, PPPOE_OPT_CONNECT_TIMER, &timer, 2)) {
            error("PPPoE can't set PPPoE connect timer...\n");
            return errno;
        }
    }

    if (retrytimer) {
        u_int16_t 	timer = retrytimer;
        if (setsockopt(sockfd, PPPPROTO_PPPOE, PPPOE_OPT_RETRY_TIMER, &timer, 2)) {
            error("PPPoE can't set PPPoE retry timer...\n");
            return errno;
        }
    }

    if (setsockopt(sockfd, PPPPROTO_PPPOE, PPPOE_OPT_INTERFACE, device, strlen(device))) {
        error("PPPoE can't specify interface...\n");
        return errno;
    }

    if (!strcmp(mode, MODE_ANSWER)) {
        // nothing to do
    }
    else if (!strcmp(mode, MODE_LISTEN)) {
        err = pppoe_listen();
    }
    else if (!strcmp(mode, MODE_CONNECT)) {
        err = pppoe_dial();
    }
    else 
        fatal("PPPoE incorrect mode : '%s'", mode ? mode : "");

    if (err) {
        if (err != -2) {
            if (err != -1)
                devstatus = err;
            status = EXIT_CONNECT_FAILED;
        }
        return -1;
    }
    
    return sockfd;
}

/* ----------------------------------------------------------------------------- 
run the disconnector connector
----------------------------------------------------------------------------- */
void pppoe_disconnect()
{
    info("PPPoE disconnecting...\n");
    
    if (shutdown(sockfd, SHUT_RDWR) < 0) {
        error("PPPoE disconnection failed, error = %d.\n", errno);
        return;
    }

    info("PPPoE disconnected\n");
}

/* ----------------------------------------------------------------------------- 
close the socket descriptors
----------------------------------------------------------------------------- */
void pppoe_close()
{
	if (sockfd >= 0) {
		close(sockfd);
		sockfd = -1;
	}
}

/* ----------------------------------------------------------------------------- 
clean up before quitting
----------------------------------------------------------------------------- */
void pppoe_cleanup()
{
    pppoe_close();
}

/* ----------------------------------------------------------------------------- 
establish the socket as a ppp link
----------------------------------------------------------------------------- */
int pppoe_establish_ppp(int fd)
{
    int x, new_fd;
    
    if (ioctl(fd, PPPIOCATTACH, &x) < 0) {
        error("Couldn't attach socket to the link layer: %m");
        return -1;
    }

    new_fd = generic_establish_ppp(fd);
    if (new_fd == -1)
        return -1;

    /* add our pppoe socket to the select */
    add_fd(fd);
    
    return new_fd;
}

/* ----------------------------------------------------------------------------- 
disestablish the socket as a ppp link
----------------------------------------------------------------------------- */
void pppoe_disestablish_ppp(int fd)
{
    int 	x;
    
    remove_fd(fd);

    if (ioctl(fd, PPPIOCDETACH, &x) < 0)
        error("Couldn't detach socket from link layer: %m");

    generic_disestablish_ppp(fd);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pppoe_dial()
{
    struct sockaddr_pppoe 	addr;
    u_short 			len, i;
    
    // if the specific pppoe option are not used, try to see 
    // if the remote address generic field can be decomposed
    // in the form of service_name\access_concentrator
    if (!service && !access_concentrator && remoteaddress) {
	len = strlen(remoteaddress);
	for (i = 0; i < len; i++) 
            if (remoteaddress[i] == '\\')
                break;
        if (service = malloc(i + 1)) {
            strncpy(service, remoteaddress, i);
            service[i] = 0;
        }
        if (i < len) {
            if (access_concentrator = malloc(len - i))
                strcpy(access_concentrator, &remoteaddress[i + 1]);
        }
    }
    
    info("PPPoE connecting to service '%s' [access concentrator '%s']...\n", 
            service ? service : "", 
            access_concentrator ? access_concentrator : "");

    bzero(&addr, sizeof(addr));
    addr.ppp.ppp_len = sizeof(struct sockaddr_pppoe);
    addr.ppp.ppp_family = AF_PPP;
    addr.ppp.ppp_proto = PPPPROTO_PPPOE;
    if (access_concentrator)
        strncpy(addr.pppoe_ac_name, access_concentrator, sizeof(addr.pppoe_ac_name));
    if (service)
        strncpy(addr.pppoe_service, service, sizeof(addr.pppoe_service));
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_pppoe)) < 0) {
        if (errno == EINTR)
            return -2; // user cancelled
        error("PPPoE connection failed, %m");
        switch (errno) {
            case EHOSTUNREACH:
                if ((service && service[0]) && (access_concentrator && access_concentrator[0]))
                    return EXIT_PPPoE_NOACSERVICE;
                else if (service && service[0])
                    return EXIT_PPPoE_NOSERVICE;
                else if (access_concentrator && access_concentrator[0])
                    return EXIT_PPPoE_NOAC;
                return EXIT_PPPoE_NOSERVER;
                
            case ECONNREFUSED:
                return EXIT_PPPoE_CONNREFUSED;

            case ENXIO:
                // Ethernet interface is detached
                // fake a cancel to get a consistent
                // error message with the HANGUP case
                status = EXIT_HANGUP;
                return -2; 

        }
        return -1;
    }

    info("PPPoE connection established.");
    return 0;
}


/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pppoe_listen()
{
    struct sockaddr_pppoe 	addr;
    int				len, fd;

    info("PPPoE listening on service '%s' [access concentrator '%s']...\n", 
            service ? service : "", 
            access_concentrator ? access_concentrator : "");

    bzero(&addr, sizeof(addr));
    addr.ppp.ppp_len = sizeof(struct sockaddr_pppoe);
    addr.ppp.ppp_family = AF_PPP;
    addr.ppp.ppp_proto = PPPPROTO_PPPOE;
    if (access_concentrator)
        strncpy(addr.pppoe_ac_name, access_concentrator, sizeof(addr.pppoe_ac_name));
    if (service)
        strncpy(addr.pppoe_service, service, sizeof(addr.pppoe_service));
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_pppoe)) < 0) {
        error("PPPoE bind failed, %m");
        return -1;
    }

    if (listen(sockfd, 10) < 0) {
        printf("PPPoE listen failed, %m");
        return -1;
    }

    len = sizeof(addr);
    fd = accept(sockfd, (struct sockaddr *) &addr, &len);
    if (fd < 0) {
        error("PPPoE accept failed, %m");
        return -1;
    }
    
    close(sockfd);	// close the socket used for listening
    sockfd = fd;	// use the accepted socket instead of
    
    info("PPPoE connection established in incoming call.");
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void closeall()
{
    int i;

    for (i = getdtablesize() - 1; i >= 0; i--) close(i);
    open("/dev/null", O_RDWR, 0);
    dup(0);
    dup(0);
    return;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_long load_kext(char *kext)
{
    int pid;

    if ((pid = fork()) < 0)
        return 1;

    if (pid == 0) {
        closeall();
        // PPP kernel extension not loaded, try load it...
        execl("/sbin/kextload", "kextload", kext, (char *)0);
        exit(1);
    }

    while (waitpid(pid, 0, 0) < 0) {
        if (errno == EINTR)
            continue;
       return 1;
    }
    return 0;
}


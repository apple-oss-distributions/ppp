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
includes
----------------------------------------------------------------------------- */
#include <string.h>
#include <stdio.h>
#include <sys/errno.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/ucred.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <SystemConfiguration/SCPrivate.h>      // for SCLog()

#include "ppp_msg.h"
#include "ppp_privmsg.h"
#include "ppp_client.h"
#include "ppp_manager.h"
#include "ppp_option.h"
#include "ppp_socket_server.h"
#include "ppp_utils.h"

/* -----------------------------------------------------------------------------
definitions
----------------------------------------------------------------------------- */

enum {
    do_nothing = 0,
    do_process,
    do_close,
    do_error
};


/* -----------------------------------------------------------------------------
forward declarations
----------------------------------------------------------------------------- */

static void socket_status (struct client *client, struct msg *msg, void **reply);
static void socket_extendedstatus (struct client *client, struct msg *msg, void **reply);
static void socket_connect (struct client *client, struct msg *msg, void **reply);
static void socket_disconnect (struct client *client, struct msg *msg, void **reply);
static void socket_suspend (struct client *client, struct msg *msg, void **reply);
static void socket_resume (struct client *client, struct msg *msg, void **reply);
static void socket_getconnectdata (struct client *client, struct msg *msg, void **reply);
static void socket_enable_event (struct client *client, struct msg *msg, void **reply);
static void socket_disable_event (struct client *client, struct msg *msg, void **reply);
static void socket_version (struct client *client, struct msg *msg, void **reply);
static void socket_getnblinks (struct client *client, struct msg *msg, void **reply);
static void socket_getlinkbyindex (struct client *client, struct msg *msg, void **reply);
static void socket_getlinkbyserviceid (struct client *client, struct msg *msg, void **reply);
static void socket_getlinkbyifname (struct client *client, struct msg *msg, void **reply);
static void socket_setoption (struct client *client, struct msg *msg, void **reply);
static void socket_getoption (struct client *client, struct msg *msg, void **reply);

static void socket_pppd_event(struct client *client, struct msg *msg);
static void socket_pppd_status(struct client *client, struct msg *msg);
static void socket_pppd_phase(struct client *client, struct msg *msg);



static void processRequest (struct client *client, struct msg *msg);

static void listenCallBack(CFSocketRef s, CFSocketCallBackType type,
                     CFDataRef address, const void *data, void *info);
static void clientCallBack(CFSocketRef s, CFSocketCallBackType type,
                     CFDataRef address, const void *data, void *info);



/* -----------------------------------------------------------------------------
globals
----------------------------------------------------------------------------- */

extern TAILQ_HEAD(, ppp) 	ppp_head;

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int ppp_socket_start_server ()
{
    struct sockaddr_un	addr;
    int			error, s;
    mode_t		mask;
    CFSocketRef		ref = 0;
    CFRunLoopSourceRef	rls;
    CFSocketContext	context = { 0, NULL, NULL, NULL, NULL };

    if ((s = socket(AF_LOCAL, SOCK_STREAM, 0)) == -1)
        goto fail;

    unlink(PPP_PATH);
    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_LOCAL;
    strcpy(addr.sun_path, PPP_PATH);
    mask = umask(0);
    error = bind(s, (struct sockaddr *)&addr, SUN_LEN(&addr));
    umask(mask);
    if (error) 
        goto fail;

    if ((ref = CFSocketCreateWithNative(NULL, s, kCFSocketReadCallBack,
                                   listenCallBack, &context)) == 0)
        goto fail;
    
    if ((rls = CFSocketCreateRunLoopSource(NULL, ref, 0)) == 0)
        goto fail;
           
    CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
    CFRelease(rls);

    listen(s, SOMAXCONN);
    CFRelease(ref);
    return 0;
    
fail:
    SCLog(TRUE, LOG_INFO, CFSTR("PPPController: initialization failed...\n"));
    if (s != -1) 
        close(s);
    if (ref) {
        CFSocketInvalidate(ref);
        CFRelease(ref);
    }
    return 1;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int ppp_socket_create_client(int s, int priviledged, uid_t uid, gid_t gid)
{
    int			flags;
    CFSocketRef		ref;
    CFRunLoopSourceRef	rls;
    CFSocketContext	context = { 0, NULL, NULL, NULL, NULL };
        
    if ((flags = fcntl(s, F_GETFL)) == -1
	|| fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
        SCLog(TRUE, LOG_INFO, CFSTR("Couldn't set client socket in non-blocking mode, errno = %d\n"), errno);
    }
        
    if ((ref = CFSocketCreateWithNative(NULL, s, 
                    kCFSocketReadCallBack, clientCallBack, &context)) == 0) {
        close(s);
        return -1;
    }
    if ((rls = CFSocketCreateRunLoopSource(NULL, ref, 0)) == 0)
        goto fail;

    CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
    CFRelease(rls);

    if (client_new_socket(ref, priviledged, uid, gid) == 0)
        goto fail;

    CFRelease(ref);
    return 0;
    
fail:
    CFSocketInvalidate(ref);
    CFRelease(ref);
    return -1;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
void listenCallBack(CFSocketRef inref, CFSocketCallBackType type,
                     CFDataRef address, const void *data, void *info)
{
    struct sockaddr_un	addr;
    int			s, len;
	struct xucred xucred;

    len = sizeof(addr);
    if ((s = accept(CFSocketGetNative(inref), (struct sockaddr *) &addr, &len)) == -1)
        return;
        
    PRINTF(("Accepted connection...\n"));

	len = sizeof(xucred);
	if (getsockopt(s, 0, LOCAL_PEERCRED, &xucred, &len) == -1) {
        SCLog(TRUE, LOG_ERR, CFSTR("PPPController: can't get LOCAL_PEERCRED, errno = %d\n"), errno);
		return ;
	}
	
	ppp_socket_create_client(s, 0, 0 /*xucred.cr_uid*/, 0 /*xucred.cr_gid*/);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
int readn(int ref, void *data, int len)
{
    int 	n, left = len;
    void 	*p = data;
    
    while (left > 0) {
        if ((n = read(ref, p, left)) < 0) {
            if (errno == EWOULDBLOCK) 
                return (len - left);
            if (errno != EINTR) 
                return -1;
            n = 0;
        }
        else if (n == 0)
            return -1; /* EOF */
            
        left -= n;
        p += n;
    }
    return (len - left);
}        

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
//static 
int writen(int ref, void *data, int len)
{	
    int 	n, left = len;
    void 	*p = data;
    
    while (left > 0) {
        if ((n = write(ref, p, left)) <= 0) {
            if (errno != EINTR) 
                return -1;
            n = 0;
        }
        left -= n;
        p += n;
    }
    return len;
}        

typedef void (*msg_function)(struct client *client, struct msg *msg, void **reply);

msg_function requests[] = {
    NULL,			/* */
    socket_version, 		/* PPP_VERSION */
    socket_status, 		/* PPP_STATUS */
    socket_connect, 		/* PPP_CONNECT */
    NULL,			/* */
    socket_disconnect, 		/* PPP_DISCONNECT */
    socket_getoption, 		/* PPP_GETOPTION */
    socket_setoption, 		/* PPP_SETOPTION */
    socket_enable_event, 		/* PPP_ENABLE_EVENT */
    socket_disable_event,		/* PPP_DISABLE_EVENT */
    NULL,	 		/* PPP_EVENT */
    socket_getnblinks, 		/* PPP_GETNBLINKS */
    socket_getlinkbyindex, 	/* PPP_GETLINKBYINDEX */
    socket_getlinkbyserviceid, 	/* PPP_GETLINKBYSERVICEID */
    socket_getlinkbyifname, 	/* PPP_GETLINKBYIFNAME */
    socket_suspend, 		/* PPP_SUSPEND */
    socket_resume, 		/* PPP_RESUME */
    socket_extendedstatus, 	/* PPP_EXTENDEDSTATUS */
    socket_getconnectdata 		/* PPP_GETCONNECTDATA */
};
#define LAST_REQUEST PPP_GETCONNECTDATA

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
void clientCallBack(CFSocketRef inref, CFSocketCallBackType type,
                     CFDataRef address, const void *data, void *info)
{
    int 		s = CFSocketGetNative(inref);
    int			action = do_nothing;
    ssize_t		n;
    struct client 	*client;

    client = client_findbysocketref(inref);
    if (client == 0)
        return;
            
    /* first read the header part of the message */
    if (client->msglen < sizeof(struct ppp_msg_hdr)) {
        n = readn(s, &((u_int8_t *)&client->msghdr)[client->msglen], sizeof(struct ppp_msg_hdr) - client->msglen);
        switch (n) {
            case -1:
                action = do_close;
                break;
            default:
                client->msglen += n;
                if (client->msglen == sizeof(struct ppp_msg_hdr)) {
				
					/* check if message bytes are in network order */
					if (!(client->flags & CLIENT_FLAG_PRIVILEDGED) && (client->msghdr.m_type > LAST_REQUEST)) {
						client->flags |= CLIENT_FLAG_SWAP_BYTES;
						client->msghdr.m_flags = htons(client->msghdr.m_flags);
						client->msghdr.m_type = htons(client->msghdr.m_type);
						client->msghdr.m_result = htonl(client->msghdr.m_result);
						client->msghdr.m_cookie = htonl(client->msghdr.m_cookie);
						client->msghdr.m_link = htonl(client->msghdr.m_link);
						client->msghdr.m_len = htonl(client->msghdr.m_len);
					}
					else 
						client->flags &= ~CLIENT_FLAG_SWAP_BYTES;

                    client->msgtotallen = client->msglen
                        + client->msghdr.m_len
                        + (client->msghdr.m_flags & USE_SERVICEID ? client->msghdr.m_link : 0);
                    client->msg = my_Allocate(client->msgtotallen + 1);
                    if (client->msg == 0)
                        action = do_error;
                    else {
                        bcopy(&client->msghdr, client->msg, sizeof(struct ppp_msg_hdr));
                        // let's end the message with a null byte
                        client->msg[client->msgtotallen] = 0;
                    }
                }
        }
    }
     
    /* first read the data part of the message, including serviceid */
    if (client->msglen >= sizeof(struct ppp_msg_hdr)) {
        n = readn(s, &client->msg[client->msglen], client->msgtotallen - client->msglen);
        switch (n) {
            case -1:
                action = do_close;
                break;
            default:
                client->msglen += n;
                if (client->msglen == client->msgtotallen) {
                    action = do_process;
                }
        }
    }

    /* perform action */
    switch (action) {
        case do_nothing:
            break;
        case do_error:
        case do_close:
            PRINTF(("Connection closed...\n"));
            /* connection closed by client */
            client_dispose(client);
            break;

        case do_process:
            // process client request
            processRequest(client, (struct msg *)client->msg);
            my_Deallocate(client->msg, client->msgtotallen + 1);
            client->msg = 0;
            client->msglen = 0;
            client->msgtotallen = 0;
            break;
    }
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
void processRequest (struct client *client, struct msg *msg)
{
    void		*reply = 0;
    msg_function	func; 
	struct ppp_msg_hdr	hdr;
    
    PRINTF(("process_request : type = %x, len = %d\n", msg->hdr.m_type, msg->hdr.m_len));
    //printf("process_request : type = %x, len = %d\n", msg->hdr.m_type, msg->hdr.m_len);

    if (msg->hdr.m_type <= LAST_REQUEST) {
        
        func = requests[msg->hdr.m_type];
        if (func)
            (*func)(client, msg, &reply);
    }
    else {
        // check if it belongs to a controlling service
        if (client->flags & CLIENT_FLAG_PRIVILEDGED) {
            switch (msg->hdr.m_type) { 
                // private pppd event
                case PPPD_EVENT:
                    socket_pppd_event(client, msg);
                    break;
                case PPPD_STATUS:
                    socket_pppd_status(client, msg);
                    break;
                case PPPD_PHASE:
                    socket_pppd_phase(client, msg);
                    break;
            }
        }
    }

	/* save header before swapping bytes */
	bcopy(msg, &hdr, sizeof(hdr));
	
	/* swap back bytes in the message header */
	if (client->flags & CLIENT_FLAG_SWAP_BYTES) {
		msg->hdr.m_flags = htons(msg->hdr.m_flags);
		msg->hdr.m_type = htons(msg->hdr.m_type);
		msg->hdr.m_result = htonl(msg->hdr.m_result);
		msg->hdr.m_cookie = htonl(msg->hdr.m_cookie);
		msg->hdr.m_link = htonl(msg->hdr.m_link);
		msg->hdr.m_len = htonl(msg->hdr.m_len);
	}

    if (hdr.m_len != 0xFFFFFFFF) {

        writen(CFSocketGetNative(client->socketRef), msg, sizeof(struct ppp_msg_hdr) + 
            (hdr.m_flags & USE_SERVICEID ? hdr.m_link : 0));

       if (hdr.m_len) {
            writen(CFSocketGetNative(client->socketRef), reply, hdr.m_len);
            my_Deallocate(reply, hdr.m_len);
        }
        PRINTF(("process_request : m_type = 0x%x, result = 0x%x, cookie = 0x%x, link = 0x%x, len = 0x%x\n",
                hdr.m_type, hdr.m_result, hdr.m_cookie, hdr.m_link, hdr.m_len));
#if 0
        if (hdr.m_type == PPP_STATUS) {
            struct ppp_status *stat = (struct ppp_status *)&msg->data[0];
            PRINTF(("     ----- status = 0x%x", stat->status));
            if (stat->status != PPP_RUNNING) {
                PRINTF((", cause = 0x%x", stat->s.disc.lastDiscCause));
            }
            PRINTF(("\n"));
        }
#endif
    }
}

/* -----------------------------------------------------------------------------
find the ppp structure corresponding to the message
----------------------------------------------------------------------------- */
struct ppp *ppp_find(struct msg *msg)
{

    if (msg->hdr.m_flags & USE_SERVICEID)
        return ppp_findbysid(msg->data, msg->hdr.m_link);
    else
        return ppp_findbyref(msg->hdr.m_link);

    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_status(struct client *client, struct msg *msg, void **reply)
{
    struct ppp			*ppp = ppp_find(msg);
    int					err;
	u_int16_t			replylen;

    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }

	err = ppp_getstatus(ppp, reply, &replylen);
	if (err) {
		msg->hdr.m_result = err;
		msg->hdr.m_len = 0;
        return;
	}
	
	if (client->flags & CLIENT_FLAG_SWAP_BYTES) {
		struct ppp_status *stat = (struct ppp_status *)*reply;
		stat->status = htonl(stat->status);
		stat->s.run.timeElapsed = htonl(stat->s.run.timeElapsed);
		stat->s.run.timeRemaining = htonl(stat->s.run.timeRemaining);
		stat->s.run.inBytes = htonl(stat->s.run.inBytes);
		stat->s.run.inPackets = htonl(stat->s.run.inPackets);
		stat->s.run.inErrors = htonl(stat->s.run.inErrors);
		stat->s.run.outBytes = htonl(stat->s.run.outBytes);
		stat->s.run.outPackets = htonl(stat->s.run.outPackets);
		stat->s.run.outErrors = htonl(stat->s.run.outErrors);
	}

    msg->hdr.m_result = 0;
    msg->hdr.m_len = replylen;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_extendedstatus(struct client *client, struct msg *msg, void **reply)
{
    struct ppp			*ppp = ppp_find(msg);
    u_int16_t			replylen = 0;
	int					err;
    
    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }

	err = ppp_copyextendedstatus(ppp, reply, &replylen);
	if (err) {
		msg->hdr.m_result = err;
		msg->hdr.m_len = 0;
        return;
	}

    msg->hdr.m_result = 0;
    msg->hdr.m_len = replylen;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_connect(struct client *client, struct msg *msg, void **reply)
{
    void 		*data = (struct ppp_status *)&msg->data[MSG_DATAOFF(msg)];
    struct ppp		*ppp = ppp_find(msg);
    CFDictionaryRef	opts = 0;

    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }
        
    if (msg->hdr.m_len == 0) {
        // first find current the appropriate set of options
        opts = client_findoptset(client, ppp->serviceID);
    }
    else {
        opts = (CFDictionaryRef)Unserialize(data, msg->hdr.m_len);
        if (opts == 0 || CFGetTypeID(opts) != CFDictionaryGetTypeID()) {
            msg->hdr.m_result = ENOMEM;
            msg->hdr.m_len = 0;
            if (opts) 
                CFRelease(opts);
            return;
        }
    }
    
    msg->hdr.m_result = ppp_connect(ppp, opts, 0, 
        (msg->hdr.m_flags & CONNECT_ARBITRATED_FLAG) ? client : 0,  
        (msg->hdr.m_flags & CONNECT_AUTOCLOSE_FLAG) ? 1 : 0, client->uid, client->gid, 0);
    if (opts && msg->hdr.m_len) 
        CFRelease(opts);
    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_disconnect(struct client *client, struct msg *msg, void **reply)
{
    struct ppp		*ppp = ppp_find(msg);
    
    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }
    
    ppp_disconnect(ppp, (msg->hdr.m_flags & DISCONNECT_ARBITRATED_FLAG) ? client : 0, SIGHUP);

    msg->hdr.m_result = 0;
    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_suspend(struct client *client, struct msg *msg, void **reply)
{
    struct ppp		*ppp = ppp_find(msg);
    
    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }
    
    ppp_suspend(ppp); 

    msg->hdr.m_result = 0;
    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_resume(struct client *client, struct msg *msg, void **reply)
{
    struct ppp		*ppp = ppp_find(msg);
    
    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }
    
    ppp_resume(ppp);

    msg->hdr.m_result = 0;
    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_getconnectdata(struct client *client, struct msg *msg, void **reply)
{
    struct ppp			*ppp = ppp_find(msg);
    int					err;
	u_int16_t			replylen;

    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }

	err = ppp_getconnectdata(ppp, reply, &replylen, 0);
	if (err) {
		msg->hdr.m_result = err;
		msg->hdr.m_len = 0;
        return;
	}

    msg->hdr.m_result = 0;
    msg->hdr.m_len = replylen;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_enable_event(struct client *client, struct msg *msg, void **reply)
{
    u_int32_t	notification = 1; // type of notification, event or status
    
    if (msg->hdr.m_len == 4) {
        notification = *(u_int32_t *)&msg->data[MSG_DATAOFF(msg)];
		if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
			notification = htonl(notification);
        if (notification < 1 || notification > 3) {
            msg->hdr.m_result = EINVAL;
            msg->hdr.m_len = 0;
            return;
        }
    }

    msg->hdr.m_result = 0;
	client->flags &= ~(CLIENT_FLAG_NOTIFY_EVENT + CLIENT_FLAG_NOTIFY_STATUS);
	if (notification & 1)
		client->flags |= CLIENT_FLAG_NOTIFY_EVENT;
	if (notification & 2)
		client->flags |= CLIENT_FLAG_NOTIFY_STATUS;
    client->notify_link = 0;
	if (client->notify_serviceid) {
		free(client->notify_serviceid);
		client->notify_serviceid = 0;
	}
    if (msg->hdr.m_flags & USE_SERVICEID) {        
        if (client->notify_serviceid = malloc(msg->hdr.m_link + 1)) {
            strncpy(client->notify_serviceid, msg->data, msg->hdr.m_link);
            client->notify_serviceid[msg->hdr.m_link] = 0;
        }
        else 
            msg->hdr.m_result = ENOMEM;
    }
    else 
        client->notify_link = msg->hdr.m_link;

    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_disable_event(struct client *client, struct msg *msg, void **reply)
{
    u_int32_t	notification = 1; // type of notification, event or status
    
    if (msg->hdr.m_len == 4) {
        notification = *(u_int32_t *)&msg->data[MSG_DATAOFF(msg)];
		if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
			notification = htonl(notification);
        if (notification < 1 || notification > 3) {
            msg->hdr.m_result = EINVAL;
            msg->hdr.m_len = 0;
            return;
        }
    }

	if (notification & 1)
		client->flags &= ~CLIENT_FLAG_NOTIFY_EVENT;
	if (notification & 2)
		client->flags &= ~CLIENT_FLAG_NOTIFY_STATUS;

    if ((client->flags & (CLIENT_FLAG_NOTIFY_EVENT + CLIENT_FLAG_NOTIFY_EVENT)) == 0) {
        client->notify_link = 0;    
        if (client->notify_serviceid) {
            free(client->notify_serviceid);
            client->notify_serviceid = 0;
        }
    }
    msg->hdr.m_result = 0;
    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_version(struct client *client, struct msg *msg, void **reply)
{

    *reply = my_Allocate(sizeof(u_int32_t));
    if (*reply == 0) {
        msg->hdr.m_result = ENOMEM;
        msg->hdr.m_len = 0;
    }
    else {
        msg->hdr.m_result = 0;
        msg->hdr.m_len = sizeof(u_int32_t);
        *(u_int32_t*)*reply = CURRENT_VERSION;
		if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
			*(u_int32_t*)*reply = htonl(*(u_int32_t*)*reply);
    }
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_getnblinks(struct client *client, struct msg *msg, void **reply)
{
    u_long		nb = 0;
    struct ppp		*ppp;
    u_short		subtype = msg->hdr.m_link >> 16;

    TAILQ_FOREACH(ppp, &ppp_head, next) {
        if ((subtype == 0xFFFF)
            || ( subtype == ppp->subtype)) {
            nb++;
        }
    }

    *reply = my_Allocate(sizeof(u_int32_t));
    if (*reply == 0) {
        msg->hdr.m_result = ENOMEM;
        msg->hdr.m_len = 0;
    }
    else {
        msg->hdr.m_result = 0;
        msg->hdr.m_len = sizeof(u_int32_t);
        *(u_int32_t*)*reply = nb;
		if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
			*(u_int32_t*)*reply = htonl(*(u_int32_t*)*reply);
    }
}

/* -----------------------------------------------------------------------------
index is a global index across all the link types (or within the family)
index if between 0 and nblinks
----------------------------------------------------------------------------- */
static
void socket_getlinkbyindex(struct client *client, struct msg *msg, void **reply)
{
    u_long		nb = 0, len = 0, err = ENODEV, index;
    struct ppp		*ppp;
    u_short		subtype = msg->hdr.m_link >> 16;

    index = *(u_long *)&msg->data[0];
	if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
		index = htonl(index);

    TAILQ_FOREACH(ppp, &ppp_head, next) {
        if ((subtype == 0xFFFF)
            || (subtype == ppp->subtype)) {
            if (nb == index) {
                *reply = my_Allocate(sizeof(u_int32_t));
                if (*reply == 0)
                    err = ENOMEM;
                else {
                    err = 0;
                    len = sizeof(u_int32_t);
                    *(u_int32_t*)*reply = ppp_makeref(ppp);
					if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
						*(u_int32_t*)*reply = htonl(*(u_int32_t*)*reply);
                }
                break;
            }
            nb++;
        }
    }
    
    msg->hdr.m_result = err;
    msg->hdr.m_len = len;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_getlinkbyserviceid(struct client *client, struct msg *msg, void **reply)
{
    u_long		len = 0, err = ENODEV;
    struct ppp		*ppp;
    CFStringRef		ref;

    msg->data[msg->hdr.m_len] = 0;
    ref = CFStringCreateWithCString(NULL, msg->data, kCFStringEncodingUTF8);
    if (ref) {
	ppp = ppp_findbyserviceID(ref);
        if (ppp) {
            *reply = my_Allocate(sizeof(u_int32_t));
            if (*reply == 0)
                err = ENOMEM;
            else {
                err = 0;
                len = sizeof(u_int32_t);
                *(u_int32_t*)*reply = ppp_makeref(ppp);
				if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
					*(u_int32_t*)*reply = htonl(*(u_int32_t*)*reply);
            }
        }
        CFRelease(ref);
    }
    else 
        err = ENOMEM;
    
    msg->hdr.m_result = err;
    msg->hdr.m_len = len;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_getlinkbyifname(struct client *client, struct msg *msg, void **reply)
{
    u_long		len = 0, err = ENODEV;
    struct ppp		*ppp;

    TAILQ_FOREACH(ppp, &ppp_head, next) {
        if (!strncmp(ppp->ifname, &msg->data[0], sizeof(ppp->ifname))) {
            
            if (msg->hdr.m_flags & USE_SERVICEID) {
                *reply = my_Allocate(strlen(ppp->sid));
                if (*reply == 0)
                    err = ENOMEM;
                else {
                    err = 0;
                    len = strlen(ppp->sid);
                    bcopy(ppp->sid, *reply, len);
                }
            }
            else {
                *reply = my_Allocate(sizeof(u_int32_t));
                if (*reply == 0)
                    err = ENOMEM;
                else {
                    err = 0;
                    len = sizeof(u_int32_t);
                    *(u_int32_t*)*reply = ppp_makeref(ppp);
					if (client->flags & CLIENT_FLAG_SWAP_BYTES) 
						*(u_int32_t*)*reply = htonl(*(u_int32_t*)*reply);
                }
            }
            break;
        }
    }

    msg->hdr.m_result = err;
    msg->hdr.m_len = len;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_pppd_event(struct client *client, struct msg *msg)
{
    struct ppp		*ppp = ppp_find(msg);
    void 		*data = &msg->data[MSG_DATAOFF(msg)];
    u_int32_t		event = *(u_int32_t *)data;
    u_int32_t		error = *(u_int32_t *)(data + 4);

    msg->hdr.m_len = 0xFFFFFFFF; // no reply
    //printf("ppp_event, event = 0x%x, cause = 0x%x, serviceid = '%s'\n", event, error, serviceid);

    if (!ppp)
        return;

    if (event == PPP_EVT_DISCONNECTED) {
        //if (error == EXIT_USER_REQUEST)
        //    return;	// PPP API generates PPP_EVT_DISCONNECTED only for unrequested disconnections
        error = ppp_translate_error(ppp->subtype, error, 0);
    }
    else 
        error = 0;
        
    client_notify(ppp->serviceID, ppp->sid, ppp_makeref(ppp), event, error, CLIENT_FLAG_NOTIFY_EVENT);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_pppd_status(struct client *client, struct msg *msg)
{
    struct ppp		*ppp = ppp_find(msg);
    void 		*data = &msg->data[MSG_DATAOFF(msg)];
    u_int32_t		status = *(u_int32_t *)data;
    u_int32_t		devstatus = *(u_int32_t *)(data + 4);

    msg->hdr.m_len = 0xFFFFFFFF; // no reply

    if (!ppp)
        return;
    
    ppp_updatestatus(ppp, status, devstatus);
    
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_pppd_phase(struct client *client, struct msg *msg)
{
    struct ppp		*ppp = ppp_find(msg);
    void 		*data = &msg->data[MSG_DATAOFF(msg)];
    u_int32_t		phase = *(u_int32_t *)data;

    msg->hdr.m_len = 0xFFFFFFFF; // no reply

    if (!ppp)
        return;

    ppp_updatephase(ppp, phase);

}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
CFMutableDictionaryRef prepare_entity(CFMutableDictionaryRef opts, CFStringRef entity, CFStringRef property)
{
    CFMutableDictionaryRef 	dict;
    
    dict = (CFMutableDictionaryRef)CFDictionaryGetValue(opts, entity);
    // make sure we get a valid dictionary here
    if (dict && (CFGetTypeID(dict) != CFDictionaryGetTypeID()))
        return 0;	
        
    if (dict == 0) {
    	dict = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (dict == 0)
            return 0;
        CFDictionaryAddValue((CFMutableDictionaryRef)opts, entity, dict);
        CFRelease(dict);
    }
    CFDictionaryRemoveValue(dict, property);
    
    return dict;
}


/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
int set_long_opt(CFMutableDictionaryRef opts, CFStringRef entity, CFStringRef property, u_long opt, u_long mini, u_long maxi, u_long limit)
{
    CFMutableDictionaryRef 	dict;
    CFNumberRef 		num;
    
    if (opt < mini) {
        if (limit) opt = mini;
        else return EINVAL;
    }
    else if (opt > maxi) {
        if (limit) opt = maxi;
        else return EINVAL;
    }

    dict = prepare_entity(opts, entity, property);
    if (dict == 0)
        return ENOMEM;
    
    num = CFNumberCreate(NULL, kCFNumberSInt32Type, &opt);
    if (num) {
        CFDictionaryAddValue(dict, property, num);
        CFRelease(num); 
    } 

    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
int set_str_opt(CFMutableDictionaryRef opts, CFStringRef entity, CFStringRef property, char *opt, int len, CFStringRef optref)
{
    CFMutableDictionaryRef 	dict;
    CFStringRef 		str;
    
    dict = prepare_entity(opts, entity, property);
    if (dict == 0)
        return ENOMEM;

    if (optref)
        CFDictionaryAddValue(dict, property, optref);
    else {
        opt[len] = 0;
        str = CFStringCreateWithCString(NULL, opt, kCFStringEncodingUTF8);
        if (str) {
            CFDictionaryAddValue(dict, property, str);
            CFRelease(str);
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
int set_array_opt(CFMutableDictionaryRef opts, CFStringRef entity, CFStringRef property, CFStringRef optref1, CFStringRef optref2)
{
    CFMutableDictionaryRef 	dict;
    CFMutableArrayRef 		array;
    
    dict = prepare_entity(opts, entity, property);
    if (dict == 0)
        return ENOMEM;
    
    array = CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);
    if (array == 0)
        return ENOMEM;
    
    if (optref1) 
        CFArrayAppendValue(array, optref1);
    if (optref2) 
        CFArrayAppendValue(array, optref2);
        
    CFDictionaryAddValue(dict, property, array);
    CFRelease(array);

    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static 
void remove_opt(CFMutableDictionaryRef opts, CFStringRef entity, CFStringRef property)
{
    CFMutableDictionaryRef	dict;
    
    dict = (CFMutableDictionaryRef)CFDictionaryGetValue(opts, entity);
    // make sure we get a valid dictionary here
    if (dict&& (CFGetTypeID(dict) == CFDictionaryGetTypeID()))
        CFDictionaryRemoveValue(dict, property);
}


/* -----------------------------------------------------------------------------
id must be a valid client 
----------------------------------------------------------------------------- */
static
void socket_setoption(struct client *client, struct msg *msg, void **reply)
{
    struct ppp_opt 	*opt = (struct ppp_opt *)&msg->data[MSG_DATAOFF(msg)];
    u_int32_t		optint = *(u_int32_t *)(&opt->o_data[0]);
    u_char		*optstr = &opt->o_data[0];
    CFMutableDictionaryRef	opts;
    u_long		err = 0, len = msg->hdr.m_len - sizeof(struct ppp_opt_hdr), speed;
    struct ppp 		*ppp = ppp_find(msg);
    CFStringRef		string1, string2;
    
    if (!ppp) {
        msg->hdr.m_result = ENODEV;
        msg->hdr.m_len = 0;
        return;
    }

	if (client->flags & CLIENT_FLAG_SWAP_BYTES) {
		opt->o_type = htonl(opt->o_type);
		optint = htonl(optint);
	}
	
    // not connected, set the client options that will be used.
    opts = client_findoptset(client, ppp->serviceID);
    if (!opts) {
        // first option used by client, create private set
        opts = client_newoptset(client, ppp->serviceID);
        if (!opts) {
            msg->hdr.m_result = ENOMEM;
            msg->hdr.m_len = 0;
            return;
        }
    }

    switch (opt->o_type) {

        // COMM options
        case PPP_OPT_DEV_NAME:
            err = set_str_opt(opts, kSCEntNetInterface, kSCPropNetInterfaceDeviceName, optstr, len, 0);
            break;
        case PPP_OPT_DEV_SPEED:
            // add flexibility and adapt the speed to the immediatly higher speed
            speed = optint;
            if (speed <= 1200) speed = 1200;
            else if ((speed > 1200) && (speed <= 2400)) speed = 2400;
            else if ((speed > 2400) && (speed <= 9600)) speed = 9600;
            else if ((speed > 9600) && (speed <= 19200)) speed = 19200;
            else if ((speed > 19200) && (speed <= 38400)) speed = 38400;
            else if ((speed > 38400) && (speed <= 57600)) speed = 57600;
            else if ((speed > 38400) && (speed <= 57600)) speed = 57600;
            else if ((speed > 57600) && (speed <= 0xFFFFFFFF)) speed = 115200;
            err = set_long_opt(opts, kSCEntNetModem, kSCPropNetModemSpeed, speed, 0, 0xFFFFFFFF, 0);
            break;
        case PPP_OPT_DEV_CONNECTSCRIPT:
            err = set_str_opt(opts, kSCEntNetModem, kSCPropNetModemConnectionScript, optstr, len, 0);
            break;
        case PPP_OPT_DEV_DIALMODE:
            string1 = kSCValNetModemDialModeWaitForDialTone;
            switch (optint) {
                case PPP_DEV_IGNOREDIALTONE:
                    string1 = kSCValNetModemDialModeIgnoreDialTone;
                    break;
                case PPP_DEV_MANUALDIAL:
                    string1 = kSCValNetModemDialModeManual;
                    break;
            }
            if (string1)
                set_str_opt(opts, kSCEntNetModem, kSCPropNetModemDialMode, 0, 0, string1);
            break;
        case PPP_OPT_COMM_TERMINALMODE:
            switch (optint) {
                case PPP_COMM_TERM_NONE:
                    remove_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommDisplayTerminalWindow);
                    remove_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommUseTerminalScript);
                    break;
                case PPP_COMM_TERM_SCRIPT:
                    err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommUseTerminalScript, 1, 0, 1, 1)
                    	|| set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommDisplayTerminalWindow, 0, 0, 1, 1);
                    break;
                case PPP_COMM_TERM_WINDOW:
                    err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommUseTerminalScript, 0, 0, 1, 1)
                    	|| set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommDisplayTerminalWindow, 1, 0, 1, 1);
                    break;
            }
	    break;
        case PPP_OPT_COMM_TERMINALSCRIPT:
            err = set_str_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommTerminalScript, optstr, len, 0);
            break;
        case PPP_OPT_COMM_REMOTEADDR:
            err = set_str_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommRemoteAddress, optstr, len, 0);
            break;
        case PPP_OPT_COMM_IDLETIMER:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPDisconnectOnIdleTimer, optint, 0, 0xFFFFFFFF, 1)
                || set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPDisconnectOnIdle, optint, 0, 1, 1);
            break;
        case PPP_OPT_COMM_SESSIONTIMER:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPSessionTimer, optint, 0, 0xFFFFFFFF, 1)
            	|| set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPUseSessionTimer, optint, 0, 1, 1);
            break;
        case PPP_OPT_COMM_CONNECTDELAY:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPCommConnectDelay, optint, 0, 0xFFFFFFFF, 1);
            break;

            // LCP options
        case PPP_OPT_LCP_HDRCOMP:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPCompressionPField, optint & PPP_LCP_HDRCOMP_PROTO, 0, 1, 1)
                || set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPCompressionACField, optint & PPP_LCP_HDRCOMP_ADDR, 0, 1, 1);
            break;
        case PPP_OPT_LCP_MRU:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPMRU, optint, 0, 0xFFFFFFFF, 1);
            break;
        case PPP_OPT_LCP_MTU:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPMTU, optint, 0, 0xFFFFFFFF, 1);
            break;
        case PPP_OPT_LCP_RCACCM:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPReceiveACCM, optint, 0, 0xFFFFFFFF, 1);
            break;
        case PPP_OPT_LCP_TXACCM:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPTransmitACCM, optint, 0, 0xFFFFFFFF, 1);
            break;
        case PPP_OPT_LCP_ECHO:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPEchoInterval, ((struct ppp_opt_echo *)opt->o_data)->interval, 0, 0xFFFFFFFF, 1)
            	|| set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPLCPEchoFailure, ((struct ppp_opt_echo *)opt->o_data)->failure, 0, 0xFFFFFFFF, 1);
            break;

            // SEC options
        case PPP_OPT_AUTH_PROTO:
            string1 = string2 = 0;
            switch (optint) {
                case PPP_AUTH_NONE:
                    string1 = CFSTR("None");//kSCValNetPPPAuthProtocolNone;
                    break;
                case PPP_AUTH_PAP:
                    string1 = kSCValNetPPPAuthProtocolPAP;
                    break;
                case PPP_AUTH_CHAP:
                    string2 = kSCValNetPPPAuthProtocolCHAP;
                    break;
                case PPP_AUTH_PAPCHAP:
                    string1 = kSCValNetPPPAuthProtocolPAP;
                    string2 = kSCValNetPPPAuthProtocolCHAP;
                    break;
                default:
                    err = EINVAL;
            }
            if (string1 || string2)
                err = set_array_opt(opts, kSCEntNetPPP, kSCPropNetPPPAuthProtocol, string1, string2);
            break;
        case PPP_OPT_AUTH_NAME:
           err = set_str_opt(opts, kSCEntNetPPP, kSCPropNetPPPAuthName, optstr, len, 0);
            break;
        case PPP_OPT_AUTH_PASSWD:
            err = set_str_opt(opts, kSCEntNetPPP, kSCPropNetPPPAuthPassword, optstr, len, 0);
            break;

            // IPCP options
        case PPP_OPT_IPCP_HDRCOMP:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPIPCPCompressionVJ, optint, 0, 1, 1);
            break;
        case PPP_OPT_IPCP_REMOTEADDR:
            string1 = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d.%d.%d.%d"), 
                optint >> 24, (optint >> 16) & 0xFF, (optint >> 8) & 0xFF, optint & 0xFF);
            if (string1) {
                err = set_array_opt(opts, kSCEntNetIPv4, kSCPropNetIPv4DestAddresses, string1, 0);
                CFRelease(string1);
            }
            break;
        case PPP_OPT_IPCP_LOCALADDR:
            string1 = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d.%d.%d.%d"), 
                optint >> 24, (optint >> 16) & 0xFF, (optint >> 8) & 0xFF, optint & 0xFF);
            if (string1) {
                err = set_array_opt(opts, kSCEntNetIPv4, kSCPropNetIPv4Addresses, string1, 0);
                CFRelease(string1);
            }
            break;
            // MISC options
        case PPP_OPT_LOGFILE:
            err = EOPNOTSUPP;
            //err = set_str_opt(&opts->misc.logfile, optstr, len);
            break;
        case PPP_OPT_COMM_REMINDERTIMER:
            err = set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPIdleReminderTimer, optint, 0, 0xFFFFFFFF, 1)
                || set_long_opt(opts, kSCEntNetPPP, kSCPropNetPPPIdleReminder, optint, 0, 1, 1);
            break;
        case PPP_OPT_ALERTENABLE:
            err = set_long_opt(opts, kSCEntNetPPP, CFSTR("AlertEnable"), optint, 0, 0xFFFFFFFF, 1);
            break;
        default:
            err = EOPNOTSUPP;
    };
    
    msg->hdr.m_result = err;
    msg->hdr.m_len = 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
static
void socket_getoption (struct client *client, struct msg *msg, void **reply)
{
    struct ppp_opt 		*opt = (struct ppp_opt *)&msg->data[MSG_DATAOFF(msg)];
    CFDictionaryRef		opts;
    struct ppp 			*ppp = ppp_find(msg);
    u_int8_t			optdata[OPT_STR_LEN + 1];
    u_int32_t			optlen;
	
    if (!ppp) {
        msg->hdr.m_len = 0;
        msg->hdr.m_result = ENODEV;
        return;
    }
	
	if (client->flags & CLIENT_FLAG_SWAP_BYTES)
		opt->o_type = htonl(opt->o_type);

    if (ppp->phase != PPP_IDLE)
        // take the active user options
        opts = ppp->connectopts; 
    else 
        // not connected, get the client options that will be used.
        opts = client_findoptset(client, ppp->serviceID);
    
    if (!ppp_getoptval(ppp, opts, 0, opt->o_type, optdata, &optlen)) {
        msg->hdr.m_len = 0;
        msg->hdr.m_result = EOPNOTSUPP;
        return;
    }
    
	*reply = my_Allocate(sizeof(struct ppp_opt_hdr) + optlen);
    if (*reply == 0) {
        msg->hdr.m_result = ENOMEM;
        msg->hdr.m_len = 0;
        return;
    }

	if (client->flags & CLIENT_FLAG_SWAP_BYTES) {
		switch(opt->o_type) {
			// all 4 bytes options
			case PPP_OPT_DEV_SPEED:
			case PPP_OPT_COMM_IDLETIMER:
			case PPP_OPT_AUTH_PROTO:
			case PPP_OPT_LCP_HDRCOMP:
			case PPP_OPT_LCP_MRU:
			case PPP_OPT_LCP_MTU:
			case PPP_OPT_LCP_RCACCM:
			case PPP_OPT_LCP_TXACCM:
			case PPP_OPT_IPCP_HDRCOMP:
			case PPP_OPT_IPCP_LOCALADDR:
			case PPP_OPT_IPCP_REMOTEADDR:
			case PPP_OPT_RESERVED:
			case PPP_OPT_COMM_REMINDERTIMER:
			case PPP_OPT_ALERTENABLE:
			case PPP_OPT_COMM_CONNECTDELAY:
			case PPP_OPT_COMM_SESSIONTIMER:
			case PPP_OPT_COMM_TERMINALMODE:
			case PPP_OPT_DEV_CONNECTSPEED:
			case PPP_OPT_DEV_DIALMODE:
			case PPP_OPT_DIALONDEMAND:
			case PPP_OPT_LCP_ECHO:
				*(u_int32_t*)optdata = htonl(*(u_int32_t*)optdata);
				break;
		}
	}

    bcopy(opt, *reply, sizeof(struct ppp_opt_hdr));
    bcopy(optdata, (*reply) + sizeof(struct ppp_opt_hdr), optlen);
    
    msg->hdr.m_result = 0;
    msg->hdr.m_len = sizeof(struct ppp_opt_hdr) + optlen;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
void socket_client_notify (CFSocketRef ref, u_char *sid, u_int32_t link, u_long event, u_long error, u_int32_t flags)
{
    struct ppp_msg_hdr	msg;    
	int link_len;
	
	bzero(&msg, sizeof(msg));
	msg.m_type = PPP_EVENT;
	msg.m_link = link;
	msg.m_result = event;
	msg.m_cookie = error;
	if (sid) {
		msg.m_flags |= USE_SERVICEID;
		msg.m_link = strlen(sid);
		link_len = msg.m_link; /* save len */
	}
	
	/* swap back bytes that have been assigned by internal processing functions */
	if (flags & CLIENT_FLAG_SWAP_BYTES) {
		msg.m_flags = htons(msg.m_flags);
		msg.m_type = htons(msg.m_type);
		msg.m_result = htonl(msg.m_result);
		msg.m_cookie = htonl(msg.m_cookie);
		msg.m_link = htonl(msg.m_link);
		msg.m_len = htonl(msg.m_len);
	}

	if (writen(CFSocketGetNative(ref), &msg, sizeof(msg)) != sizeof(msg))
		return;

	if (sid) {
		if (writen(CFSocketGetNative(ref), sid, link_len) != link_len)
			return;
	}
}


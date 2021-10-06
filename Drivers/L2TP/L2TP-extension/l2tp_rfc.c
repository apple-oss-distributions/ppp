/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
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


#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/malloc.h>
#include <sys/syslog.h>
#include <sys/domain.h>

#include <machine/spl.h>

#include <net/if_var.h>

#include "../../../Family/if_ppplink.h"
#include "../../../Family/ppp_domain.h"
#include "l2tp.h"
#include "l2tp_rfc.h"
#include "l2tp_udp.h"
#include "l2tpk.h"


/* -----------------------------------------------------------------------------
Definitions
----------------------------------------------------------------------------- */


#define L2TP_STATE_SESSION_EST	0x00000001	/* session is established - data can be transfered */
#define L2TP_STATE_NEW_SEQUENCE	0x00000002	/* we have a seq number to acknowledge */
#define L2TP_STATE_FREEING	0x00000004	/* rfc has been freed. structure is kept for 31 seconds */


/*
 * l2tp sequence numbers are 16 bit integers operated
 * on with modular arithmetic.  These macros can be
 * used to compare such integers.
 */
#define SEQ_LT(a,b)     ((int16_t)(((int16_t)(a))-((int16_t)(b))) < 0)
#define SEQ_LEQ(a,b)    ((int16_t)(((int16_t)(a))-((int16_t)(b))) <= 0)
#define SEQ_GT(a,b)     ((int16_t)(((int16_t)(a))-((int16_t)(b))) > 0)
#define SEQ_GEQ(a,b)    ((int16_t)(((int16_t)(a))-((int16_t)(b))) >= 0)

#define ROUND16DIFF(a, b)  	((a >= b) ? (a - b) : (0xFFFF - b + a + 1))
#define ABS(a) 			(a >= 0 ? a : -a)

struct l2tp_elem {
    TAILQ_ENTRY(l2tp_elem)	next;
    struct mbuf *		packet;
    u_int16_t			seqno;
    u_int8_t			addr[INET6_ADDRSTRLEN]; /* use the largest address between v4 and v6 */
};


struct l2tp_rfc {

    // administrative info
    TAILQ_ENTRY(l2tp_rfc) 	next;
    void 			*host; 			/* pointer back to the hosting structure */
    l2tp_rfc_input_callback 	inputcb;		/* callback function when data are present */
    l2tp_rfc_event_callback 	eventcb;		/* callback function for events */
    struct socket	 	*socket;		/* socket used for udp packets */
    
    // l2tp info
    u_int32_t  		state;				/* state information */
    u_int32_t  		flags;				/* miscellaneous flags */
    struct sockaddr*	peer_address;			/* ip address we are connected to */
    struct sockaddr*	our_address;			/* our side of the tunnel */
    u_int16_t		our_tunnel_id;			/* our tunnel id */
    u_int16_t		peer_tunnel_id;			/* peer's tunnel id */
    u_int16_t		our_session_id;			/* our session id */
    u_int16_t		peer_session_id;		/* peer's session id */
    u_int16_t		our_window;			/* our recv window */
    u_int16_t		peer_window;			/* peer's recv window */
    u_int16_t		free_time_remain;		/* time until rfc is freed - seconds/2 */
    u_int16_t		initial_timeout;		/* initial timeout value - seconds/2 */
    u_int16_t		timeout_cap;			/* maximum timeout cap - seconds/2 */
    u_int16_t		max_retries;			/* maximum retries allowed */
    u_int16_t		retry_count;			/* current retry count */
    u_int16_t		retrans_time_remain;		/* time until next retransmission - seconds/2 */
    u_int16_t		our_ns;				/* last seq number we sent */
    u_int16_t		our_nr;				/* last seq number we acked */
    u_int16_t		peer_nr;			/* last seq number peer acked */
    u_int16_t		our_last_data_seq;		/* last data seq number we sent */
    u_int16_t		peer_last_data_seq;		/* last data seq number we received */
    TAILQ_HEAD(, l2tp_elem) send_queue;		/* control message send queue */
    TAILQ_HEAD(, l2tp_elem) recv_queue;		/* control or sequenced data message recv queue */

};

#define LOGIT(rfc, str, args...)	\
    if (rfc->flags & L2TP_FLAG_DEBUG)	\
        log(LOG_INFO, str, args)

/* -----------------------------------------------------------------------------
Globals
----------------------------------------------------------------------------- */

TAILQ_HEAD(, l2tp_rfc) 	l2tp_rfc_head;
static u_int16_t unique_tunnel_id = 0;

/* -----------------------------------------------------------------------------
Forward declarations
----------------------------------------------------------------------------- */

u_int16_t l2tp_rfc_output_control(struct l2tp_rfc *rfc, struct mbuf *m, struct sockaddr *to);
u_int16_t l2tp_rfc_output_data(struct l2tp_rfc *rfc, struct mbuf *m);
int l2tp_rfc_output_queued(struct l2tp_rfc *rfc, struct l2tp_elem *elem);
int l2tp_rfc_compare_address(struct sockaddr* addr1, struct sockaddr* addr2);
void l2tp_rfc_handle_ack(struct l2tp_rfc *rfc, u_int16_t nr);
u_int16_t l2tp_handle_data(struct l2tp_rfc *rfc, struct mbuf *m, struct sockaddr *from, 
    u_int16_t flags, u_int16_t len, u_int16_t tunnel_id, u_int16_t session_id);
u_int16_t l2tp_handle_control(struct l2tp_rfc *rfc, struct mbuf *m, struct sockaddr *from, 
    u_int16_t flags, u_int16_t len, u_int16_t tunnel_id, u_int16_t session_id);
void l2tp_rfc_free_now(struct l2tp_rfc *rfc);
void l2tp_rfc_accept(struct l2tp_rfc* rfc);

/* -----------------------------------------------------------------------------
intialize L2TP protocol
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_init()
{

    l2tp_udp_init();
    TAILQ_INIT(&l2tp_rfc_head);
    return 0;
}

/* -----------------------------------------------------------------------------
dispose of a L2TP protocol
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_dispose()
{

    if (TAILQ_FIRST(&l2tp_rfc_head))
        return 1;
    if (l2tp_udp_dispose())
        return 1;
    return 0;
}

/* -----------------------------------------------------------------------------
intialize a new L2TP structure
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_new_client(void *host, void **data,
                         l2tp_rfc_input_callback input, 
                         l2tp_rfc_event_callback event)
{
    struct l2tp_rfc 	*rfc;
        
    if (input == 0 || event == 0)
        return EINVAL;
    
    rfc = (struct l2tp_rfc *)_MALLOC(sizeof (struct l2tp_rfc), M_TEMP, M_WAITOK);
    if (rfc == 0)
        return 1;

    //log(LOG_INFO, "L2TP new_client rfc = 0x%x\n", rfc);

    bzero(rfc, sizeof(struct l2tp_rfc));

    rfc->host = host;
    rfc->inputcb = input;
    rfc->eventcb = event;
    rfc->timeout_cap = L2TP_DEFAULT_TIMEOUT_CAP * 2;		
    rfc->initial_timeout = L2TP_DEFAULT_INITIAL_TIMEOUT * 2;	
    rfc->max_retries = L2TP_DEFAULT_RETRY_COUNT;
    rfc->flags = L2TP_FLAG_ADAPT_TIMER;
    
    // let's use some default values
    rfc->peer_window = L2TP_DEFAULT_WINDOW_SIZE;
    rfc->our_window = L2TP_DEFAULT_WINDOW_SIZE;
    
    TAILQ_INIT(&rfc->send_queue);
    TAILQ_INIT(&rfc->recv_queue);
   
    *data = rfc;

    TAILQ_INSERT_TAIL(&l2tp_rfc_head, rfc, next);

    return 0;
}

/* -----------------------------------------------------------------------------
prepare for dispose of a L2TP structure
----------------------------------------------------------------------------- */
void l2tp_rfc_free_client(void *data)
{
    struct l2tp_rfc 		*rfc = (struct l2tp_rfc *)data;
    
    if (rfc->flags & L2TP_FLAG_CONTROL 
        && rfc->our_tunnel_id && rfc->peer_tunnel_id) {
        /* keep control connections around for a full retransmission cycle */
        
        LOGIT(rfc, "L2TP prepare for freeing (0x%x)\n", rfc);
        
        rfc->host = 0;
        rfc->state |= L2TP_STATE_FREEING;
        rfc->free_time_remain = 62; // give 31 seconds
    }
    else {
        /* immediatly dispose of data connections */
        l2tp_rfc_free_now(rfc);
    }
}

/* -----------------------------------------------------------------------------
dispose of a L2TP structure
----------------------------------------------------------------------------- */
void l2tp_rfc_free_now(struct l2tp_rfc *rfc)
{
    struct l2tp_elem 	*send_elem;
    struct l2tp_elem	*recv_elem;
    struct l2tp_rfc 	*rfc1;
    
    LOGIT(rfc, "L2TP free (0x%x)\n", rfc);
    
    if (rfc->socket) {
        /* the control connection own socket */
        if (rfc->flags & L2TP_FLAG_CONTROL){
            // find an rfc with same socket
            TAILQ_FOREACH(rfc1, &l2tp_rfc_head, next)
                if (rfc1 != rfc
                    && (rfc1->flags & L2TP_FLAG_CONTROL)
                    && (rfc1->socket == rfc->socket))
                    break;
            if (rfc1 == 0)
                l2tp_udp_detach(rfc->socket);
        }
        rfc->socket = 0;
    }

    if (rfc->peer_address)
        _FREE(rfc->peer_address, M_SONAME);
                            
    while(send_elem = TAILQ_FIRST(&rfc->send_queue)) {
        TAILQ_REMOVE(&rfc->send_queue, send_elem, next);
        m_freem(send_elem->packet);
        _FREE(send_elem, M_TEMP);
    }
    while(recv_elem = TAILQ_FIRST(&rfc->recv_queue)) {
        TAILQ_REMOVE(&rfc->recv_queue, recv_elem, next);
        m_freem(recv_elem->packet);
        _FREE(recv_elem, M_TEMP);
    }

    TAILQ_REMOVE(&l2tp_rfc_head, rfc, next);
    _FREE(rfc, M_TEMP);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_command(void *data, u_int32_t cmd, void *cmddata)
{
    struct l2tp_rfc 	*rfc1, *rfc = (struct l2tp_rfc *)data;
    u_int16_t		error = 0;
    int			len;
    u_char 		*p;

    switch (cmd) {

        case L2TP_CMD_SETFLAGS:
            LOGIT(rfc, "L2TP command (0x%x): set flags = 0x%x\n", rfc, *(u_int32_t *)cmddata);
            rfc->flags = *(u_int32_t *)cmddata;
            break;

        case L2TP_CMD_GETFLAGS:
            LOGIT(rfc, "L2TP command (0x%x): get flags = 0x%x\n", rfc, rfc->flags);
            *(u_int32_t *)cmddata = rfc->flags;
            break;

        case L2TP_CMD_SETWINDOW:
            LOGIT(rfc, "L2TP command (0x%x): set window = %d\n", rfc, *(u_int16_t *)cmddata);
            rfc->our_window = *(u_int16_t *)cmddata;
            break;

        case L2TP_CMD_SETPEERWINDOW:
            LOGIT(rfc, "L2TP command (0x%x): set peer window = %d\n", rfc, *(u_int16_t *)cmddata);
            rfc->peer_window = *(u_int16_t *)cmddata;
            break;

        case L2TP_CMD_GETNEWTUNNELID:
            /* make up a unique tunnel id */
            do {
                unique_tunnel_id++;
                if (unique_tunnel_id == 0)
                    unique_tunnel_id++;
                TAILQ_FOREACH(rfc1, &l2tp_rfc_head, next)
                    if (rfc1->our_tunnel_id == unique_tunnel_id)
                        break;
            } while (rfc1);
            *(u_int16_t *)cmddata = rfc->our_tunnel_id = unique_tunnel_id;
            LOGIT(rfc, "L2TP command (0x%x): get new tunnel id = 0x%x\n", rfc, *(u_int16_t *)cmddata);
            break;
            
        case L2TP_CMD_SETTUNNELID:
            LOGIT(rfc, "L2TP command (0x%x): set tunnel id = 0x%x\n", rfc, *(u_int16_t *)cmddata);
            rfc->our_tunnel_id = *(u_int16_t *)cmddata;
            if (!(rfc->flags & L2TP_FLAG_CONTROL)) {
                /* for data connection, join the existing socket of the associated control connection */
                rfc->socket = 0;
                TAILQ_FOREACH(rfc1, &l2tp_rfc_head, next)
                    if ((rfc1->flags & L2TP_FLAG_CONTROL)
                        && (rfc->our_tunnel_id == rfc1->our_tunnel_id)) {
                        rfc->socket = rfc1->socket;
                        break;
                    }
            }
            break;

        case L2TP_CMD_GETTUNNELID:
            LOGIT(rfc, "L2TP command (0x%x): get tunnel id = 0x%x\n", rfc, rfc->our_tunnel_id);
            *(u_int16_t *)cmddata =  rfc->our_tunnel_id;
            break;

        case L2TP_CMD_SETPEERTUNNELID:
            LOGIT(rfc, "L2TP command (0x%x): set peer tunnel id = 0x%x\n", rfc, *(u_int16_t *)cmddata);
            rfc->peer_tunnel_id = *(u_int16_t *)cmddata;
            break;

        case L2TP_CMD_SETSESSIONID:
            LOGIT(rfc, "L2TP command (0x%x): set session id = 0x%x\n", rfc, *(u_int16_t *)cmddata);
            if (!(rfc->flags & L2TP_FLAG_CONTROL))
                rfc->our_session_id = *(u_int16_t *)cmddata;
            break;

        case L2TP_CMD_GETSESSIONID:
            LOGIT(rfc, "L2TP command (0x%x): get session id = 0x%x\n", rfc, rfc->our_session_id);
            *(u_int16_t *)cmddata =  rfc->our_session_id;
            break;

        case L2TP_CMD_SETPEERSESSIONID:
            LOGIT(rfc, "L2TP command (0x%x): set peer session id = 0x%x\n", rfc, *(u_int16_t *)cmddata);
            /* session id only used for data */
            if (!(rfc->flags & L2TP_FLAG_CONTROL))
                rfc->peer_session_id = *(u_int16_t *)cmddata;
            break;
		
        case L2TP_CMD_SETTIMEOUT:
            LOGIT(rfc, "L2TP command (0x%x): set initial timeout = %d (seconds)\n", rfc, *(u_int16_t *)cmddata);
            rfc->initial_timeout = *(u_int16_t *)cmddata * 2;	
            break;

        case L2TP_CMD_SETTIMEOUTCAP:
            LOGIT(rfc, "L2TP command (0x%x): set timeout cap = %d (seconds)\n", rfc, *(u_int16_t *)cmddata);
            rfc->timeout_cap = *(u_int16_t *)cmddata * 2;	
            break;

        case L2TP_CMD_SETMAXRETRIES:
            LOGIT(rfc, "L2TP command (0x%x): set max retries = %d\n", rfc, *(u_int16_t *)cmddata);
            rfc->max_retries = *(u_int16_t *)cmddata;
            break;

        case L2TP_CMD_ACCEPT:
            LOGIT(rfc, "L2TP command (0x%x): accept\n", rfc);
            l2tp_rfc_accept(rfc);	
            break;

        case L2TP_CMD_SETPEERADDR:	
            if (rfc->peer_address) {
                _FREE(rfc->peer_address, M_SONAME);
                rfc->peer_address = 0;
            }
            p = (u_int8_t*)cmddata;
            len = *((u_int8_t*)cmddata);
            if (len == 0) {
                LOGIT(rfc, "L2TP command (0x%x): set peer IP address = no address\n", rfc);
                break;
            }
            LOGIT(rfc, "L2TP command (0x%x): set peer IP address = %d.%d.%d.%d, port %d\n", rfc, p[4], p[5], p[6], p[7], *((u_int16_t *)(p+2)));
            /* copy the address - this can handle IPv6 addresses */
            if (len > INET6_ADDRSTRLEN) {
                error = EINVAL; 
                break;
            }
            rfc->peer_address = _MALLOC(len, M_SONAME, M_WAITOK);
            if (rfc->peer_address == 0)
                error = ENOMEM;
            else {
                bcopy((u_int8_t*)cmddata, rfc->peer_address, len);
                if (rfc->flags & L2TP_FLAG_CONTROL) {
                    /* for control connections, set the other end of the socket */
                    error = l2tp_udp_setpeer(rfc->socket, rfc->peer_address);
                        
                    if (error == EADDRINUSE) {
                        // find the rfc with same src/dst pair
                        TAILQ_FOREACH(rfc1, &l2tp_rfc_head, next)
                            if (rfc1 != rfc
                                && (rfc1->flags & L2TP_FLAG_CONTROL)
                                && rfc1->our_address 
                                && rfc1->peer_address
                                && !l2tp_rfc_compare_address(rfc1->our_address, rfc->our_address) 
                                && !l2tp_rfc_compare_address(rfc1->peer_address, rfc->peer_address)) {
                                // use socket from other rfc
                                l2tp_udp_detach(rfc->socket);
                                rfc->socket = rfc1->socket;
                                error = 0;
                                break;
                            }
                    }
                }
            }
            break;
        
        case L2TP_CMD_GETPEERADDR:
            p = (u_int8_t*)cmddata;
            len = *((u_int8_t*)cmddata);
            bzero(p, len);
            if (rfc->peer_address)
                bcopy((u_int8_t*)rfc->peer_address, p, MIN(len, rfc->peer_address->sa_len));
            LOGIT(rfc, "L2TP command (0x%x): get peer IP address = %d.%d.%d.%d, port %d\n", rfc, p[4], p[5], p[6], p[7], *((u_int16_t *)(p+2)));
            break;
            
        case L2TP_CMD_SETOURADDR:	
            if (rfc->our_address) {
                _FREE(rfc->our_address, M_SONAME);
                rfc->our_address = 0;
            }
            if (rfc->socket) {
                if (rfc->flags & L2TP_FLAG_CONTROL) {
                    // find an rfc with same socket
                    TAILQ_FOREACH(rfc1, &l2tp_rfc_head, next)
                        if (rfc1 != rfc
                            && (rfc1->flags & L2TP_FLAG_CONTROL)
                            && (rfc1->socket == rfc->socket))
                            break;
                    if (rfc1 == 0)
                        l2tp_udp_detach(rfc->socket);
                }
                rfc->socket = 0;
            }
            p = (u_int8_t*)cmddata;
            len = *((u_int8_t*)cmddata);
            if (len == 0) {
                LOGIT(rfc, "L2TP command (0x%x): set our IP address = no address\n", rfc);
                break;
            }
            LOGIT(rfc, "L2TP command (0x%x): set our IP address = %d.%d.%d.%d, port %d\n", rfc, p[4], p[5], p[6], p[7], *((u_int16_t *)(p+2)));
            /* copy the address - this can handle IPv6 addresses */
            if (len > INET6_ADDRSTRLEN) {
                error = EINVAL; 
                break;
            }
            rfc->our_address = _MALLOC(len, M_SONAME, M_WAITOK);
            if (rfc->our_address == 0)
                error = ENOMEM;
            else {
                bcopy((u_int8_t*)cmddata, rfc->our_address, len);
                if (rfc->flags & L2TP_FLAG_CONTROL) 
                    /* for control connections, create a socket and bind */
                    error = l2tp_udp_attach(&rfc->socket, rfc->our_address);
            }
            break;

        case L2TP_CMD_GETOURADDR:
            p = (u_int8_t*)cmddata;
            len = *((u_int8_t*)cmddata);
            bzero(p, len);
            if (rfc->our_address)
                bcopy((u_int8_t*)rfc->our_address, p, MIN(len, rfc->our_address->sa_len));
            LOGIT(rfc, "L2TP command (0x%x): get our IP address = %d.%d.%d.%d, port %d\n", rfc, p[4], p[5], p[6], p[7], *((u_int16_t *)(p+2)));
            break;

        default:
            LOGIT(rfc, "L2TP command (0x%x): unknown command = %d\n", rfc, cmd);
    }

    return error;
}


/* -----------------------------------------------------------------------------
called by protocol family when fast timer expires
----------------------------------------------------------------------------- */
void l2tp_rfc_fasttimer()
{
    struct mbuf		*m;
    struct l2tp_header 	*hdr;
    struct l2tp_rfc 	*rfc;
    
    TAILQ_FOREACH(rfc, &l2tp_rfc_head, next)
        if ((rfc->state & L2TP_STATE_NEW_SEQUENCE) && rfc->peer_tunnel_id) {

            MGETHDR(m, M_DONTWAIT, MT_DATA);
            if (m == 0)
                return;
            
            m->m_len = L2TP_CNTL_HDR_SIZE;
            m->m_pkthdr.len = L2TP_CNTL_HDR_SIZE;
            hdr = mtod(m, struct l2tp_header*);
        
            bzero(hdr, L2TP_CNTL_HDR_SIZE);
            
            hdr->flags_vers = htons(L2TP_FLAGS_L | L2TP_FLAGS_T | L2TP_FLAGS_S | L2TP_HDR_VERSION); 
            hdr->len = htons(L2TP_CNTL_HDR_SIZE); 
            
            hdr->ns = htons(rfc->our_ns);
            hdr->nr = htons(rfc->our_nr);
            hdr->tunnel_id = htons(rfc->peer_tunnel_id);
            hdr->session_id = 0;
            rfc->state &= ~L2TP_STATE_NEW_SEQUENCE;
            
            l2tp_udp_output(rfc->socket, m, rfc->peer_address);
        }
}

/* -----------------------------------------------------------------------------
called by protocol family when slow timer expires

    Decrements retrans_time_remain and checks if time to re-send the message
    at the beginning of the transmit queue.  If retry count is exhasted,
    time to break the connection.
----------------------------------------------------------------------------- */
void l2tp_rfc_slowtimer()
{
    struct l2tp_rfc  	*rfc1, *rfc = TAILQ_FIRST(&l2tp_rfc_head);

    while (rfc) {

        if (rfc->state & L2TP_STATE_FREEING 
            && --rfc->free_time_remain == 0) {
            
            rfc1 = TAILQ_NEXT(rfc, next);
            l2tp_rfc_free_now(rfc);
            rfc = rfc1;
            continue;
        }

        if (!TAILQ_EMPTY(&rfc->send_queue)) {
            if (--rfc->retrans_time_remain == 0) {
                rfc->retry_count++;
                if (rfc->retry_count >= rfc->max_retries) {
                    /* send event to client */
                    if (!(rfc->state & L2TP_STATE_FREEING))
                        (*rfc->eventcb)(rfc->host, L2TP_EVT_RELIABLE_FAILED, 0);
                }
                else {
                    l2tp_rfc_output_queued(rfc, TAILQ_FIRST(&rfc->send_queue)); 
                    if (rfc->flags & L2TP_FLAG_ADAPT_TIMER)
                        rfc->retrans_time_remain = rfc->initial_timeout << rfc->retry_count; 
                    else 
                        rfc->retrans_time_remain = rfc->initial_timeout;
                    if (rfc->retrans_time_remain > rfc->timeout_cap)
                        rfc->retrans_time_remain = rfc->timeout_cap;
                }
            }
        }
    	
        rfc = TAILQ_NEXT(rfc, next);
    }
}

/* -----------------------------------------------------------------------------
take the packet present in the recv queue of the first rfc with tunnel id 0
and transfer it to the given rfc.
This is useful to listen for incoming connection on a generic tunnel 0 rfc, and
accepting it on an other created rfc.
----------------------------------------------------------------------------- */
void l2tp_rfc_accept(struct l2tp_rfc* rfc)
{
    struct l2tp_rfc 		*call_rfc;
    struct l2tp_elem	*elem;
    
    TAILQ_FOREACH(call_rfc, &l2tp_rfc_head, next) {
        if ((call_rfc->flags & L2TP_FLAG_CONTROL) 
            && call_rfc->our_tunnel_id == 0
            && !TAILQ_EMPTY(&call_rfc->recv_queue)) {
            
            /* transfer packet to new rfc */
            elem = TAILQ_FIRST(&call_rfc->recv_queue);
            TAILQ_REMOVE(&call_rfc->recv_queue, elem, next);	/* remove the packet from the call socket */
            
            rfc->our_nr = 1;							/* set nr to the correct value */
            rfc->state |= L2TP_STATE_NEW_SEQUENCE;				/* setup to send ack */
            if ((*rfc->inputcb)(rfc->host, elem->packet, (struct sockaddr*)elem->addr, 1))	/* up to the socket */
                m_freem(elem->packet);
            _FREE(elem, M_TEMP);

            return;
        }
    }
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_output(void *data, struct mbuf *m, struct sockaddr *to)
{
    struct l2tp_rfc 	*rfc = (struct l2tp_rfc *)data;
    
    if (rfc->state & L2TP_STATE_FREEING) {
        m_freem(m);
        return ENXIO;
    }

    /* control packet are received from pppd with an incomplete l2tp header in front,
        and an ip address to send to */
    if (rfc->flags & L2TP_FLAG_CONTROL)
        return l2tp_rfc_output_control(rfc, m, to);
    
    /* data packet are received from ppp stack without a l2tp header and without address
        and an ip address to send to */
    return l2tp_rfc_output_data(data, m);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_output_control(struct l2tp_rfc *rfc, struct mbuf *m, struct sockaddr *to)
{
    struct l2tp_elem 	*elem;
    struct l2tp_header		*hdr;
    struct mbuf 		*m0;
    u_int16_t 			len;

    len = 0;
    for (m0 = m; m0 != 0; m0 = m0->m_next)
        len += m0->m_len;
                
    hdr = mtod(m, struct l2tp_header *);
    
    /* flags, version and length should have been filled already by pppd */
    hdr->flags_vers = htons(L2TP_FLAGS_L | L2TP_HDR_VERSION | L2TP_FLAGS_T | L2TP_FLAGS_S);
    hdr->len = htons(len); 
    
    /* session id MUST be filled by the L2TP plugin */
    /* hdr->session_id = htons(rfc->peer_session_id); */
    
    hdr->tunnel_id = htons(rfc->peer_tunnel_id);
    
    /* we fill the tunnel id and the sequence information */
    hdr->ns = htons(rfc->our_ns++);
    hdr->nr = htons(rfc->our_nr);

    /* if the address is too large then we have a problem... */
    if (to->sa_len > sizeof(elem->addr)
        || (to->sa_family == 0 && rfc->peer_address == 0)) {
        m_freem(m);
        return EINVAL;
    }

    elem = (struct l2tp_elem *)_MALLOC(sizeof (struct l2tp_elem), M_TEMP, M_DONTWAIT);
    if (elem == 0) {
        m_freem(m);
        return ENOMEM;
    }

    elem->seqno = ntohs(hdr->ns);
    elem->packet = m;
    if (to->sa_family)
        bcopy(to, elem->addr, to->sa_len);
    else
        bcopy(rfc->peer_address, elem->addr, rfc->peer_address->sa_len);
    
    if (TAILQ_EMPTY(&rfc->send_queue)) {			/* first on queue ? */
        rfc->retry_count = 0;
        rfc->retrans_time_remain = rfc->initial_timeout;
    }
    TAILQ_INSERT_TAIL(&rfc->send_queue, elem, next);
    if (SEQ_LT(elem->seqno, rfc->peer_nr + rfc->peer_window)) {	/* within window ?  - send it */
        rfc->state &= ~L2TP_STATE_NEW_SEQUENCE;			/* disable sending of ack - piggybacked on this packet */
        return l2tp_rfc_output_queued(rfc, elem);
    } 
    
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_int16_t l2tp_rfc_output_data(struct l2tp_rfc *rfc, struct mbuf *m)
{
    struct l2tp_header		*hdr;
    struct mbuf 		*m0;
    u_int16_t 			len, hdr_length;
    u_int16_t			flags;

    len = 0;
    for (m0 = m; m0 != 0; m0 = m0->m_next)
        len += m0->m_len;

    hdr_length = L2TP_DATA_HDR_SIZE + (rfc->flags & L2TP_FLAG_PEER_SEQ_REQ ? 4 : 0);
                
    M_PREPEND(m, hdr_length, M_WAIT);
    if (m == 0)
        return ENOBUFS;
    hdr = mtod(m, struct l2tp_header *);
    bzero(hdr, hdr_length);
    
    flags = L2TP_FLAGS_L | L2TP_HDR_VERSION; 
    hdr->len = htons(len + hdr_length); 
    hdr->tunnel_id = htons(rfc->peer_tunnel_id);
    hdr->session_id = htons(rfc->peer_session_id);

    if (rfc->flags & L2TP_FLAG_PEER_SEQ_REQ) {
        flags |= L2TP_FLAGS_S;
        hdr->ns = htons(rfc->our_last_data_seq++);
        hdr->nr = htons(0);
    }
    
    hdr->flags_vers = htons(flags);

    return l2tp_udp_output(rfc->socket, m, rfc->peer_address);
}

/* -----------------------------------------------------------------------------
    send a queued control message
----------------------------------------------------------------------------- */
int l2tp_rfc_output_queued(struct l2tp_rfc *rfc, struct l2tp_elem *elem)
{
    struct mbuf 	*dup;
    struct l2tp_header	*hdr;

    dup = m_copy(elem->packet, 0, M_COPYALL);
    if (dup == 0) 
        return ENOBUFS;
   
    hdr = mtod(dup, struct l2tp_header*);
    hdr->nr = htons(rfc->our_nr); 
    
    return l2tp_udp_output(rfc->socket, dup, (struct sockaddr *)elem->addr);
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_int16_t l2tp_handle_data(struct l2tp_rfc *rfc, struct mbuf *m, struct sockaddr *from, 
    u_int16_t flags, u_int16_t len, u_int16_t tunnel_id, u_int16_t session_id)
{
    struct l2tp_header 		*hdr = mtod(m, struct l2tp_header*);
    u_int16_t 			*p, ns, hdr_length;

    //log(LOG_INFO, "handle_data, rfc = 0x%x, from 0x%x, peer address = 0x%x, our tunnel id = %d, tunnel id = %d, our session id = %d, session id = %d\n", rfc, from, rfc->peer_address, rfc->our_tunnel_id, tunnel_id, rfc->our_session_id, session_id);    
    
    // check the tunnel ID and session ID as well as the peer address
    // to determine which client the packet belongs to
    if (rfc->our_tunnel_id == tunnel_id
        && rfc->our_session_id == session_id
        && rfc->peer_address
        && !l2tp_rfc_compare_address(rfc->peer_address, from)) {
                        
        if (flags & L2TP_FLAGS_L) {			/* len field present */
            p = &hdr->ns;            
            hdr_length = L2TP_DATA_HDR_SIZE;
        }
        else {		
            p = &hdr->session_id;            
            hdr_length = L2TP_DATA_HDR_SIZE - 2;
        }

        if (flags & L2TP_FLAGS_S) {			/* packet has sequence numbers */
            ns = ntohs(*p);
            p += 2;					/* skip sequence fields */
            hdr_length += 4;
            if (SEQ_GT(ns, rfc->peer_last_data_seq)) {
                if (rfc->peer_last_data_seq + 1 == ns)
                    rfc->peer_last_data_seq++;
                else 
                    (*rfc->eventcb)(rfc->host, L2TP_EVT_INPUTERROR, 0);
            } 
            else 
                goto dropit;
        }

        if (flags & L2TP_FLAGS_O) 			/* payload is at offset in the packet */
            hdr_length += (2 + ntohs(*p));
        
        /* data packet are given up without header */
        m_adj(m, hdr_length);				/* remove the header and send it up to PPP */
        (*rfc->inputcb)(rfc->host, m, 0, 0);

        return 1;
    }

    return 0;

dropit:
    m_freem(m);
    return 1;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_int16_t l2tp_handle_control(struct l2tp_rfc *rfc, struct mbuf *m, struct sockaddr *from, 
    u_int16_t flags, u_int16_t len, u_int16_t tunnel_id, u_int16_t session_id)
{
    struct l2tp_header 		*hdr = mtod(m, struct l2tp_header*);
    struct l2tp_elem 	*elem, *new_elem;
    u_int16_t			buf_full;

    //log(LOG_INFO, "handle_control, rfc = 0x%x, from 0x%x, peer address = 0x%x, our tunnel id = %d, tunnel id = %d, our session id = %d, session id = %d\n", rfc, from, rfc->peer_address, rfc->our_tunnel_id, tunnel_id, rfc->our_session_id, session_id);    
    
    // check tunnel ID as well as the peer address
    // to determine which client the packet belongs to
    if (rfc->our_tunnel_id == tunnel_id) {
            if (rfc->peer_address) {
                if (l2tp_rfc_compare_address(rfc->peer_address, from)) 
                    goto dropit;
            }
            
            if ((flags & L2TP_FLAGS_S == 0)			/* check for valid flags */
                || (flags & L2TP_FLAGS_L == 0)
                || (flags & L2TP_FLAGS_O != 0))     
                    goto dropit;

            if (m->m_len < len)					/* check the length */
                goto dropit;			
            m->m_len = len;					/* remove padding - plugin uses datagram len */
            m->m_pkthdr.len = len;

            if (tunnel_id == 0) {
                /* receive a packet on a generic listening connection and queue it */
                if (ntohs(hdr->ns) != 0)	/* first control packet for a connection ? */
                    goto dropit;
                new_elem = (struct l2tp_elem *)_MALLOC(sizeof (struct l2tp_elem), M_TEMP, M_DONTWAIT);
                if (new_elem == 0)
                    goto dropit;
                    
                if ((new_elem->packet = m_copy(m, 0, M_COPYALL)) == 0) {
                    _FREE(new_elem, M_TEMP);
                    goto dropit;
                }
                new_elem->seqno = 0;
                bcopy(from, new_elem->addr, from->sa_len);
                TAILQ_INSERT_TAIL(&rfc->recv_queue, new_elem, next);	/* queue copy */

                if ((*rfc->inputcb)(rfc->host, m, from, 0)) {		/* send up to call socket */                        
                    TAILQ_REMOVE(&rfc->recv_queue, new_elem, next);	/* remove the packet from the queue */
                    m_freem(new_elem->packet);
                    _FREE(new_elem, M_TEMP);
                    goto dropit;
                }
                
                return 1;
            }
            
            /*									
             * the following code is only exececuted when tunnel ID is not zero 
             * i.e. not a call socket	
             */					
            
            /* clear packets being ack'd by peer */
            if (SEQ_GT(ntohs(hdr->nr), rfc->peer_nr))	
                l2tp_rfc_handle_ack(rfc, (int16_t)(ntohs(hdr->nr)));			

            if (len == L2TP_CNTL_HDR_SIZE)				/* ZLB ACK */
                goto dropit;

            if (SEQ_GT(ntohs(hdr->ns), rfc->our_nr)) {			/* out of order - need to queue it */	
                //log(LOG_INFO, "L2TP out of order message reveived seq#=%d\n", ntohs(hdr->ns));
                TAILQ_FOREACH(elem, &rfc->recv_queue, next) {
                    if (ntohs(hdr->ns) == elem->seqno)	
                        goto dropit;					/* already queued - drop it */
                    if (SEQ_GT(ntohs(hdr->ns), elem->seqno))
                        break;
                }
                //log(LOG_INFO, "L2TP queing out of order message\n");
                new_elem = (struct l2tp_elem *)_MALLOC(sizeof (struct l2tp_elem), M_TEMP, M_DONTWAIT);
                if (new_elem == 0)
                    goto dropit;
                new_elem->packet = m;
                new_elem->seqno = ntohs(hdr->ns);
                bcopy(from, new_elem->addr, from->sa_len);
                if (elem)
                    TAILQ_INSERT_AFTER(&rfc->recv_queue, elem, new_elem, next);
                else
                    TAILQ_INSERT_HEAD(&rfc->recv_queue, new_elem, next);   
            } else if (SEQ_LT(ntohs(hdr->ns), rfc->our_nr)) {
                //log(LOG_INFO, "L2TP dropping message already received seq#=%d\n", ntohs(hdr->ns));
                rfc->state |= L2TP_STATE_NEW_SEQUENCE;		/* its a dup thats already been ack'd - drop it and ack */
                goto dropit;					
            } else {						/* packet we are waiting for */
                                                                        
                /* control packets are given up with l2tp header */

                if (rfc->state & L2TP_STATE_FREEING)
                    m_freem(m);
                else if ((*rfc->inputcb)(rfc->host, m, from, 1))
                    goto dropit;
                   
                rfc->our_nr++;
                rfc->state |= L2TP_STATE_NEW_SEQUENCE;		/* sent up - ack it */
                
                /*
                    * now check for other packets on the queue that can be sent up.
                    * if the host queue is full and input fails
                    * only the packets sent up are ack'd and the remaining are dropped
                    * from the queue
                    */
                buf_full = 0;
                while (elem = TAILQ_FIRST(&rfc->recv_queue)) {
                    if (buf_full) {						/* host buffer is full - empty the queue */
                        m_freem(elem->packet);
                        TAILQ_REMOVE(&rfc->recv_queue, elem, next);
                        _FREE(elem, M_TEMP);
                    } else if (elem->seqno == rfc->our_nr) {		/* another packet to send up */

                        if (rfc->state & L2TP_STATE_FREEING) {
                            m_freem(elem->packet);
                            rfc->our_nr++;
                        }
                        else if ((*rfc->inputcb)(rfc->host, elem->packet, (struct sockaddr *)elem->addr, 1)) {
                            buf_full = 1;
                            m_freem(elem->packet);
                        }
                        else 
                            rfc->our_nr++;
                        TAILQ_REMOVE(&rfc->recv_queue, elem, next);
                        _FREE(elem, M_TEMP);
                    } else
                        break;
                }
                
                /* nothing more */
                if (!(rfc->state & L2TP_STATE_FREEING))
                    (*rfc->inputcb)(rfc->host, 0, 0, 0);
            }
                        
            return 1;
        } 
        
        /* packet not for this rfc */
        return 0;
        
dropit:
        m_freem(m);
        return 1;
}


/* -----------------------------------------------------------------------------
    compare UDP addresses
----------------------------------------------------------------------------- */
int l2tp_rfc_compare_address(struct sockaddr* addr1, struct sockaddr* addr2)
{

    if (((struct sockaddr_in*)addr1)->sin_family != ((struct sockaddr_in*)addr2)->sin_family ||
        ((struct sockaddr_in*)addr1)->sin_port != ((struct sockaddr_in*)addr2)->sin_port ||
        ((struct sockaddr_in*)addr1)->sin_addr.s_addr != ((struct sockaddr_in*)addr2)->sin_addr.s_addr)
        return 1;
    return 0;
}

/* -----------------------------------------------------------------------------
    handle incomming ack - remove ack'd packets from the control message
    send queue and send any packets that were outside the window.
----------------------------------------------------------------------------- */
void l2tp_rfc_handle_ack(struct l2tp_rfc *rfc, u_int16_t nr)
{
    struct l2tp_elem 	*elem;
    u_int16_t			old_nr = rfc->peer_nr;
    
    rfc->peer_nr = nr;
    while(elem = TAILQ_FIRST(&rfc->send_queue))
        if (SEQ_GT(nr, elem->seqno)) {
            rfc->retrans_time_remain = rfc->initial_timeout;	/* setup timeout and count */
            rfc->retry_count = 0;
            TAILQ_REMOVE(&rfc->send_queue, elem, next);
            m_freem(elem->packet);
            _FREE(elem, M_TEMP);
        } else
            break;
            
    
    if (!TAILQ_EMPTY(&rfc->send_queue)) {      
         /* check for packets that were outside the window that should now be sent */
        TAILQ_FOREACH(elem, &rfc->send_queue, next) {
            if (SEQ_GT(elem->seqno, nr + rfc->peer_window - 1))	/* outside current window ? */
                break;
            if (SEQ_GT(elem->seqno, old_nr + rfc->peer_window - 1))	/* outside previous window ? */
                l2tp_rfc_output_queued(rfc, elem);
        }
    }
}


/* -----------------------------------------------------------------------------
called from l2tp_ip when l2tp data are present
----------------------------------------------------------------------------- */
int l2tp_rfc_lower_input(struct socket *so, struct mbuf *m, struct sockaddr *from)
{
    struct l2tp_rfc  	*rfc;
    struct l2tp_header 	*hdr = mtod(m, struct l2tp_header*);
    u_int16_t 		*p;
    u_int16_t		flags, len, tunnel_id, session_id;
    
    //log(LOG_INFO, "L2TP inputdata\n");
    
    flags = ntohs(hdr->flags_vers);
    if (flags & L2TP_VERSION_MASK != L2TP_VERSION)
        goto dropit;
            
    if (flags & L2TP_FLAGS_L) {		/* len field present ? */
        len = hdr->len;
        p = &hdr->tunnel_id;
    }
    else {
        len = 0;
        p = &hdr->len;
    }

    tunnel_id = ntohs(*p++);
    session_id = ntohs(*p);

    if (flags & L2TP_FLAGS_T) {
        /* control packet */
        TAILQ_FOREACH(rfc, &l2tp_rfc_head, next)
            if ((rfc->flags & L2TP_FLAG_CONTROL)
                && l2tp_handle_control(rfc, m, from, flags, len, tunnel_id, session_id))
                    return 1;
    }
    else {
        /* data packet */
        TAILQ_FOREACH(rfc, &l2tp_rfc_head, next)
            if ((rfc->flags & L2TP_FLAG_CONTROL) == 0
                && l2tp_handle_data(rfc, m, from, flags, len, tunnel_id, session_id))
                    return 1;
    }

    //log(LOG_INFO, ">>>>>>> L2TP - no matching client found for packet\n");
    // need to drop the packet
    
dropit:
    m_freem(m);
    return 0;
}

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


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <sys/syslog.h>
#include <mach/vm_types.h>
#include <mach/kmod.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/domain.h>
#include <kern/thread.h>
#include <net/if_var.h>

#include "../../../Family/if_ppplink.h"
#include "../../../Family/ppp_domain.h"
#include "PPTP.h"
#include "pptp_proto.h"
#include "pptp_wan.h"
#include "pptp_rfc.h"
#include "ppp_mppe.h"


/* -----------------------------------------------------------------------------
Definitions
----------------------------------------------------------------------------- */


/* -----------------------------------------------------------------------------
Forward declarations
----------------------------------------------------------------------------- */
int pptp_domain_init(int);
int pptp_domain_terminate(int);

/* this function has not prototype in the .h file */
struct domain *pffinddomain(int pf);

/* -----------------------------------------------------------------------------
Globals
----------------------------------------------------------------------------- */

int 		pptp_domain_inited = 0;



/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pptp_domain_module_start(struct kmod_info *ki, void *data)
{
    boolean_t 	funnel_state;
    int		ret;

    funnel_state = thread_funnel_set(network_flock, TRUE);
    ret = pptp_domain_init(0);
    thread_funnel_set(network_flock, funnel_state);

    return ret;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pptp_domain_module_stop(struct kmod_info *ki, void *data)
{
    boolean_t 	funnel_state;
    int		ret;

    funnel_state = thread_funnel_set(network_flock, TRUE);
    ret = pptp_domain_terminate(0);
    thread_funnel_set(network_flock, funnel_state);

    return ret;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pptp_domain_init(int init_arg)
{
    int 	ret;
    struct domain *pppdomain;
    
    log(LOGVAL, "PPTP domain init\n");

    if (pptp_domain_inited)
        return(KERN_SUCCESS);

    pppdomain = pffinddomain(PF_PPP);
    if (!pppdomain) {
        log(LOGVAL, "PPTP domain init : PF_PPP domain does not exist...\n");
        return KERN_FAILURE;
    }
    
    ret = pptp_rfc_init();
    if (ret) {
        log(LOGVAL, "PPTP domain init : can't init PPTP protocol RFC, err : %d\n", ret);
        return ret;
    }
    
    ret = pptp_add(pppdomain);
    if (ret) {
        log(LOGVAL, "PPTP domain init : can't add proto to PPTP domain, err : %d\n", ret);
        pptp_rfc_dispose();
        return ret;
    }

    pptp_wan_init();

    ppp_mppe_init();

    pptp_domain_inited = 1;

    return(KERN_SUCCESS);
}


/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int pptp_domain_terminate(int term_arg)
{
    int 	ret;
    struct domain *pppdomain;
    
    log(LOGVAL, "PPTP domain terminate\n");

    if (!pptp_domain_inited)
        return(KERN_SUCCESS);

    ret = pptp_rfc_dispose();
    if (ret) {
        log(LOGVAL, "PPTP domain is in use and cannot terminate, err : %d\n", ret);
        return ret;
    }

    ret = pptp_wan_dispose();
    if (ret) {
        log(LOGVAL, "PPTP domain terminate : pptp_wan_dispose, err : %d\n", ret);
        return ret;
    }

    ppp_mppe_dispose();
    if (ret) {
        log(LOGVAL, "PPTP domain terminate : pptp_mppe_dispose, err : %d\n", ret);
        return ret;
    }

    pppdomain = pffinddomain(PF_PPP);
    if (!pppdomain) {
        // humm.. should not happen
        log(LOGVAL, "PPTP domain terminate : PF_PPP domain does not exist...\n");
        return KERN_FAILURE;
    }
    
    ret = pptp_remove(pppdomain);
    if (ret) {
        log(LOGVAL, "PPTP domain terminate : can't del proto from PPTP domain, err : %d\n", ret);
        return ret;
    }

    pptp_domain_inited = 0;
    
    return(KERN_SUCCESS);
}

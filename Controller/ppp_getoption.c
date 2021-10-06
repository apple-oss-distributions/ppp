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

/*
 * February 2000 - created.
 */

/* -----------------------------------------------------------------------------
includes
----------------------------------------------------------------------------- */

#include <string.h>
#include <stdio.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/signal.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include "ppp_msg.h"
#include "ppp_privmsg.h"

#include "../Family/ppp_domain.h"
#include "ppp_client.h"
#include "ppp_manager.h"
#include "ppp_option.h"
#include "ppp_command.h"

/* -----------------------------------------------------------------------------
definitions
----------------------------------------------------------------------------- */

#ifndef MIN
#define MIN(a, b)	((a) < (b)? (a): (b))
#endif
#ifndef MAX
#define MAX(a, b)	((a) > (b)? (a): (b))
#endif

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_long get_addr_option (struct ppp *ppp, CFStringRef entity, CFStringRef property, 
        CFDictionaryRef options, CFDictionaryRef setup, u_int32_t *opt, u_int32_t defaultval)
{
    CFDictionaryRef	dict;
    CFArrayRef		array;

    // first search in the state
    if (ppp->phase != PPP_IDLE
        && getAddressFromEntity(kSCDynamicStoreDomainState, ppp->serviceID, 
                entity, property, opt)) {
        return 1;
    }
    // then, search in the option set
    if (options
        && (dict = CFDictionaryGetValue(options, entity))
        && (CFGetTypeID(dict) == CFDictionaryGetTypeID())) {
        
        if ((array = CFDictionaryGetValue(dict, property))
            && (CFGetTypeID(array) == CFArrayGetTypeID())
            && CFArrayGetCount(array)) {
            *opt = CFStringAddrToLong(CFArrayGetValueAtIndex(array, 0));
            return 2;
        }
    }
    
    // at last, search in the setup 
    if (getAddressFromEntity(kSCDynamicStoreDomainSetup, ppp->serviceID, 
        entity, property, opt)) {
        return 3;
    }

    *opt = defaultval;
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_long get_int_option (struct ppp *ppp, CFStringRef entity, CFStringRef property,
        CFDictionaryRef options, CFDictionaryRef setup, u_int32_t *opt, u_int32_t defaultval)
{
    CFDictionaryRef	dict;

    // first search in the state
    if (ppp->phase != PPP_IDLE
        && getNumberFromEntity(kSCDynamicStoreDomainState, ppp->serviceID, 
                entity, property, opt)) {
        return 1;
    }

    // then, search in the option set
    if (options
        && (dict = CFDictionaryGetValue(options, entity))
        && (CFGetTypeID(dict) == CFDictionaryGetTypeID())
        && getNumber(dict, property, opt)) {
        return 2;
    }

    // at last, search in the setup
    if ((setup 
        && (dict = CFDictionaryGetValue(setup, entity))
        && (CFGetTypeID(dict) == CFDictionaryGetTypeID())
        && getNumber(dict, property, opt))
        || (!setup && getNumberFromEntity(kSCDynamicStoreDomainSetup, ppp->serviceID, 
        entity, property, opt))) {
        return 3;
    }

    *opt = defaultval;
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int get_str_option (struct ppp *ppp, CFStringRef entity, CFStringRef property,
        CFDictionaryRef options, CFDictionaryRef setup, u_char *opt, u_int32_t *outlen, u_char *defaultval)
{
    CFDictionaryRef	dict;
    
    // first search in the state
    if (ppp->phase != PPP_IDLE
    	&& getStringFromEntity(kSCDynamicStoreDomainState, ppp->serviceID, 
                entity, property, opt, OPT_STR_LEN)) {
        *outlen = strlen(opt);
        return 1;
    }

    // then, search in the option set
    if (options
        && (dict = CFDictionaryGetValue(options, entity))
    	&& (CFGetTypeID(dict) == CFDictionaryGetTypeID())
        && getString(dict, property, opt, OPT_STR_LEN)) {
        *outlen = strlen(opt);
        return 2;
    }
    // at last, search in the setup, only in lookinsetup flag is set
    if ((setup 
        && (dict = CFDictionaryGetValue(setup, entity))
        && (CFGetTypeID(dict) == CFDictionaryGetTypeID()) 
        && getString(dict, property, opt, OPT_STR_LEN))
        || (!setup && getStringFromEntity(kSCDynamicStoreDomainSetup, ppp->serviceID, 
        entity, property, opt, OPT_STR_LEN))) {
        *outlen = strlen(opt);
        return 3;
    }

    strcpy(opt, defaultval);
    *outlen = strlen(opt);
    return 0;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
CFTypeRef get_cf_option (CFStringRef entity, CFStringRef property, CFTypeID type, 
        CFDictionaryRef options, CFDictionaryRef setup, CFTypeRef defaultval)
{
    CFDictionaryRef	dict;
    CFTypeRef		ref;
    
    // first, search in the option set
    if (options
        && (dict = CFDictionaryGetValue(options, entity))
    	&& (CFGetTypeID(dict) == CFDictionaryGetTypeID())
        && (ref = CFDictionaryGetValue(dict, property))
    	&& (CFGetTypeID(ref) == type)) {
        return ref;
    }
    
    // then, search in the setup
    if (setup 
        && (dict = CFDictionaryGetValue(setup, entity))
        && (CFGetTypeID(dict) == CFDictionaryGetTypeID()) 
        && (ref = CFDictionaryGetValue(dict, property))
    	&& (CFGetTypeID(ref) == type)) {
        return ref;
    }

    return defaultval;
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
int ppp_getoptval(struct ppp *ppp, CFDictionaryRef opts, CFDictionaryRef setup, u_int32_t otype, void *pdata, u_int32_t *plen)
{
    u_int32_t 			lval, lval1, lval2;
    u_int32_t			*lopt = (u_int32_t *)pdata;
    u_char 			*popt = (u_char *)pdata;
    char 			str[OPT_STR_LEN], str2[OPT_STR_LEN];

    *plen = 4; // init the len for long options
    *lopt = 0; // init to zero
   
    switch (otype) {

        // DEVICE options
        case PPP_OPT_DEV_NAME:
            get_str_option(ppp, kSCEntNetInterface, kSCPropNetInterfaceDeviceName, opts, setup, popt, plen, 
                (ppp->subtype == PPP_TYPE_SERIAL) ? OPT_DEV_NAME_DEF : 
                ((ppp->subtype == PPP_TYPE_PPPoE) ? OPT_DEV_NAME_PPPoE_DEF : ""));
            break;
        case PPP_OPT_DEV_SPEED:
            *lopt = 0; 
            switch (ppp->subtype) {
                case PPP_TYPE_SERIAL:
                    get_int_option(ppp, kSCEntNetModem, kSCPropNetModemSpeed, opts, setup, lopt, OPT_DEV_SPEED_DEF);
                    break;
                case PPP_TYPE_PPPoE:
                case PPP_TYPE_PPTP:
                case PPP_TYPE_L2TP:
                    break;
            }
            break;
        case PPP_OPT_DEV_CONNECTSCRIPT:
            get_str_option(ppp, kSCEntNetModem, kSCPropNetModemConnectionScript, opts, setup, popt, plen, 
                (ppp->subtype == PPP_TYPE_SERIAL) ? OPT_DEV_CONNECTSCRIPT_DEF : "");
            break;
 
        case PPP_OPT_DEV_DIALMODE:
            *plen = 4;
            *lopt = PPP_DEV_WAITFORDIALTONE;
            str[0] = 0;
            lval = sizeof(str);
            get_str_option(ppp, kSCEntNetModem, kSCPropNetModemDialMode, opts, setup, str, &lval, "");
            str2[0] = 0;
            CFStringGetCString(kSCValNetModemDialModeIgnoreDialTone, str2, sizeof(str2), kCFStringEncodingUTF8);
            if (!strcmp(str, str2))
                *lopt = PPP_DEV_IGNOREDIALTONE;
            else {
                str2[0] = 0;
                CFStringGetCString(kSCValNetModemDialModeManual, str2, sizeof(str2), kCFStringEncodingUTF8);
                if (!strcmp(str, str2))
                    *lopt = PPP_DEV_MANUALDIAL;
            }
            break;

        // COMM options
       case PPP_OPT_DEV_CONNECTSPEED:
            switch (ppp->subtype) {
                case PPP_TYPE_SERIAL:
                    get_int_option(ppp, kSCEntNetModem, kSCPropNetModemConnectSpeed, opts, setup, lopt, 0);
                    break;
                case PPP_TYPE_PPPoE:
                case PPP_TYPE_PPTP:
                case PPP_TYPE_L2TP:
                    break;
            }
            break;
        case PPP_OPT_COMM_TERMINALMODE:
            *lopt = OPT_COMM_TERMINALMODE_DEF; 
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPCommDisplayTerminalWindow, opts, setup, &lval, 0);
            if (lval) 
                *lopt = PPP_COMM_TERM_WINDOW;
            else {
                get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPCommUseTerminalScript, opts, setup, &lval, 0);
                if (lval) 
                    *lopt = PPP_COMM_TERM_SCRIPT;
            }
	    break;	    
        case PPP_OPT_COMM_TERMINALSCRIPT:
            get_str_option(ppp, kSCEntNetPPP, kSCPropNetPPPCommTerminalScript, opts, setup, popt, plen, "");
            break;
        case PPP_OPT_COMM_IDLETIMER:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPDisconnectOnIdle, opts, setup, &lval, 0);
            if (lval)
                get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPDisconnectOnIdleTimer, opts, setup, lopt, OPT_COMM_IDLETIMER_DEF);
            break;
        case PPP_OPT_COMM_SESSIONTIMER:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPUseSessionTimer, opts, setup, &lval, 0);
            if (lval)
                get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPSessionTimer, opts, setup, lopt, OPT_COMM_SESSIONTIMER_DEF);
            break;
        case PPP_OPT_COMM_REMINDERTIMER:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPIdleReminder,opts, setup,  &lval, 0);
            if (lval)
                get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPIdleReminderTimer, opts, setup, lopt, OPT_COMM_REMINDERTIMER_DEF);
            break;
        case PPP_OPT_COMM_REMOTEADDR:
            get_str_option(ppp, kSCEntNetPPP, kSCPropNetPPPCommRemoteAddress, opts, setup, popt, plen, "");
            break;
        case PPP_OPT_COMM_CONNECTDELAY:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPCommConnectDelay, opts, setup, lopt, OPT_COMM_CONNECTDELAY_DEF);
            break;
            
        // LCP options
        case PPP_OPT_LCP_HDRCOMP:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPCompressionACField, opts, setup, &lval, OPT_LCP_PCOMP_DEF);
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPCompressionPField, opts, setup, &lval1, OPT_LCP_ACCOMP_DEF);
            *lopt = lval + (lval1 << 1); 
            break;
        case PPP_OPT_LCP_MRU:
            switch (ppp->subtype) {
                case PPP_TYPE_PPPoE:
                    lval = OPT_LCP_MRU_PPPoE_DEF;
                    break;
                case PPP_TYPE_PPTP:
                    lval = OPT_LCP_MRU_PPTP_DEF;
                    break;
                case PPP_TYPE_L2TP:
                    lval = OPT_LCP_MRU_L2TP_DEF; 
		    break;
                default:
                    lval = OPT_LCP_MRU_DEF;
            }
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPMRU, opts, setup, lopt, lval);
            break;
        case PPP_OPT_LCP_MTU:
            switch (ppp->subtype) {
                case PPP_TYPE_PPPoE:
                    lval = OPT_LCP_MTU_PPPoE_DEF;
                    break;
                case PPP_TYPE_PPTP:
                    lval = OPT_LCP_MTU_PPTP_DEF;
                    break;
                case PPP_TYPE_L2TP:
                    lval = OPT_LCP_MTU_L2TP_DEF;
                    break;
                default:
                    lval = OPT_LCP_MTU_DEF;
            }
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPMTU, opts, setup, lopt, lval);
            break;
        case PPP_OPT_LCP_RCACCM:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPReceiveACCM, opts, setup, lopt, OPT_LCP_RCACCM_DEF);
            break;
        case PPP_OPT_LCP_TXACCM:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPTransmitACCM, opts, setup, lopt, OPT_LCP_TXACCM_DEF);
            break;
        case PPP_OPT_LCP_ECHO:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPEchoEnabled, opts, setup, &lval, 0);
            if (lval) {
                get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPEchoInterval, opts, setup, &lval1, OPT_LCP_ECHOINTERVAL_DEF);
                get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPLCPEchoFailure, opts, setup, &lval2, OPT_LCP_ECHOFAILURE_DEF);
                *lopt = (lval1 << 16) + lval2; 
            }
            break;

        // AUTH options
        case PPP_OPT_AUTH_PROTO:
            *lopt = OPT_AUTH_PROTO_DEF;
            // XXX To be fixed
            break;
        case PPP_OPT_AUTH_NAME:
             get_str_option(ppp, kSCEntNetPPP, kSCPropNetPPPAuthName, opts, setup, popt, plen, "");
            break;
        case PPP_OPT_AUTH_PASSWD:
            get_str_option(ppp, kSCEntNetPPP, kSCPropNetPPPAuthPassword, opts, setup, popt, plen, "");
            // don't return the actual pasword.
            // instead, return len = 1 if password is known, len = 0 if password is unknown
            if (*plen) {
                popt[0] = '*';
                *plen = 1;
            }
            break;

            // IPCP options
        case PPP_OPT_IPCP_HDRCOMP:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPIPCPCompressionVJ, opts, setup, lopt, OPT_IPCP_HDRCOMP_DEF);
            break;
        case PPP_OPT_IPCP_LOCALADDR:
             get_addr_option(ppp, kSCEntNetIPv4, kSCPropNetIPv4Addresses, opts, setup, lopt, 0);
           break;
        case PPP_OPT_IPCP_REMOTEADDR:
            get_addr_option(ppp, kSCEntNetIPv4, kSCPropNetIPv4DestAddresses, opts, setup, lopt, 0);
            break;

           // MISC options
        case PPP_OPT_LOGFILE:
            // Note: this option is not taken from the user options
             get_str_option(ppp, kSCEntNetPPP, kSCPropNetPPPLogfile, 0 /* opts */, setup, popt, plen, "");
             if (popt[0] && popt[0] != '/') {
                lval = strlen(DIR_LOGS);
                strncpy(popt + lval, popt, *plen);
                strncpy(popt, DIR_LOGS, lval);
                *plen += lval;
             }
            break;
        case PPP_OPT_ALERTENABLE:
            get_int_option(ppp, kSCEntNetPPP, CFSTR("AlertEnable"), opts, setup, lopt, 0xFFFFFFFF);
            break;
        case PPP_OPT_DIALONDEMAND:
        case PPP_OPT_AUTOCONNECT_DEPRECATED:
            get_int_option(ppp, kSCEntNetPPP, kSCPropNetPPPDialOnDemand, opts, setup, lopt, 0);
            break;
        case PPP_OPT_SERVICEID:
            popt[0] = 0;
            CFStringGetCString(ppp->serviceID, popt, 256, kCFStringEncodingUTF8);
            *plen = strlen(popt);
            break;
        case PPP_OPT_IFNAME:
            if (ppp->ifunit != 0xFFFF) {
                sprintf(popt, "%s%d", ppp->name, ppp->ifunit);
                *plen = strlen(popt);
            }
            break;

        default:
            *plen = 0;
            return 0; // not found
    };

    return 1; // OK
}

/* -----------------------------------------------------------------------------
----------------------------------------------------------------------------- */
u_long ppp_getoption (struct client *client, struct msg *msg, void **reply)
{
    struct ppp_opt 		*opt = (struct ppp_opt *)&msg->data[MSG_DATAOFF(msg)];
    CFDictionaryRef		opts;
    struct ppp 			*ppp = ppp_find(msg);
            
    if (!ppp) {
        msg->hdr.m_len = 0;
        msg->hdr.m_result = ENODEV;
        return 0;
    }

    if (ppp->phase != PPP_IDLE)
        // take the active user options
        opts = ppp->connectopts ? ppp->connectopts : ppp->needconnectopts; 
    else 
        // not connected, get the client options that will be used.
        opts = client_findoptset(client, ppp->serviceID);
    
    *reply = CFAllocatorAllocate(NULL, sizeof(struct ppp_opt_hdr) + OPT_STR_LEN + 1, 0); // XXX should allocate len dynamically
    if (*reply == 0) {
        msg->hdr.m_result = ENOMEM;
        msg->hdr.m_len = 0;
        return 0;
    }
    bcopy(opt, *reply, sizeof(struct ppp_opt_hdr));
    opt = (struct ppp_opt *)*reply;
    
    if (!ppp_getoptval(ppp, opts, 0, opt->o_type, &opt->o_data[0], &msg->hdr.m_len)) {
        msg->hdr.m_len = 0;
        msg->hdr.m_result = EOPNOTSUPP;
        CFAllocatorDeallocate(NULL, *reply);
        return 0;
    }
    
    msg->hdr.m_result = 0;
    msg->hdr.m_len += sizeof(struct ppp_opt_hdr);
    return 0;
}

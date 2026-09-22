/* SPDX-License-Identifier: MIT */
#ifndef SIP_RESP_H
#define SIP_RESP_H

#include <stddef.h>

/* Builds the response to one SIP request (stateless):
 *   OPTIONS   -> 200 OK with Allow (keep-alive / health pings from the P-CSCF)
 *   REGISTER  -> 200 OK echoing Contact and Expires (a registrar stub)
 *   other     -> 501 Not Implemented
 * Via, From, Call-ID and CSeq are copied; To gets a tag if it has none.
 * Returns the response length, 0 if the request is malformed (drop it),
 * or 0 if it is a response rather than a request. */
size_t sip_respond(const char *req, size_t len, const char *to_tag,
		   char *out, size_t cap, int *status);

#endif

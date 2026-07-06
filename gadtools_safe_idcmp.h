#ifndef MINIAMP3_GADTOOLS_SAFE_IDCMP_H
#define MINIAMP3_GADTOOLS_SAFE_IDCMP_H

#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <proto/gadtools.h>

/*
 * RC1 guard for the GadTools frontend.
 *
 * amiga_mp3gui.c reads IntuiMessage->IAddress as a struct Gadget before it
 * has checked the IDCMP class.  That is only safe for real gadget messages;
 * messages such as IDCMP_CLOSEWINDOW, IDCMP_REFRESHWINDOW and IDCMP_VANILLAKEY
 * do not carry a Gadget pointer there.  On real Amiga systems this can wedge
 * the UI when the Internet Radio window is opened and then closed.
 *
 * Keep gadget-bearing messages untouched, but null IAddress for the window/key
 * messages used by the radio dialog so stale/non-gadget pointers are never
 * dereferenced as struct Gadget.
 */
static struct IntuiMessage *
MiniAmp3SafeGTGetIMsg(struct MsgPort *port)
{
	struct IntuiMessage *msg = GT_GetIMsg(port);
	if (msg) {
		switch (msg->Class) {
		case IDCMP_CLOSEWINDOW:
		case IDCMP_REFRESHWINDOW:
		case IDCMP_VANILLAKEY:
			msg->IAddress = NULL;
			break;
		default:
			break;
		}
	}
	return msg;
}

#ifdef GT_GetIMsg
#undef GT_GetIMsg
#endif
#define GT_GetIMsg(port) MiniAmp3SafeGTGetIMsg(port)
#endif /* AMIGA_M68K */

#endif /* MINIAMP3_GADTOOLS_SAFE_IDCMP_H */

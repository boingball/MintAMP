#ifndef MINIAMP3_PROTO_GADTOOLS_SAFE_WRAPPER_H
#define MINIAMP3_PROTO_GADTOOLS_SAFE_WRAPPER_H

/*
 * MiniAMP3 GadTools RC1 guard.
 *
 * The GadTools frontend reads IntuiMessage->IAddress as a struct Gadget before
 * it switches on the IDCMP class.  That is valid for IDCMP_GADGETUP, but not
 * for the Internet Radio window close gadget path: IDCMP_CLOSEWINDOW is a
 * window message, not a gadget message.  On real Amiga systems a stale/non-
 * gadget IAddress there can wedge the UI when the radio window is opened and
 * then closed without playing anything.
 *
 * We sit in front of the compiler's real <proto/gadtools.h> via the repo's
 * existing -Ipub include path, then wrap GT_GetIMsg() so close/refresh/key
 * messages cannot be misread as gadget events.  Gadget messages are left
 * untouched.
 */
#include_next <proto/gadtools.h>

#if defined(AMIGA_M68K) && !defined(MINIAMP3_DISABLE_SAFE_GTIMSG)
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
#endif /* AMIGA_M68K && !MINIAMP3_DISABLE_SAFE_GTIMSG */

#endif /* MINIAMP3_PROTO_GADTOOLS_SAFE_WRAPPER_H */

#ifndef MINIAMP3_PROTO_GADTOOLS_SAFE_WRAPPER_H
#define MINIAMP3_PROTO_GADTOOLS_SAFE_WRAPPER_H

/*
 * MiniAMP3 GadTools RC1 guard.
 *
 * The GadTools frontend reads IntuiMessage->IAddress as a struct Gadget before
 * it switches on the IDCMP class.  That is valid for IDCMP_GADGETUP, but not
 * for the Internet Radio window close gadget path: IDCMP_CLOSEWINDOW is a
 * window message, not a gadget message.
 *
 * The radio dialog also freezes during close teardown on real hardware, even
 * with no playback active.  Its gadget IDs live in the 300..315 range, so the
 * FreeGadgets() guard below skips freeing that dialog's gadget chain only.  The
 * app clears its pointers after close, so this becomes a small diagnostic/RC1
 * leak instead of an Amiga UI lockup.  Main and playlist gadgets are outside
 * this ID range and still free normally.
 */
#include_next <proto/gadtools.h>

#if defined(AMIGA_M68K) && !defined(MINIAMP3_DISABLE_SAFE_GTIMSG)
static int
MiniAmp3LooksLikeRadioGadToolsChain(struct Gadget *gad)
{
	return gad && gad->GadgetID >= 300 && gad->GadgetID <= 315;
}

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

static VOID
MiniAmp3SafeFreeGadgets(struct Gadget *gad)
{
	if (MiniAmp3LooksLikeRadioGadToolsChain(gad))
		return;
	FreeGadgets(gad);
}

#ifdef GT_GetIMsg
#undef GT_GetIMsg
#endif
#define GT_GetIMsg(port) MiniAmp3SafeGTGetIMsg(port)

#ifdef FreeGadgets
#undef FreeGadgets
#endif
#define FreeGadgets(gad) MiniAmp3SafeFreeGadgets(gad)
#endif /* AMIGA_M68K && !MINIAMP3_DISABLE_SAFE_GTIMSG */

#endif /* MINIAMP3_PROTO_GADTOOLS_SAFE_WRAPPER_H */

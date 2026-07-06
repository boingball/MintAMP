#ifndef MINIAMP3_PROTO_INTUITION_SAFE_WRAPPER_H
#define MINIAMP3_PROTO_INTUITION_SAFE_WRAPPER_H

/*
 * MiniAMP3 GadTools RC1 diagnostic guard.
 *
 * The Internet Radio dialog freezes on close even with no playback active, so
 * the failure is in the window teardown path rather than the radio stream path.
 * The radio dialog gadget IDs live in the 300..315 range.  If the close hang is
 * caused by RemoveGList() walking a GadTools ListView/string gadget chain during
 * teardown, skip that detach for the radio dialog only and let CloseWindow()
 * dispose of the window structure.  FreeGadgets() is guarded separately in the
 * GadTools wrapper to avoid freeing a still-attached radio gadget chain.
 *
 * This intentionally does not affect the main MiniAMP3 window or playlist
 * window, whose gadget IDs are outside this range.
 */
#include_next <proto/intuition.h>

#if defined(AMIGA_M68K) && !defined(MINIAMP3_DISABLE_RADIO_CLOSE_QUARANTINE)
static int
MiniAmp3LooksLikeRadioGadgetChain(struct Gadget *gad)
{
	return gad && gad->GadgetID >= 300 && gad->GadgetID <= 315;
}

static UWORD
MiniAmp3SafeRemoveGList(struct Window *win, struct Gadget *gad, WORD num)
{
	if (win && MiniAmp3LooksLikeRadioGadgetChain(gad))
		return 0;
	return RemoveGList(win, gad, num);
}

#ifdef RemoveGList
#undef RemoveGList
#endif
#define RemoveGList(win, gad, num) MiniAmp3SafeRemoveGList((win), (gad), (num))
#endif /* AMIGA_M68K && !MINIAMP3_DISABLE_RADIO_CLOSE_QUARANTINE */

#endif /* MINIAMP3_PROTO_INTUITION_SAFE_WRAPPER_H */

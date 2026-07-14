/* crt_freeall_probe.c -- DEBUG-ONLY diagnostic replacement of libnix ___free_all
 * ===========================================================================
 *
 * Purpose
 * -------
 * Capture the EXACT allocation node that libnix's ___free_all hands to Exec
 * FreeMem when AmigaOS raises the recoverable close-time alert
 *
 *     01000009  AN_FreeTwice     (same block handed to FreeMem twice)
 *     0100000F  AN_BadFreeAddr   (FreeMem given an address with no mem header)
 *
 * The alert fires on the main/CRT task inside ___free_all, AFTER main()
 * returns and after every application-side free has already run and been proven
 * balanced.  Capturing the offending block therefore requires instrumenting
 * ___free_all itself -- not the application allocator.
 *
 * Ground truth for this linked RC1 executable (reverse-engineered from the
 * shipped binary, supplied by the maintainer):
 *
 *     ___free_all walks three malloc list heads (addresses in the un-modified
 *     RC1 binary: 0x0020c8b4, 0x0020c8b8, 0x0020c8c8) and for each node:
 *         next            = *(void  **)node
 *         allocationBase  = (unsigned char *)node - 4
 *         allocationBytes = *(unsigned long *)(node - 4)
 *         FreeMem(allocationBase, allocationBytes)
 *
 * This file reproduces that traversal EXACTLY (same three heads, same order,
 * same node arithmetic, same FreeMem base/size, same head-drain cleanup) and,
 * for every node, before the FreeMem, stores the node/next/base/size/task into
 * a fixed static record (magic 'FREA', state ABOUT_TO_FREE) and echoes one line
 * over the non-allocating Exec RawPutChar raw-debug port, flipping the record
 * to FREE_RETURNED only after FreeMem returns.  The record left ABOUT_TO_FREE,
 * or the last raw-debug line before the requester, names the offending node,
 * base and size.  No malloc/calloc/realloc/free, stdio, buffered output, file
 * or requester is used inside the traversal.
 *
 * This is a DIAGNOSTIC ONLY.  It does not fix anything and does not claim to.
 *
 * ---------------------------------------------------------------------------
 * Layout stability (the whole point of this revision)
 * ---------------------------------------------------------------------------
 * The complete wrapper, the complete traversal and gFreeAllProbe are compiled
 * UNCONDITIONALLY.  There is NO FREEALL_PROBE_ARMED build variant and nothing
 * in this file is conditionally compiled on the head values.  So the object
 * produced for a "discovery" build and for an "armed" build is byte-identical.
 *
 * The three head addresses are read from the linker-defined ABSOLUTE symbols
 * __freeall_head0/1/2, supplied at link time via --defsym (see Makefile.amiga).
 * Because those are absolute symbols, only their VALUES change between builds --
 * never a section size and never an instruction width (the loads that read
 * them are 32-bit absolute relocations, fixed-width regardless of the value).
 * Consequently libnix's own list heads sit at IDENTICAL addresses in the
 * discovery and armed binaries, so an address discovered by disassembling the
 * discovery binary is still valid in the armed binary.  (The classic pass-one/
 * pass-two hazard -- arming changing the text size and shifting the very
 * addresses discovered in pass one -- cannot occur here.)
 *
 * Discovery vs armed is therefore purely a LINK-time distinction:
 *   Discovery:  __freeall_head0/1/2 == 0.  At run time the wrapper sees a zero
 *               head and calls the genuine ___free_all verbatim (transparent).
 *               This build exists only to be disassembled for the real heads.
 *   Armed:      __freeall_head0/1/2 == the verified real head addresses.  The
 *               wrapper performs the instrumented traversal.
 * The Makefile refuses to arm (build error) unless all three head values are
 * given and non-zero.
 *
 * ---------------------------------------------------------------------------
 * How it is linked in (see Makefile.amiga)
 * ---------------------------------------------------------------------------
 *   Discovery:  make -f Makefile.amiga sslgui DEBUG=1 FREEALL_PROBE=1
 *   Verify the real heads in that binary, e.g.
 *       m68k-amigaos-objdump -d miniamp3 \
 *         | sed -n '/<__real____free_all>:/,/rts/p'
 *       m68k-amigaos-nm -n miniamp3        # locate the three list-head globals
 *   Armed:      make -f Makefile.amiga sslgui DEBUG=1 FREEALL_PROBE=1 \
 *                 FREEALL_ARMED=1 FREEALL_HEAD0=0xXXXXXXXX \
 *                 FREEALL_HEAD1=0xYYYYYYYY FREEALL_HEAD2=0xZZZZZZZZ
 *
 * --wrap redirects the CRT exit-cleanup reference to ___free_all to the
 * __wrap____free_all defined here, keeping the genuine implementation reachable
 * as __real____free_all.  Nothing else is wrapped: malloc, calloc, realloc,
 * free and Exec FreeMem keep their normal implementations.
 *
 * ---------------------------------------------------------------------------
 * Required final-binary evidence (see the summary; not runnable without the
 * Bebbo m68k toolchain, which is absent in the environment that wrote this):
 * ---------------------------------------------------------------------------
 *   nm:       __wrap____free_all, __real____free_all, gFreeAllProbe and
 *             __freeall_head0/1/2 all present (the last three as absolute
 *             symbols whose values are the armed head addresses).
 *   objdump:  the CRT exit-cleanup call site resolves to __wrap____free_all
 *             (NOT ___free_all) -- prove the wrap actually intercepted it;
 *             the three head addresses read by __real____free_all equal the
 *             three head addresses read by __wrap____free_all equal the three
 *             --defsym values.
 *
 * ---------------------------------------------------------------------------
 * Reading the result while the recoverable requester is on screen
 * ---------------------------------------------------------------------------
 * The requester is RECOVERABLE, so the process is still alive when it appears.
 *   * Serial/raw-debug (Sushi/Sashimi/serial capture/WinUAE debug console): the
 *     last "FREEALL seq=... ABOUT_TO_FREE" line with no following
 *     "FREEALL seq=... FREE_RETURNED" is the offending node (recoverable frees
 *     DO return, so for those the culprit is the last ABOUT_TO_FREE line before
 *     the guru).
 *   * Static record via a debugger/monitor: read the global symbol gFreeAllProbe
 *     (address from  m68k-amigaos-nm minimp3r | grep gFreeAllProbe ) while the
 *     requester is up; the 0x46524541 ('FREA') magic marks the struct and a
 *     state of 1 (ABOUT_TO_FREE) marks a free that did not return.
 * This does NOT rely on the static record surviving a reboot.
 */

/* Non-empty translation unit even when the probe is compiled out. */
typedef int crt_freeall_probe_unit;

#if defined(FREEALL_PROBE) && defined(AMIGA_M68K)

#include <exec/types.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#ifndef FREEALL_HEAD_COUNT
#define FREEALL_HEAD_COUNT 3
#endif

/* Safety bound: only reachable on a corrupted/looping list (itself the fault
 * under investigation); set high enough never to clip a healthy heap. */
#ifndef FREEALL_MAX_NODES
#define FREEALL_MAX_NODES 1000000UL
#endif

#ifndef FREEALL_SEEN_LIMIT
#define FREEALL_SEEN_LIMIT 256
#endif

#ifndef FREEALL_MAX_SENSIBLE_SIZE
#define FREEALL_MAX_SENSIBLE_SIZE 0x10000000UL
#endif

/* The genuine libnix ___free_all, reachable through --wrap.  Called verbatim in
 * discovery (zero heads); referenced in every build so it -- and its symbol --
 * stay in the link for the required disassembly proof. */
extern void FreeAll_Real(void) __asm__("__real____free_all");

/* Our replacement; the linker substitutes it for every ___free_all reference. */
void FreeAll_Wrap(void) __asm__("__wrap____free_all");

/* Linker-defined ABSOLUTE symbols (Makefile: -Wl,--defsym,__freeall_headN=..).
 * Their ADDRESS is the list-head address; only the value changes per build. */
extern char __freeall_head0[];
extern char __freeall_head1[];
extern char __freeall_head2[];

#if defined(FREEALL_DANGEROUS_LEAK_CRT_ON_EXIT)
/* Deliberately dangerous proof-only mode: never enable in production. */
#endif

/* ------------------------------------------------------------------------- */
/* Fixed static record (no allocation, ever).                                */
/* ------------------------------------------------------------------------- */
#define FREEALL_MAGIC        0x46524541UL	/* 'FREA' */
#define FREEALL_STATE_IDLE          0UL
#define FREEALL_STATE_ABOUT_TO_FREE 1UL
#define FREEALL_STATE_FREE_RETURNED 2UL

typedef struct FreeAllProbeRecord {
	unsigned long magic;		/* FREEALL_MAGIC, so a monitor can find it */
	unsigned long seq;		/* monotonic free sequence number */
	unsigned long list;		/* list index 0..FREEALL_HEAD_COUNT-1 */
	unsigned long headAddr;		/* address of that list's head */
	void         *node;		/* current list node */
	void         *next;		/* *(void **)node */
	void         *base;		/* node - 4  (address passed to FreeMem) */
	unsigned long size;		/* *(ULONG *)(node - 4)  (size to FreeMem) */
	void         *task;		/* FindTask(NULL) */
	unsigned long state;		/* ABOUT_TO_FREE -> FREE_RETURNED */
	unsigned long flags;		/* fixed-storage validation bits */
	unsigned long endMagic;		/* FREEALL_MAGIC (brackets the struct) */
} FreeAllProbeRecord;

/* Initialised (not BSS) so the magic is present before any code runs. */
FreeAllProbeRecord gFreeAllProbe = {
	FREEALL_MAGIC, 0UL, 0UL, 0UL,
	(void *)0, (void *)0, (void *)0, 0UL, (void *)0,
	FREEALL_STATE_IDLE, 0UL, FREEALL_MAGIC
};

/* Force the genuine ___free_all object (and its list-head globals) to stay in
 * the link even in the armed build, where we do not call it. */
void * volatile gFreeAllKeepReal;

#define FREEALL_FLAG_NODE_MISALIGNED  0x00000001UL
#define FREEALL_FLAG_BASE_NULL        0x00000002UL
#define FREEALL_FLAG_BASE_MISALIGNED  0x00000004UL
#define FREEALL_FLAG_SIZE_ZERO        0x00000008UL
#define FREEALL_FLAG_SIZE_ODD         0x00000010UL
#define FREEALL_FLAG_SIZE_HUGE        0x00000020UL
#define FREEALL_FLAG_NEXT_SELF        0x00000040UL
#define FREEALL_FLAG_NODE_REPEATED    0x00000080UL
#define FREEALL_FLAG_BASE_REPEATED    0x00000100UL

static void *gFreeAllSeenNodes[FREEALL_SEEN_LIMIT];
static void *gFreeAllSeenBases[FREEALL_SEEN_LIMIT];
static unsigned long gFreeAllSeenCount;

/* ------------------------------------------------------------------------- */
/* Non-allocating raw-debug output (Exec RawPutChar, LVO -516).              */
/* One byte to the serial/raw-debug port; allocates nothing.  Numbers are     */
/* formatted here, so no printf/RawDoFmt buffer is involved.                  */
/* ------------------------------------------------------------------------- */
static void probe_ch(char c)
{
	RawPutChar((UBYTE)c);
}

static void probe_str(const char *s)
{
	while (*s)
		probe_ch(*s++);
}

static void probe_hex32(unsigned long v)
{
	static const char hexd[] = "0123456789abcdef";
	int i;
	probe_ch('0');
	probe_ch('x');
	for (i = 28; i >= 0; i -= 4)
		probe_ch(hexd[(v >> i) & 0xfUL]);
}


static unsigned long freeall_seen(void **seen, unsigned long count, void *value)
{
	unsigned long i;
	for (i = 0; i < count; i++) {
		if (seen[i] == value)
			return 1UL;
	}
	return 0UL;
}

static unsigned long freeall_validation_flags(void *node, void *next, void *base, unsigned long size)
{
	unsigned long flags = 0UL;
	if (((unsigned long)node & 1UL) != 0UL)
		flags |= FREEALL_FLAG_NODE_MISALIGNED;
	if (base == (void *)0)
		flags |= FREEALL_FLAG_BASE_NULL;
	if (((unsigned long)base & 1UL) != 0UL)
		flags |= FREEALL_FLAG_BASE_MISALIGNED;
	if (size == 0UL)
		flags |= FREEALL_FLAG_SIZE_ZERO;
	if ((size & 1UL) != 0UL)
		flags |= FREEALL_FLAG_SIZE_ODD;
	if (size > FREEALL_MAX_SENSIBLE_SIZE)
		flags |= FREEALL_FLAG_SIZE_HUGE;
	if (next == node)
		flags |= FREEALL_FLAG_NEXT_SELF;
	if (freeall_seen(gFreeAllSeenNodes, gFreeAllSeenCount, node))
		flags |= FREEALL_FLAG_NODE_REPEATED;
	if (freeall_seen(gFreeAllSeenBases, gFreeAllSeenCount, base))
		flags |= FREEALL_FLAG_BASE_REPEATED;
	return flags;
}

static void probe_dec32(unsigned long v)
{
	char tmp[10];
	int n = 0;
	if (v == 0) {
		probe_ch('0');
		return;
	}
	while (v && n < (int)sizeof(tmp)) {
		tmp[n++] = (char)('0' + (int)(v % 10UL));
		v /= 10UL;
	}
	while (n > 0)
		probe_ch(tmp[--n]);
}

/* ------------------------------------------------------------------------- */
/* The replacement.  Complete traversal, always compiled.                    */
/* ------------------------------------------------------------------------- */
void FreeAll_Wrap(void)
{
	/* volatile: __freeall_headN are ABSOLUTE symbols that legitimately take the
	 * value 0 in a discovery build.  Without volatile, -O3 would fold
	 * "&symbol == 0" to false (the address of a declared object is assumed
	 * never NULL) and delete the discovery passthrough below.  Routing the
	 * addresses through volatile storage forces a real load and a real compare
	 * in every build, so the discovery/armed decision is made at run time from
	 * the linked value -- and the codegen is identical either way. */
	volatile unsigned long headAddr[FREEALL_HEAD_COUNT];
	struct Task *task;
	unsigned long li;

	/* Keep the genuine ___free_all (and its head globals) linked in. */
	gFreeAllKeepReal = (void *)&FreeAll_Real;

	headAddr[0] = (unsigned long)&__freeall_head0;
	headAddr[1] = (unsigned long)&__freeall_head1;
	headAddr[2] = (unsigned long)&__freeall_head2;

	/* Discovery build: heads are 0 (--defsym).  Run the genuine ___free_all
	 * verbatim -- never walk a zero/unknown head.  This branch is present in
	 * EVERY build, so it costs the same layout whether or not it is taken. */
	if (headAddr[0] == 0UL || headAddr[1] == 0UL || headAddr[2] == 0UL) {
		FreeAll_Real();
		return;
	}

#if defined(FREEALL_DANGEROUS_LEAK_CRT_ON_EXIT)
	probe_str("FREEALL-PROBE DANGEROUS_LEAK_CRT_ON_EXIT: deliberately leaking all remaining CRT allocations\n");
	return;
#endif

	task = FindTask((STRPTR)0);
	probe_str("FREEALL-PROBE armed task=");
	probe_hex32((unsigned long)task);
	probe_ch('\n');

	for (li = 0; li < (unsigned long)FREEALL_HEAD_COUNT; li++) {
		unsigned long headA = headAddr[li];
		void *node = *(void **)headA;	/* first node of this list */
		unsigned long guard = 0UL;

		while (node != (void *)0) {
			unsigned char *base = (unsigned char *)node - 4;
			unsigned long  size = *(unsigned long *)base;
			void          *next = *(void **)node;
			unsigned long  flags = freeall_validation_flags(node, next, (void *)base, size);

			/* --- store the fixed record FULLY before the free --- */
			++gFreeAllProbe.seq;
			gFreeAllProbe.list     = li;
			gFreeAllProbe.headAddr = headA;
			gFreeAllProbe.node     = node;
			gFreeAllProbe.next     = next;
			gFreeAllProbe.base     = (void *)base;
			gFreeAllProbe.size     = size;
			gFreeAllProbe.task     = (void *)task;
			gFreeAllProbe.state    = FREEALL_STATE_ABOUT_TO_FREE;
			gFreeAllProbe.flags    = flags;

			probe_str("FREEALL seq=");
			probe_dec32(gFreeAllProbe.seq);
			probe_str(" list=");
			probe_dec32(li);
			probe_str(" head=");
			probe_hex32(headA);
			probe_str(" node=");
			probe_hex32((unsigned long)node);
			probe_str(" next=");
			probe_hex32((unsigned long)next);
			probe_str(" base=");
			probe_hex32((unsigned long)base);
			probe_str(" size=");
			probe_dec32(size);
			probe_str(" task=");
			probe_hex32((unsigned long)task);
			probe_str(" flags=");
			probe_hex32(flags);
			probe_str(" state=ABOUT_TO_FREE\n");

			if (gFreeAllSeenCount < (unsigned long)FREEALL_SEEN_LIMIT) {
				gFreeAllSeenNodes[gFreeAllSeenCount] = node;
				gFreeAllSeenBases[gFreeAllSeenCount] = (void *)base;
				gFreeAllSeenCount++;
			}

			/* Identical base/size/order as the genuine ___free_all. */
			FreeMem((APTR)base, size);

			/* --- after the free returns, update ONLY the state --- */
			gFreeAllProbe.state = FREEALL_STATE_FREE_RETURNED;
			probe_str("FREEALL seq=");
			probe_dec32(gFreeAllProbe.seq);
			probe_str(" state=FREE_RETURNED\n");

			node = next;
			if (++guard > FREEALL_MAX_NODES) {
				probe_str("FREEALL guard-stop list=");
				probe_dec32(li);
				probe_ch('\n');
				break;
			}
		}

		/* Drain the head exactly as a completed ___free_all leaves it. */
		*(void **)headA = (void *)0;
	}

	probe_str("FREEALL-PROBE done\n");
}

#endif	/* FREEALL_PROBE && AMIGA_M68K */

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
 *               head and returns without walking unknown CRT lists.  This build
 *               exists only to be disassembled for the real heads; do not use it
 *               as a runtime cleanup/alert-capture build.
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
 *         | sed -n '/<__wrap____free_all>:/,/rts/p'
 *       m68k-amigaos-nm -n miniamp3        # locate the three list-head globals
 *   Armed:      make -f Makefile.amiga sslgui DEBUG=1 FREEALL_PROBE=1 \
 *                 FREEALL_ARMED=1 FREEALL_HEAD0=0xXXXXXXXX \
 *                 FREEALL_HEAD1=0xYYYYYYYY FREEALL_HEAD2=0xZZZZZZZZ
 *
 * The probe leaves the genuine ___free_all linked normally, then patches the
 * libnix ___EXIT_LIST__ entry for that function at application startup. Nothing
 * else is wrapped: malloc, calloc, realloc, free and Exec FreeMem keep their
 * normal implementations.
 *
 * ---------------------------------------------------------------------------
 * Required final-binary evidence (see the summary; not runnable without the
 * Bebbo m68k toolchain, which is absent in the environment that wrote this):
 * ---------------------------------------------------------------------------
 *   nm:       genuine ___free_all, FreeAllProbe_Run, FreeAllProbe_Install,
 *             gFreeAllProbe and __freeall_head0/1/2 all present.
 *   objdump:  main calls FreeAllProbe_Install; FreeAllProbe_Install scans and
 *             writes ___EXIT_LIST__; FreeAllProbe_Run emits the raw entry
 *             markers and discovery calls the saved original function pointer.
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

/* Genuine libnix ___free_all and its exit-list callback table. */
extern void LibnixFreeAll(void) __asm__("___free_all");
extern void *LibnixExitList[] __asm__("___EXIT_LIST__");

void FreeAllProbe_Install(void);
void FreeAllProbe_Run(void);

/* Linker-defined ABSOLUTE symbols (Makefile: -Wl,--defsym,__freeall_headN=..).
 * Their ADDRESS is the list-head address; only the value changes per build. */
extern char __freeall_head0[] __asm__("___freeall_head0");
extern char __freeall_head1[] __asm__("___freeall_head1");
extern char __freeall_head2[] __asm__("___freeall_head2");

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
static void (*gFreeAllOriginal)(void);
static unsigned long gFreeAllInstallReplacements;
static void *gFreeAllInstallEntry;

/* ------------------------------------------------------------------------- */
/* Non-allocating raw-debug output (Exec RawPutChar, LVO -516).              */
/* One byte to the serial/raw-debug port; allocates nothing.  Numbers are     */
/* formatted here, so no printf/RawDoFmt buffer is involved.                  */
/* ------------------------------------------------------------------------- */
static void probe_ch(char c)
{
	/* proto/exec.h does not provide an out-of-line RawPutChar symbol in
	 * every m68k-amigaos configuration, so call the Exec vector directly. */
	register unsigned long d0 __asm("d0") = (unsigned long)(UBYTE)c;
	__asm volatile ("move.l 4.w,%%a6\n\tjsr -516(%%a6)"
		: "+d"(d0)
		:
		: "a6", "cc", "memory");
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
/* Exit-list installation and replacement runner.                             */
/* ------------------------------------------------------------------------- */

typedef struct FreeAllExitEntry {
	void (*func)(void);
	unsigned long meta;
} FreeAllExitEntry;

#ifndef FREEALL_EXIT_SCAN_LIMIT
#define FREEALL_EXIT_SCAN_LIMIT 1024UL
#endif

void FreeAllProbe_Install(void)
{
	FreeAllExitEntry *entry = (FreeAllExitEntry *)LibnixExitList;
	FreeAllExitEntry *match = (FreeAllExitEntry *)0;
	void (*func)(void);
	unsigned long replacements = 0UL;
	unsigned long i;

	for (i = 0; i < FREEALL_EXIT_SCAN_LIMIT; i++) {
		func = entry[i].func;
		if (func == (void (*)(void))0)
			break;
		if (func == LibnixFreeAll) {
			match = &entry[i];
			replacements++;
		}
	}

	probe_str("FREEALL-INSTALL exitList=");
	probe_hex32((unsigned long)entry);
	probe_str(" entry=");
	probe_hex32((unsigned long)match);
	probe_str(" original=");
	probe_hex32((unsigned long)LibnixFreeAll);
	probe_str(" replacement=");
	probe_hex32((unsigned long)FreeAllProbe_Run);
	probe_str(" replacements=");
	probe_dec32(replacements);
	probe_ch('\n');

	if (replacements != 1UL) {
		probe_str("FREEALL-INSTALL-FAILED replacements=");
		probe_dec32(replacements);
		probe_ch('\n');
		gFreeAllInstallReplacements = replacements;
		return;
	}

	gFreeAllOriginal = match->func;
	gFreeAllInstallEntry = (void *)match;
	match->func = FreeAllProbe_Run;
	gFreeAllInstallReplacements = replacements;
}

void FreeAllProbe_Run(void)
{
	/* volatile: __freeall_headN are ABSOLUTE symbols that legitimately take the
	 * value 0 in a discovery build.  Without volatile, -O3 would fold
	 * "&symbol == 0" to false (the address of a declared object is assumed
	 * never NULL) and delete the discovery passthrough below. */
	volatile unsigned long headAddr[FREEALL_HEAD_COUNT];
	struct Task *task;
	unsigned long realAddr;
	unsigned long li;

	headAddr[0] = (unsigned long)&__freeall_head0;
	headAddr[1] = (unsigned long)&__freeall_head1;
	headAddr[2] = (unsigned long)&__freeall_head2;

	probe_str("FREEALL-WRAPPER-ENTER\n");

	/* Discovery build: heads are 0 (--defsym).  Delegate to the exact function
	 * pointer that FreeAllProbe_Install removed from ___EXIT_LIST__. */
	if (headAddr[0] == 0UL || headAddr[1] == 0UL || headAddr[2] == 0UL) {
		realAddr = (unsigned long)gFreeAllOriginal;
		probe_str("FREEALL-DISCOVERY-CALLING-REAL original=");
		probe_hex32(realAddr);
		probe_ch('\n');
		if (gFreeAllOriginal != (void (*)(void))0) {
			gFreeAllOriginal();
			probe_str("FREEALL-REAL-RETURNED\n");
		} else {
			probe_str("FREEALL-DISCOVERY-NO-ORIGINAL\n");
		}
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
		void *node = *(void **)headA;
		unsigned long guard = 0UL;

		while (node != (void *)0) {
			unsigned char *base = (unsigned char *)node - 4;
			unsigned long  size = *(unsigned long *)base;
			void          *next = *(void **)node;
			unsigned long  flags = freeall_validation_flags(node, next, (void *)base, size);

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

			FreeMem((APTR)base, size);

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

		*(void **)headA = (void *)0;
	}

	probe_str("FREEALL-PROBE done\n");
}

#endif	/* FREEALL_PROBE && AMIGA_M68K */

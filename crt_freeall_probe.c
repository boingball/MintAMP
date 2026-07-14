/* crt_freeall_probe.c -- libnix ___free_all compatibility replacement
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
 * This file installs a compatibility replacement for the exit-list callback.
 * It preserves the valid-node traversal order, but treats both NULL and
 * 0xffffffff as empty list-head values.  The latter is normalised to NULL
 * before any dereference, preventing the stock libnix exit path from calling
 * FreeMem(0xfffffffb, 0).  FREEALL_PROBE builds additionally store the latest
 * attempted free in gFreeAllProbe and emit non-allocating RawPutChar diagnostics.
 *
 * ---------------------------------------------------------------------------
 * Layout stability (the whole point of this revision)
 * ---------------------------------------------------------------------------
 * The three list heads are addressed symbolically as offsets from libnix's
 * _errno data symbol, matching the linked binary layout observed for this
 * libnix runtime.  No executable-absolute head addresses are required.
 *
 * Production builds install the compatibility replacement without verbose raw
 * diagnostics.  FREEALL_PROBE=1 keeps the same sentinel-safe traversal but also
 * emits installer, sentinel, per-node and final-head diagnostics.
 *
 * ---------------------------------------------------------------------------
 * How it is linked in (see Makefile.amiga)
 * ---------------------------------------------------------------------------
 *   Production: make -f Makefile.amiga sslgui DEBUG=1
 *   Diagnostic: make -f Makefile.amiga sslgui DEBUG=1 FREEALL_PROBE=1
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
 *   nm:       genuine ___free_all, LibnixFreeAllCompat_Run,
 *             LibnixFreeAllCompat_Install and gFreeAllProbe all present.
 *   objdump:  main calls LibnixFreeAllCompat_Install; LibnixFreeAllCompat_Install scans and
 *             writes ___EXIT_LIST__; LibnixFreeAllCompat_Run emits the raw entry
 *             markers in FREEALL_PROBE builds.
 *
 * ---------------------------------------------------------------------------
 * FREEALL_PROBE diagnostics
 * ---------------------------------------------------------------------------
 * Diagnostic builds keep installer, sentinel, per-node and final-head raw
 * output. Production builds retain the compatibility traversal without verbose
 * raw output.
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
#define FREEALL_MAX_NODES 65536UL
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
extern char LibnixErrno[] __asm__("_errno");
extern char __freeall_head0[] __asm__("___freeall_head0");
extern char __freeall_head1[] __asm__("___freeall_head1");
extern char __freeall_head2[] __asm__("___freeall_head2");

void LibnixFreeAllCompat_Install(void);
void LibnixFreeAllCompat_Run(void);

#define LIBNIX_FREEALL_HEAD0_OFFSET 0x0cUL
#define LIBNIX_FREEALL_HEAD1_OFFSET 0x10UL
#define LIBNIX_FREEALL_HEAD2_OFFSET 0x20UL

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
static unsigned long gFreeAllInstalled;

#if defined(FREEALL_PROBE)
#define FREEALL_DIAGNOSTICS 1
#else
#define FREEALL_DIAGNOSTICS 0
#endif

/* ------------------------------------------------------------------------- */
/* Non-allocating raw-debug output (Exec RawPutChar, LVO -516).              */
/* One byte to the serial/raw-debug port; allocates nothing.  Numbers are     */
/* formatted here, so no printf/RawDoFmt buffer is involved.                  */
/* ------------------------------------------------------------------------- */
static void probe_ch(char c)
{
#if !FREEALL_DIAGNOSTICS
	(void)c;
#else
	/* proto/exec.h does not provide an out-of-line RawPutChar symbol in
	 * every m68k-amigaos configuration, so call the Exec vector directly. */
	register unsigned long d0 __asm("d0") = (unsigned long)(UBYTE)c;
	__asm volatile ("move.l 4.w,%%a6\n\tjsr -516(%%a6)"
		: "+d"(d0)
		:
		: "d1", "a0", "a1", "a6", "cc", "memory");
#endif
}

static void probe_str(const char *s)
{
#if !FREEALL_DIAGNOSTICS
	(void)s;
#else
	while (*s)
		probe_ch(*s++);
#endif
}

static void probe_hex32(ULONG value)
{
	static const char hexd[] = "0123456789abcdef";
	ULONG v = value;
	ULONG digit;
	int shift;

	probe_ch('0');
	probe_ch('x');
	for (shift = 28; shift >= 0; shift -= 4) {
		digit = (v >> shift) & 0x0fUL;
		probe_ch(hexd[digit]);
	}
}

static void probe_hex_selftest(void)
{
	ULONG a = 0x12345678UL;
	ULONG b = 0x89abcdefUL;
	ULONG c = 0x00000001UL;
	probe_str("FREEALL-HEX-TEST-A expected=0x12345678 actual=");
	probe_hex32(a);
	probe_ch('\n');
	probe_str("FREEALL-HEX-TEST-B expected=0x89abcdef actual=");
	probe_hex32(b);
	probe_ch('\n');
	probe_str("FREEALL-HEX-TEST-C expected=0x00000001 actual=");
	probe_hex32(c);
	probe_ch('\n');
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
#define FREEALL_EXIT_SCAN_LIMIT 256UL
#endif

void LibnixFreeAllCompat_Install(void)
{
	FreeAllExitEntry *entry;
	FreeAllExitEntry *matchingEntry = (FreeAllExitEntry *)0;
	void (*func)(void);
	unsigned long entryCount = 0UL;
	unsigned long terminatorFound = 0UL;
	unsigned long matchCount = 0UL;
	ULONG exitListSnapshot;
	ULONG scanBaseSnapshot;
	ULONG entryCountSnapshot;
	ULONG terminatorSnapshot;
	ULONG matchCountSnapshot;
	ULONG matchingEntrySnapshot;
	ULONG originalSnapshot;
	ULONG replacementSnapshot;
	unsigned long i;

	probe_hex_selftest();

	{
		ULONG errnoSymbol = (ULONG)LibnixErrno;
		ULONG errnoHead0 = (ULONG)(LibnixErrno + LIBNIX_FREEALL_HEAD0_OFFSET);
		ULONG errnoHead1 = (ULONG)(LibnixErrno + LIBNIX_FREEALL_HEAD1_OFFSET);
		ULONG errnoHead2 = (ULONG)(LibnixErrno + LIBNIX_FREEALL_HEAD2_OFFSET);
		ULONG linkedHead0 = (ULONG)&__freeall_head0;
		ULONG linkedHead1 = (ULONG)&__freeall_head1;
		ULONG linkedHead2 = (ULONG)&__freeall_head2;
		probe_str("FREEALL-ADDRESS errnoSymbol=");
		probe_hex32(errnoSymbol);
		probe_str(" head0Address=");
		probe_hex32(linkedHead0);
		probe_str(" head1Address=");
		probe_hex32(linkedHead1);
		probe_str(" head2Address=");
		probe_hex32(linkedHead2);
		probe_str(" errnoHead0=");
		probe_hex32(errnoHead0);
		probe_str(" errnoHead1=");
		probe_hex32(errnoHead1);
		probe_str(" errnoHead2=");
		probe_hex32(errnoHead2);
		probe_ch('\n');
#if defined(FREEALL_REQUIRE_ERRNO_HEAD_MATCH)
		if (linkedHead0 != errnoHead0 || linkedHead1 != errnoHead1 || linkedHead2 != errnoHead2) {
			probe_str("FREEALL-HEAD-ADDRESS-MISMATCH\n");
			return;
		}
#endif
	}

	if (gFreeAllInstalled) {
		matchingEntrySnapshot = (ULONG)gFreeAllInstallEntry;
		originalSnapshot = (ULONG)gFreeAllOriginal;
		probe_str("FREEALL-INSTALL-ALREADY entry=");
		probe_hex32(matchingEntrySnapshot);
		probe_str(" original=");
		probe_hex32(originalSnapshot);
		probe_ch('\n');
		return;
	}

	entry = (FreeAllExitEntry *)((unsigned char *)LibnixExitList + sizeof(void *));

	/* Read-only scan first.  The list layout is exactly the startup _callfuncs
	 * layout: eight-byte records, callback at offset 0, NULL callback terminates. */
	for (i = 0UL; i < FREEALL_EXIT_SCAN_LIMIT; i++) {
		func = entry[i].func;
		if (func == (void (*)(void))0) {
			terminatorFound = 1UL;
			break;
		}
		entryCount++;
		if (func == LibnixFreeAll) {
			matchingEntry = &entry[i];
			matchCount++;
		}
	}

	exitListSnapshot = (ULONG)LibnixExitList;
	scanBaseSnapshot = (ULONG)entry;
	entryCountSnapshot = (ULONG)entryCount;
	terminatorSnapshot = (ULONG)terminatorFound;
	matchCountSnapshot = (ULONG)matchCount;
	matchingEntrySnapshot = (ULONG)matchingEntry;
	originalSnapshot = (ULONG)LibnixFreeAll;
	replacementSnapshot = (ULONG)LibnixFreeAllCompat_Run;

	probe_str("FREEALL-SCAN exitList=");
	probe_hex32(exitListSnapshot);
	probe_str(" scanBase=");
	probe_hex32(scanBaseSnapshot);
	probe_str(" entryCount=");
	probe_hex32(entryCountSnapshot);
	probe_str(" terminator=");
	probe_hex32(terminatorSnapshot);
	probe_str(" matchCount=");
	probe_hex32(matchCountSnapshot);
	probe_str(" matchingEntry=");
	probe_hex32(matchingEntrySnapshot);
	probe_str(" original=");
	probe_hex32(originalSnapshot);
	probe_str(" replacement=");
	probe_hex32(replacementSnapshot);
	probe_ch('\n');

	if (!terminatorFound || matchCount != 1UL || matchingEntry == (FreeAllExitEntry *)0) {
		terminatorSnapshot = (ULONG)terminatorFound;
		matchCountSnapshot = (ULONG)matchCount;
		probe_str("FREEALL-INSTALL-FAILED terminator=");
		probe_hex32(terminatorSnapshot);
		probe_str(" matchCount=");
		probe_hex32(matchCountSnapshot);
		probe_ch('\n');
		gFreeAllInstallReplacements = matchCount;
		return;
	}

	gFreeAllOriginal = matchingEntry->func;
	gFreeAllInstallEntry = (void *)matchingEntry;
	matchingEntry->func = LibnixFreeAllCompat_Run;
	gFreeAllInstallReplacements = matchCount;

	if (matchingEntry->func != LibnixFreeAllCompat_Run) {
		matchingEntrySnapshot = (ULONG)matchingEntry;
		replacementSnapshot = (ULONG)matchingEntry->func;
		probe_str("FREEALL-INSTALL-VERIFY-FAILED entry=");
		probe_hex32(matchingEntrySnapshot);
		probe_str(" value=");
		probe_hex32(replacementSnapshot);
		probe_ch('\n');
		return;
	}

	matchingEntrySnapshot = (ULONG)matchingEntry;
	originalSnapshot = (ULONG)gFreeAllOriginal;
	replacementSnapshot = (ULONG)LibnixFreeAllCompat_Run;
	gFreeAllInstalled = 1UL;
	probe_str("FREEALL-INSTALL-SUCCESS entry=");
	probe_hex32(matchingEntrySnapshot);
	probe_str(" original=");
	probe_hex32(originalSnapshot);
	probe_str(" originalAgain=");
	probe_hex32(originalSnapshot);
	probe_str(" replacement=");
	probe_hex32(replacementSnapshot);
	probe_ch('\n');
}

void LibnixFreeAllCompat_Run(void)
{
	/* Volatile keeps the three symbolic head addresses materialised in the
	 * generated code while preserving one traversal implementation for production
	 * and FREEALL_PROBE diagnostic builds. */
	volatile unsigned long headAddr[FREEALL_HEAD_COUNT];
	struct Task *task;
	unsigned long li;

	headAddr[0] = (unsigned long)&__freeall_head0;
	headAddr[1] = (unsigned long)&__freeall_head1;
	headAddr[2] = (unsigned long)&__freeall_head2;

	probe_str("FREEALL-RUN-PHASE crt-exit\n");
	probe_str("FREEALL-WRAPPER-ENTER\n");

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

		if (node == (void *)0) {
			probe_str("FREEALL-LIST-EMPTY-NULL list=");
			probe_dec32(li);
			probe_str(" head=");
			probe_hex32((ULONG)headA);
			probe_ch('\n');
			continue;
		}

		if (node == (void *)0xffffffffUL) {
			probe_str("FREEALL-LIST-EMPTY-SENTINEL list=");
			probe_dec32(li);
			probe_str(" head=");
			probe_hex32((ULONG)headA);
			probe_str(" node=");
			probe_hex32((ULONG)node);
			probe_ch('\n');
			*(void **)headA = (void *)0;
			continue;
		}

		while (node != (void *)0) {
			unsigned char *base;
			unsigned long  size;
			void          *next;
			unsigned long  flags;

			if (guard >= FREEALL_MAX_NODES) {
				probe_str("FREEALL-LIST-LIMIT list=");
				probe_dec32(li);
				probe_str(" head=");
				probe_hex32((ULONG)headA);
				probe_str(" node=");
				probe_hex32((ULONG)node);
				probe_ch('\n');
				break;
			}
			guard++;

			base = (unsigned char *)node - 4;
			size = *(unsigned long *)base;
			next = *(void **)node;
			if (next == (void *)0xffffffffUL)
				next = (void *)0;
			flags = freeall_validation_flags(node, next, (void *)base, size);

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
			probe_hex32((ULONG)headA);
			probe_str(" node=");
			probe_hex32((ULONG)node);
			probe_str(" next=");
			probe_hex32((ULONG)next);
			probe_str(" base=");
			probe_hex32((ULONG)base);
			probe_str(" size=");
			probe_dec32(size);
			probe_str(" task=");
			probe_hex32((ULONG)task);
			probe_str(" flags=");
			probe_hex32((ULONG)flags);
			probe_str(" state=ABOUT_TO_FREE\n");

			if (gFreeAllSeenCount < (unsigned long)FREEALL_SEEN_LIMIT) {
				gFreeAllSeenNodes[gFreeAllSeenCount] = node;
				gFreeAllSeenBases[gFreeAllSeenCount] = (void *)base;
				gFreeAllSeenCount++;
			}

			*(void **)headA = next;
			FreeMem((APTR)base, size);

			gFreeAllProbe.state = FREEALL_STATE_FREE_RETURNED;
			probe_str("FREEALL seq=");
			probe_dec32(gFreeAllProbe.seq);
			probe_str(" state=FREE_RETURNED\n");

			node = next;
		}
	}

	probe_str("FREEALL-FINAL-HEADS head0=");
	probe_hex32((ULONG)*(void **)headAddr[0]);
	probe_str(" head1=");
	probe_hex32((ULONG)*(void **)headAddr[1]);
	probe_str(" head2=");
	probe_hex32((ULONG)*(void **)headAddr[2]);
	probe_ch('\n');

	probe_str("FREEALL-PROBE done\n");
}

#endif	/* FREEALL_PROBE && AMIGA_M68K */

/* Standalone AmiSSL reproducer for a suspected AmiSSL bug:
 *
 *   SSL_read() fails with SSL_get_error()==SSL_ERROR_SYSCALL and an EMPTY
 *   OpenSSL error queue (ERR_get_error()==0).  The caller does the normal
 *   "abort" cleanup -- skip SSL_shutdown() (the session is already broken),
 *   call SSL_free() -- and AmiSSL/Exec raises a Recoverable Alert
 *   (AN_BadFreeAddr, 0x0100000F) from inside or immediately after SSL_free().
 *
 * This is deliberately a *small*, self-contained file: no GUI, no decoder,
 * no radio_stream.c quarantine/workaround logic.  MiniAMP3's production
 * code (radio_stream.c) has been audited end-to-end (see
 * docs/amissl-lifecycle-audit.md) and does NOT double-free, does NOT free
 * after CleanupAmiSSL(), and does NOT reuse a stale pointer -- so the
 * cleanup sequence below is intentionally the same *naive* lifecycle the
 * bug report describes (abort, then SSL_free()/SSL_CTX_free()/
 * CleanupAmiSSL(), no quarantine) so the fault can be observed directly,
 * without the app's own leak-based workaround hiding it.
 *
 * Two modes, chosen with an environment variable so nothing needs
 * rebuilding between runs:
 *
 *   MP3_REPRO_MODE=inject (default)
 *     Deterministic.  Connects and completes a real TLS handshake, does a
 *     few successful SSL_read() calls (so the SSL object is a normal,
 *     "has been used successfully" object, not a freshly allocated one),
 *     then closes the *raw socket fd* out from under the still-live SSL
 *     object (CloseSocket() without SSL_shutdown(), without detaching the
 *     BIO) and calls SSL_read() once more.  That reliably reproduces a
 *     syscall-level failure with an empty error queue -- the same
 *     SSL_get_error()==SSL_ERROR_SYSCALL / ERR_get_error()==0 signature
 *     the app hit organically on a live Icecast stream -- without needing
 *     a flaky server or a multi-day soak.
 *
 *   MP3_REPRO_MODE=soak
 *     Naturalistic.  Walks a LIST of real HTTPS radio stations (a small
 *     built-in default list of public Icecast streams, or your own list
 *     via MP3_REPRO_STATION_FILE) round-robin, connecting to each one over
 *     a non-blocking socket and reading audio in a loop, exactly like the
 *     real player switching stations -- so a genuine mid-stream fault
 *     (peer RST, expiring token, transparent proxy hiccup, a server that
 *     just behaves oddly, ...) can occur on its own, the way it did over
 *     about three days of soak-testing the real app across many stations.
 *     Every attempt prints which station it's testing; the moment
 *     SSL_read()/SSL_get_error() shows SSL_ERROR_SYSCALL with an empty
 *     queue it's called out in the log immediately, right before the
 *     (deliberately naive) SSL_free() that may then fault.
 *
 * Other environment variables (all optional):
 *   MP3_REPRO_HOST           inject-mode host, default "ice1.somafm.com"
 *   MP3_REPRO_PATH           inject-mode path, default "/groovesalad-128-mp3"
 *   MP3_REPRO_STATION_FILE   soak-mode: path to a text file, one station
 *                            per line as "host path [port]" (port defaults
 *                            to 443; '#' comments and blank lines skipped).
 *                            Without this, a small built-in list of public
 *                            SomaFM Icecast streams (different edge hosts)
 *                            is used.
 *   MP3_REPRO_ITERS          soak-mode total connection attempts, default
 *                            = one pass over the whole station list
 *   MP3_REPRO_INJECT_AFTER   inject-mode: successful reads before the
 *                            fault injection, default 3
 *
 * Build (from the repo root, cross-compiling with m68k-amigaos-gcc):
 *   make -f Makefile.amiga amissl-free-repro-test
 *
 * WARNING: the cleanup path here is intentionally unsafe-by-the-book (no
 * poison/quarantine gate) so the underlying AmiSSL behavior is visible.
 * Do not copy this cleanup pattern into product code -- see
 * radio_stream.c's radio_ssl_close_stream_mode()/radio_ssl_free_ctx() for
 * the guarded version actually shipped.
 */

#if !defined(AMIGA_M68K) || !defined(HAVE_AMISSL)
#include <stdio.h>
int main(void)
{
    puts("amissl_ssl_free_repro must be built for AmigaOS with HAVE_AMISSL");
    return 1;
}
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>

struct Library *SocketBase = NULL;
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;

#define DEFAULT_HOST "ice1.somafm.com"
#define DEFAULT_PATH "/groovesalad-128-mp3"
#define DEFAULT_PORT 443
#define READ_BUF_SIZE 4096

#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif

static int amissl_initialized = 0;

static void progress(const char *msg) { puts(msg); fflush(stdout); }
static void progressf_long(const char *prefix, long value) { printf("%s%ld\n", prefix, value); fflush(stdout); }
static void progressf_str(const char *prefix, const char *value) { printf("%s%s\n", prefix, value); fflush(stdout); }

static int would_block(void)
{
    long e = Errno();
    return e == EWOULDBLOCK || e == EAGAIN;
}

static void set_nonblocking(long s)
{
    long nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
}

static void sleep_tick(void) { Delay(2); /* ~40ms @ 50Hz, matches radio_stream.c's backoff */ }

static const char *cfg_host(void) { const char *v = getenv("MP3_REPRO_HOST"); return (v && *v) ? v : DEFAULT_HOST; }
static const char *cfg_path(void) { const char *v = getenv("MP3_REPRO_PATH"); return (v && *v) ? v : DEFAULT_PATH; }
static int cfg_iters(int default_val) { const char *v = getenv("MP3_REPRO_ITERS"); int n = v ? atoi(v) : 0; return n > 0 ? n : default_val; }
static int cfg_inject_after(void) { const char *v = getenv("MP3_REPRO_INJECT_AFTER"); int n = v ? atoi(v) : 0; return n > 0 ? n : 3; }
static int cfg_is_soak(void) { const char *v = getenv("MP3_REPRO_MODE"); return v && strcmp(v, "soak") == 0; }

/* Station list for soak mode -- a handful of real public HTTPS Icecast
 * streams on different edge hosts by default, or your own list (e.g. the
 * exact stations that triggered the fault in production) via
 * MP3_REPRO_STATION_FILE. */
#define MAX_STATIONS 64
typedef struct { char host[128]; char path[256]; int port; } Station;
static Station g_stations[MAX_STATIONS];
static int g_station_count = 0;

static void add_default_stations(void)
{
    static const struct { const char *host; const char *path; } defs[] = {
        { "ice1.somafm.com", "/groovesalad-128-mp3" },
        { "ice2.somafm.com", "/dronezone-128-mp3" },
        { "ice4.somafm.com", "/lush-128-mp3" },
        { "ice5.somafm.com", "/deepspaceone-128-mp3" },
        { "ice6.somafm.com", "/indiepop-128-mp3" },
        { "ice1.somafm.com", "/secretagent-128-mp3" },
    };
    unsigned int i;
    for (i = 0; i < sizeof(defs) / sizeof(defs[0]) && g_station_count < MAX_STATIONS; i++) {
        strncpy(g_stations[g_station_count].host, defs[i].host, sizeof(g_stations[0].host) - 1);
        g_stations[g_station_count].host[sizeof(g_stations[0].host) - 1] = '\0';
        strncpy(g_stations[g_station_count].path, defs[i].path, sizeof(g_stations[0].path) - 1);
        g_stations[g_station_count].path[sizeof(g_stations[0].path) - 1] = '\0';
        g_stations[g_station_count].port = DEFAULT_PORT;
        g_station_count++;
    }
}

static void load_stations(void)
{
    const char *file = getenv("MP3_REPRO_STATION_FILE");
    FILE *f;
    char line[512];
    g_station_count = 0;
    if (!file || !*file) { add_default_stations(); return; }
    f = fopen(file, "r");
    if (!f) { progress("failed to open MP3_REPRO_STATION_FILE -- using built-in defaults"); add_default_stations(); return; }
    while (g_station_count < MAX_STATIONS && fgets(line, sizeof(line), f)) {
        char host[128], path[256];
        int port = DEFAULT_PORT;
        int n;
        size_t len = strlen(line);
        if (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (!line[0] || line[0] == '#') continue;
        n = sscanf(line, "%127s %255s %d", host, path, &port);
        if (n < 2) continue;
        strncpy(g_stations[g_station_count].host, host, sizeof(g_stations[0].host) - 1);
        g_stations[g_station_count].host[sizeof(g_stations[0].host) - 1] = '\0';
        strncpy(g_stations[g_station_count].path, path, sizeof(g_stations[0].path) - 1);
        g_stations[g_station_count].path[sizeof(g_stations[0].path) - 1] = '\0';
        g_stations[g_station_count].port = (n >= 3 && port > 0) ? port : DEFAULT_PORT;
        g_station_count++;
    }
    fclose(f);
    if (g_station_count == 0) add_default_stations();
}

static void log_ssl_error(const char *where, int ssl_ret, int e)
{
    unsigned long lib_error = ERR_get_error();
    char buf[160];
    buf[0] = '\0';
    if (lib_error != 0) ERR_error_string_n(lib_error, buf, sizeof(buf));
    printf("%s: SSL_ret=%d SSL_get_error=%d lib_error=%08lx (%s)\n",
        where, ssl_ret, e, lib_error, buf[0] ? buf : "none");
    fflush(stdout);
    if (e == SSL_ERROR_SYSCALL && lib_error == 0) {
        progress("*** REPRO CONDITION HIT: SSL_ERROR_SYSCALL with an EMPTY error queue ***");
    }
}

/* Naive "abort" cleanup: exactly what the bug report describes -- no
 * SSL_shutdown(), straight to SSL_free()/SSL_CTX_free().  No poison gate,
 * no quarantine, on purpose (see file header). Returns nothing; every step
 * is logged individually with fflush so a Recoverable Alert's log tail
 * pinpoints exactly which call it happened inside. */
static void naive_abort_cleanup(SSL **ssl, SSL_CTX **ctx, long *sock)
{
    if (*ssl) {
        progress("cleanup: SSL_shutdown SKIPPED (abort path, matches bug report)");
        progressf_long("cleanup: SSL_free start ssl=", (long)(void *)*ssl);
        SSL_free(*ssl);
        progress("cleanup: SSL_free done");
        *ssl = NULL;
    }
    if (*ctx) {
        progressf_long("cleanup: SSL_CTX_free start ctx=", (long)(void *)*ctx);
        SSL_CTX_free(*ctx);
        progress("cleanup: SSL_CTX_free done");
        *ctx = NULL;
    }
    if (*sock != -1) {
        progressf_long("cleanup: CloseSocket start fd=", *sock);
        CloseSocket(*sock);
        progress("cleanup: CloseSocket done");
        *sock = -1;
    }
}

static int open_libraries(void)
{
    progress("opening bsdsocket.library");
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) { progress("failed: OpenLibrary(bsdsocket.library)"); return 1; }

    progress("opening amisslmaster.library");
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) { progress("failed: OpenLibrary(amisslmaster.library)"); return 1; }

    progress("OpenAmiSSLTags start");
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        progress("failed: OpenAmiSSLTags");
        return 1;
    }
    progress("OpenAmiSSLTags OK");

    progress("InitAmiSSL start");
    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&errno,
                   TAG_DONE) != 0) {
        progress("failed: InitAmiSSL");
        return 1;
    }
    amissl_initialized = 1;
    progress("InitAmiSSL OK");
    return 0;
}

static void close_libraries(void)
{
    if (amissl_initialized) {
        progress("CleanupAmiSSL start");
        CleanupAmiSSL(TAG_DONE);
        amissl_initialized = 0;
        progress("CleanupAmiSSL done");
    }
    if (AmiSSLBase) {
        progress("CloseAmiSSL start");
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
        progress("CloseAmiSSL done");
    }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

static long tcp_connect(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    long s;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) { progress("failed: DNS lookup"); return -1; }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) { progressf_long("failed: socket errno=", Errno()); return -1; }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        progressf_long("failed: TCP connect errno=", Errno());
        CloseSocket(s);
        return -1;
    }
    return s;
}

/* Handshake over a NON-BLOCKING socket -- matches the app's real workload
 * ("repeated HTTPS Icecast/radio stream connections using non-blocking
 * sockets") rather than the blocking smoke test in amissl_https_get.c. */
static int tls_handshake(SSL_CTX **ctx, SSL **ssl, long sock)
{
    const SSL_METHOD *method;
    int tries;

    method = SSLv23_client_method();
    if (!method) { progress("failed: SSLv23_client_method"); return -1; }

    *ctx = SSL_CTX_new(method);
    if (!*ctx) { progress("failed: SSL_CTX_new"); return -1; }
    SSL_CTX_set_verify(*ctx, SSL_VERIFY_NONE, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(*ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    *ssl = SSL_new(*ctx);
    if (!*ssl) { progress("failed: SSL_new"); return -1; }
    SSL_set_fd(*ssl, (int)sock);

    set_nonblocking(sock);

    for (tries = 0; tries < 250; tries++) { /* ~10s budget at 40ms/poll */
        int r = SSL_connect(*ssl);
        if (r == 1) { progress("TLS handshake OK"); return 0; }
        {
            int e = SSL_get_error(*ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
            log_ssl_error("SSL_connect", r, e);
            progress("failed: SSL_connect");
            return -1;
        }
    }
    progress("failed: SSL_connect timed out");
    return -1;
}

static int send_request(SSL *ssl, const char *host, const char *path)
{
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: BoingPlayer-repro/0.1 AmigaOS\r\n"
        "Icy-MetaData: 1\r\nConnection: close\r\n\r\n", path, host);
    int written;
    int tries;
    for (tries = 0; tries < 250; tries++) {
        written = SSL_write(ssl, req, n);
        if (written > 0) return 0;
        {
            int e = SSL_get_error(ssl, written);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
            log_ssl_error("SSL_write(request)", written, e);
            return -1;
        }
    }
    return -1;
}

/* Read audio-ish bytes in a loop like the real player's pump loop.
 * Returns: 1 = clean EOF/ZERO_RETURN, 0 = still going (caller should stop
 * for other reasons), -1 = fatal fault seen (already logged). *reads_ok is
 * incremented on every successful (n>0) read. */
static int read_loop(SSL *ssl, int max_reads, int *reads_ok)
{
    static char buf[READ_BUF_SIZE];
    int wb;
    while (*reads_ok < max_reads) {
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) { (*reads_ok)++; continue; }
        {
            int e = SSL_get_error(ssl, n);
            wb = (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE);
            if (wb) { sleep_tick(); continue; }
            if (e == SSL_ERROR_ZERO_RETURN) { progress("read_loop: clean SSL_ERROR_ZERO_RETURN (peer close_notify)"); return 1; }
            log_ssl_error("SSL_read", n, e);
            return -1;
        }
    }
    return 0;
}

/* MP3_REPRO_MODE=inject: deterministic fault injection. */
static int run_inject(void)
{
    const char *host = cfg_host();
    const char *path = cfg_path();
    int inject_after = cfg_inject_after();
    long sock;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    int reads_ok = 0;
    int rc = 1;

    progressf_str("mode=inject host=", host);
    progressf_str("mode=inject path=", path);
    progressf_long("mode=inject inject_after_reads=", inject_after);

    sock = tcp_connect(host, DEFAULT_PORT);
    if (sock == -1) goto out;

    if (tls_handshake(&ctx, &ssl, sock) != 0) goto out_close_sock;
    if (send_request(ssl, host, path) != 0) goto out_close_sock;

    if (read_loop(ssl, inject_after, &reads_ok) < 0) {
        progressf_long("inject: fault occurred organically after reads=", reads_ok);
        naive_abort_cleanup(&ssl, &ctx, &sock);
        rc = 0;
        goto out;
    }
    progressf_long("inject: successful reads before injection=", reads_ok);

    /* The injection: rip the raw fd out from under the still-live SSL
     * object without SSL_shutdown() and without detaching the BIO first --
     * this is exactly the state a peer-dropped-connection / socket-level
     * error leaves the SSL object in, produced deterministically instead
     * of waiting for the network to misbehave. */
    progress("inject: closing raw socket fd behind the live SSL object's back");
    CloseSocket(sock);
    sock = -1;

    {
        static char buf[READ_BUF_SIZE];
        int n = SSL_read(ssl, buf, sizeof(buf));
        int e = SSL_get_error(ssl, n);
        log_ssl_error("inject: post-injection SSL_read", n, e);
    }

    /* Naive abort cleanup -- the exact sequence from the bug report:
     * SSL_shutdown() skipped, straight to SSL_free(). If AmiSSL raises the
     * Recoverable Alert, it will happen inside the SSL_free() call logged
     * immediately below. */
    naive_abort_cleanup(&ssl, &ctx, &sock);
    rc = 0;
    goto out;

out_close_sock:
    if (sock != -1) { CloseSocket(sock); sock = -1; }
out:
    if (ssl) SSL_free(ssl);
    if (ctx) SSL_CTX_free(ctx);
    if (sock != -1) CloseSocket(sock);
    return rc;
}

/* MP3_REPRO_MODE=soak: walk the station list round-robin, like the real
 * player switching stations, and let a genuine fault surface on its own. */
static int run_soak(void)
{
    int iters;
    int i;
    int fault_count = 0;

    load_stations();
    progressf_long("mode=soak station_count=", g_station_count);
    for (i = 0; i < g_station_count; i++)
        printf("mode=soak station[%d]=%s%s\n", i, g_stations[i].host, g_stations[i].path);
    fflush(stdout);

    iters = cfg_iters(g_station_count);
    progressf_long("mode=soak total attempts=", iters);

    for (i = 0; i < iters; i++) {
        const Station *st = &g_stations[i % g_station_count];
        long sock;
        SSL_CTX *ctx = NULL;
        SSL *ssl = NULL;
        int reads_ok = 0;
        int fault;

        printf("soak: attempt %d/%d station=%s%s\n", i + 1, iters, st->host, st->path);
        fflush(stdout);

        sock = tcp_connect(st->host, st->port);
        if (sock == -1) { sleep_tick(); continue; }

        if (tls_handshake(&ctx, &ssl, sock) != 0) {
            if (ssl) SSL_free(ssl);
            if (ctx) SSL_CTX_free(ctx);
            CloseSocket(sock);
            sleep_tick();
            continue;
        }
        if (send_request(ssl, st->host, st->path) != 0) {
            naive_abort_cleanup(&ssl, &ctx, &sock);
            continue;
        }

        /* Read a bounded number of chunks per station (like a station
         * switch, not an unattended multi-hour play) so the loop actually
         * cycles through many connect/read/free cycles across many
         * different servers instead of camping on one stream -- exactly
         * the churn that surfaced the original fault over repeated
         * reconnects/station switches. */
        fault = read_loop(ssl, 400, &reads_ok);
        printf("soak: station=%s%s reads_ok=%d\n", st->host, st->path, reads_ok);
        fflush(stdout);
        if (fault < 0) {
            fault_count++;
            printf("soak: FATAL FAULT #%d on station=%s%s -- see SSL_read/SSL_get_error line above\n",
                fault_count, st->host, st->path);
            fflush(stdout);
            naive_abort_cleanup(&ssl, &ctx, &sock);
            progress("soak: cleanup after fault completed without an Exec alert -- fault did not reproduce corruption this time, continuing");
            continue;
        }
        naive_abort_cleanup(&ssl, &ctx, &sock);
    }
    printf("soak: all %d attempts completed, fault_count=%d, no Exec alert\n", iters, fault_count);
    fflush(stdout);
    return 0;
}

int main(void)
{
    int rc;
    progress("AmiSSL SSL_free()-after-SSL_ERROR_SYSCALL reproducer");
    if (open_libraries() != 0) { close_libraries(); return 1; }

    rc = cfg_is_soak() ? run_soak() : run_inject();

    close_libraries();
    return rc;
}
#endif

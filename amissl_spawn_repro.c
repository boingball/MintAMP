/* AmiSSL parent/child concurrency repro.
 *
 * This harness follows the real app shape more closely than the single-task
 * amissl_ssl_free_repro.c:
 *
 *   - parent opens bsdsocket.library and the shared AmiSSL instance
 *   - parent keeps doing small HTTPS probe-style transactions
 *   - child is spawned with CreateNewProcTags()
 *   - child uses the same global SocketBase/AmiSSLBase/AmiSSLExtBase and the
 *     same AmiSSL_ErrNoPtr storage while it does HTTPS stream churn
 *
 * The child is quiet by default. The parent prints child progress/stage via
 * shared counters so shared shell output from both tasks does not become the
 * thing being tested.
 *
 * The harness is bounded by default. MP3_SPAWN_PARENT_MAX_LOOPS stops the
 * parent cleanly without needing Ctrl-C. If the child does not unwind after a
 * stop request, the parent deliberately skips closing the shared AmiSSL/socket
 * libraries rather than tearing them out from under a live child task.
 *
 * Build:
 *   make -f Makefile.amiga amissl-free-repro-test RADIO=1 SSL=1
 *
 * Run:
 *   amissl_spawn_repro
 *   setenv MP3_SPAWN_CHILD_ITERS 1000
 *   setenv MP3_SPAWN_PARENT_MAX_LOOPS 1000
 *   setenv MP3_SPAWN_CHILD_READS 5
 *   setenv MP3_SPAWN_PARENT_READS 5
 *   amissl_spawn_repro
 */

#if !defined(AMIGA_M68K) || !defined(HAVE_AMISSL)
#include <stdio.h>
int main(void) { puts("amissl_spawn_repro must be built for AmigaOS with HAVE_AMISSL"); return 1; }
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
#include <dos/dos.h>
#include <dos/dostags.h>
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

#define DEFAULT_PORT 443
#define READ_BUF_SIZE 4096
#define STATION_COUNT 8

#define CHILD_STAGE_IDLE 0
#define CHILD_STAGE_DNS 1
#define CHILD_STAGE_SOCKET 2
#define CHILD_STAGE_CONNECT 3
#define CHILD_STAGE_CTX_NEW 4
#define CHILD_STAGE_SSL_NEW 5
#define CHILD_STAGE_SSL_CONNECT 6
#define CHILD_STAGE_SSL_WRITE 7
#define CHILD_STAGE_SSL_READ 8
#define CHILD_STAGE_SSL_FREE 9
#define CHILD_STAGE_CTX_FREE 10
#define CHILD_STAGE_DONE 11
#define CHILD_STAGE_CLEANUP_AMISSL 12

#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif
#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef EALREADY
#define EALREADY 37
#endif
#ifndef EISCONN
#define EISCONN 56
#endif

typedef struct Station {
    const char *host;
    const char *path;
    int port;
} Station;

static const Station g_stations[STATION_COUNT] = {
    { "ice1.somafm.com", "/groovesalad-128-mp3", DEFAULT_PORT },
    { "ice2.somafm.com", "/dronezone-128-mp3", DEFAULT_PORT },
    { "ice4.somafm.com", "/lush-128-mp3", DEFAULT_PORT },
    { "ice5.somafm.com", "/deepspaceone-128-mp3", DEFAULT_PORT },
    { "ice6.somafm.com", "/indiepop-128-mp3", DEFAULT_PORT },
    { "ice1.somafm.com", "/secretagent-128-mp3", DEFAULT_PORT },
    { "ice2.somafm.com", "/digitalis-128-mp3", DEFAULT_PORT },
    { "ice4.somafm.com", "/u80s-128-mp3", DEFAULT_PORT }
};

static int g_shared_amissl_errno = 0;
static volatile int g_stop_requested = 0;
static volatile int g_child_started = 0;
static volatile int g_child_done = 0;
static volatile int g_child_rc = 0;
static volatile int g_child_attempt = 0;
static volatile int g_child_completed = 0;
static volatile int g_child_stage = CHILD_STAGE_IDLE;
static volatile long g_child_errno = 0;
static volatile int g_child_sslerr = 0;
static volatile int g_child_reads = 0;
static volatile int g_child_faults = 0;
static volatile int g_child_amissl_cleaned = 0;
static volatile int g_parent_amissl_closed = 0;

static int g_child_iters = 500;
static int g_parent_max_loops = 1000;
static int g_child_read_target = 10;
static int g_parent_read_target = 10;
static int g_connect_tries = 150;
static int g_read_idle_spins = 75;
static int g_child_log = 0;

static void progress(const char *msg) { puts(msg); fflush(stdout); }
static void progressf_int(const char *prefix, int value) { printf("%s%d\n", prefix, value); fflush(stdout); }
static void progressf_ptr(const char *prefix, void *value) { printf("%s%p\n", prefix, value); fflush(stdout); }
static void sleep_tick(void) { Delay(2); }

static int role_is_child(const char *role)
{
    return role && role[0] == 'c' && role[1] == 'h' && role[2] == 'i' && role[3] == 'l' && role[4] == 'd';
}

static int role_should_log(const char *role)
{
    return !role_is_child(role) || g_child_log;
}

static void set_child_stage(const char *role, int stage)
{
    if (role_is_child(role)) g_child_stage = stage;
}

static const char *child_stage_name(int stage)
{
    switch (stage) {
        case CHILD_STAGE_IDLE: return "idle";
        case CHILD_STAGE_DNS: return "dns";
        case CHILD_STAGE_SOCKET: return "socket";
        case CHILD_STAGE_CONNECT: return "connect";
        case CHILD_STAGE_CTX_NEW: return "ctx-new";
        case CHILD_STAGE_SSL_NEW: return "ssl-new";
        case CHILD_STAGE_SSL_CONNECT: return "ssl-connect";
        case CHILD_STAGE_SSL_WRITE: return "ssl-write";
        case CHILD_STAGE_SSL_READ: return "ssl-read";
        case CHILD_STAGE_SSL_FREE: return "ssl-free";
        case CHILD_STAGE_CTX_FREE: return "ctx-free";
        case CHILD_STAGE_DONE: return "done";
        case CHILD_STAGE_CLEANUP_AMISSL: return "cleanup-amissl";
    }
    return "unknown";
}

static int role_amissl_already_cleaned(const char *role)
{
    return role_is_child(role) ? g_child_amissl_cleaned : g_parent_amissl_closed;
}

static int stop_requested(const char *role)
{
    if (g_stop_requested) return 1;
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
        g_stop_requested = 1;
        if (role_should_log(role)) {
            printf("%s: Ctrl-C seen; requesting cooperative shutdown\n", role ? role : "repro");
            fflush(stdout);
        }
        return 1;
    }
    return 0;
}

static int env_int(const char *name, int default_val)
{
    const char *v;
    int n;
    v = getenv(name);
    n = v ? atoi(v) : 0;
    return n > 0 ? n : default_val;
}

static void load_config(void)
{
    g_child_iters = env_int("MP3_SPAWN_CHILD_ITERS", 500);
    g_parent_max_loops = env_int("MP3_SPAWN_PARENT_MAX_LOOPS", 1000);
    g_child_read_target = env_int("MP3_SPAWN_CHILD_READS", 10);
    g_parent_read_target = env_int("MP3_SPAWN_PARENT_READS", 10);
    g_connect_tries = env_int("MP3_SPAWN_CONNECT_TRIES", 150);
    g_read_idle_spins = env_int("MP3_SPAWN_READ_IDLE_SPINS", 75);
    g_child_log = env_int("MP3_SPAWN_CHILD_LOG", 0);
}

static void set_nonblocking(long s)
{
    long nb;
    nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
}

static int connect_errno_is_progress(long e)
{
    return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY;
}

static long tcp_connect_polling(const char *role, const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    long s;
    int tries;

    if (stop_requested(role)) return -1;
    set_child_stage(role, CHILD_STAGE_DNS);
    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        if (role_should_log(role)) { printf("%s: DNS lookup failed host=%s\n", role, host); fflush(stdout); }
        if (role_is_child(role)) g_child_errno = Errno();
        return -1;
    }

    if (stop_requested(role)) return -1;
    set_child_stage(role, CHILD_STAGE_SOCKET);
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        if (role_should_log(role)) { printf("%s: socket failed errno=%ld shared_errno=%d\n", role, Errno(), g_shared_amissl_errno); fflush(stdout); }
        if (role_is_child(role)) g_child_errno = Errno();
        return -1;
    }
    set_nonblocking(s);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    set_child_stage(role, CHILD_STAGE_CONNECT);
    for (tries = 0; tries < g_connect_tries; tries++) {
        int cr;
        long e;
        if (stop_requested(role)) { CloseSocket(s); return -1; }
        cr = connect(s, (struct sockaddr *)&sa, sizeof(sa));
        if (cr == 0) return s;
        e = Errno();
        if (role_is_child(role)) g_child_errno = e;
        if (e == EISCONN) return s;
        if (!connect_errno_is_progress(e)) {
            if (role_should_log(role)) { printf("%s: TCP connect failed errno=%ld shared_errno=%d tries=%d\n", role, e, g_shared_amissl_errno, tries + 1); fflush(stdout); }
            CloseSocket(s);
            return -1;
        }
        sleep_tick();
    }

    if (role_should_log(role)) { printf("%s: TCP connect timed out host=%s shared_errno=%d tries=%d\n", role, host, g_shared_amissl_errno, g_connect_tries); fflush(stdout); }
    CloseSocket(s);
    return -1;
}

static void log_ssl_error(const char *role, const char *where, SSL *ssl, int ret)
{
    int e;
    unsigned long lib_error;
    char buf[160];

    e = SSL_get_error(ssl, ret);
    lib_error = ERR_get_error();
    buf[0] = '\0';
    if (role_is_child(role)) {
        g_child_errno = Errno();
        g_child_sslerr = e;
    }
    if (!role_should_log(role)) return;
    if (lib_error != 0) ERR_error_string_n(lib_error, buf, sizeof(buf));
    printf("%s: %s ret=%d SSL_get_error=%d lib_error=%08lx (%s) Errno=%ld shared_errno=%d\n",
        role, where, ret, e, lib_error, buf[0] ? buf : "none", Errno(), g_shared_amissl_errno);
    fflush(stdout);
}

static int tls_handshake(const char *role, SSL_CTX **ctx, SSL **ssl, long sock, const char *host)
{
    const SSL_METHOD *method;
    int tries;

    method = SSLv23_client_method();
    if (!method) return -1;
    if (role_amissl_already_cleaned(role)) {
        if (role_should_log(role)) { printf("%s: STALE-LIFECYCLE guard: refusing SSL_CTX_new after task AmiSSL cleanup/close\n", role); fflush(stdout); }
        return -1;
    }

    set_child_stage(role, CHILD_STAGE_CTX_NEW);
    *ctx = SSL_CTX_new(method);
    if (!*ctx) { if (role_should_log(role)) { printf("%s: SSL_CTX_new failed\n", role); fflush(stdout); } return -1; }
    SSL_CTX_set_verify(*ctx, SSL_VERIFY_NONE, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(*ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    set_child_stage(role, CHILD_STAGE_SSL_NEW);
    *ssl = SSL_new(*ctx);
    if (!*ssl) { if (role_should_log(role)) { printf("%s: SSL_new failed\n", role); fflush(stdout); } return -1; }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name(*ssl, host);
#endif
    SSL_set_fd(*ssl, (int)sock);

    set_child_stage(role, CHILD_STAGE_SSL_CONNECT);
    for (tries = 0; tries < 250; tries++) {
        int r;
        int e;
        if (stop_requested(role)) return -1;
        r = SSL_connect(*ssl);
        if (r == 1) return 0;
        e = SSL_get_error(*ssl, r);
        if (role_is_child(role)) g_child_sslerr = e;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error(role, "SSL_connect", *ssl, r);
        return -1;
    }
    if (role_should_log(role)) { printf("%s: SSL_connect timed out shared_errno=%d\n", role, g_shared_amissl_errno); fflush(stdout); }
    return -1;
}

static int send_request(const char *role, SSL *ssl, const char *host, const char *path)
{
    char req[640];
    int n;
    int sent;
    int tries;

    n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BoingPlayer-amissl-spawn-repro/0.5 AmigaOS\r\n"
        "Icy-MetaData: 1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    if (n <= 0 || n >= (int)sizeof(req)) return -1;

    set_child_stage(role, CHILD_STAGE_SSL_WRITE);
    sent = 0;
    for (tries = 0; tries < 250 && sent < n; tries++) {
        int r;
        int e;
        if (stop_requested(role)) return -1;
        r = SSL_write(ssl, req + sent, n - sent);
        if (r > 0) { sent += r; continue; }
        e = SSL_get_error(ssl, r);
        if (role_is_child(role)) g_child_sslerr = e;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error(role, "SSL_write", ssl, r);
        return -1;
    }
    return sent == n ? 0 : -1;
}

static int read_chunks(const char *role, SSL *ssl, int want_reads)
{
    char buf[READ_BUF_SIZE];
    int reads_ok;
    int spin;

    reads_ok = 0;
    spin = 0;
    set_child_stage(role, CHILD_STAGE_SSL_READ);
    while (reads_ok < want_reads) {
        int n;
        int e;
        if (stop_requested(role)) return reads_ok;
        n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) {
            reads_ok++;
            if (role_is_child(role)) g_child_reads = reads_ok;
            spin = 0;
            continue;
        }
        e = SSL_get_error(ssl, n);
        if (role_is_child(role)) g_child_sslerr = e;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (++spin > g_read_idle_spins) {
                if (role_should_log(role)) { printf("%s: read idle timeout reads_ok=%d spin=%d\n", role, reads_ok, spin); fflush(stdout); }
                return reads_ok;
            }
            sleep_tick();
            continue;
        }
        if (e == SSL_ERROR_ZERO_RETURN) {
            if (role_should_log(role)) { printf("%s: SSL_ERROR_ZERO_RETURN reads_ok=%d\n", role, reads_ok); fflush(stdout); }
            return reads_ok;
        }
        log_ssl_error(role, "SSL_read", ssl, n);
        return -1;
    }
    return reads_ok;
}

static void guarded_ssl_free(const char *role, SSL **ssl, int *freed)
{
    if (!ssl || !*ssl) return;
    set_child_stage(role, CHILD_STAGE_SSL_FREE);
    if (*freed) {
        if (role_should_log(role)) { printf("%s: STALE-POINTER guard: duplicate SSL_free attempt ssl=%p\n", role, (void *)*ssl); fflush(stdout); }
        return;
    }
    if (role_amissl_already_cleaned(role)) {
        if (role_should_log(role)) { printf("%s: STALE-LIFECYCLE guard: SSL_free after task AmiSSL cleanup/close ssl=%p\n", role, (void *)*ssl); fflush(stdout); }
        return;
    }
    if (role_should_log(role)) { printf("%s: cleanup SSL_free start ssl=%p\n", role, (void *)*ssl); fflush(stdout); }
    SSL_free(*ssl);
    if (role_should_log(role)) { printf("%s: cleanup SSL_free done\n", role); fflush(stdout); }
    *freed = 1;
    *ssl = NULL;
}

static void guarded_ctx_free(const char *role, SSL_CTX **ctx, int *freed)
{
    if (!ctx || !*ctx) return;
    set_child_stage(role, CHILD_STAGE_CTX_FREE);
    if (*freed) {
        if (role_should_log(role)) { printf("%s: STALE-POINTER guard: duplicate SSL_CTX_free attempt ctx=%p\n", role, (void *)*ctx); fflush(stdout); }
        return;
    }
    if (role_amissl_already_cleaned(role)) {
        if (role_should_log(role)) { printf("%s: STALE-LIFECYCLE guard: SSL_CTX_free after task AmiSSL cleanup/close ctx=%p\n", role, (void *)*ctx); fflush(stdout); }
        return;
    }
    if (role_should_log(role)) { printf("%s: cleanup SSL_CTX_free start ctx=%p\n", role, (void *)*ctx); fflush(stdout); }
    SSL_CTX_free(*ctx);
    if (role_should_log(role)) { printf("%s: cleanup SSL_CTX_free done\n", role); fflush(stdout); }
    *freed = 1;
    *ctx = NULL;
}

static int stream_once(const char *role, const Station *st, int attempt, int reads)
{
    long sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int ssl_freed;
    int ctx_freed;
    int rc;
    int reads_ok;

    sock = -1;
    ctx = NULL;
    ssl = NULL;
    ssl_freed = 0;
    ctx_freed = 0;
    rc = 0;

    if (role_is_child(role)) {
        g_child_reads = 0;
        g_child_sslerr = 0;
        g_child_errno = 0;
    }
    if (stop_requested(role)) return 0;
    if (role_should_log(role)) {
        printf("%s: attempt=%d url=https://%s:%d%s SocketBase=%p AmiSSLBase=%p ErrNoPtr=%p\n",
            role, attempt, st->host, st->port, st->path,
            (void *)SocketBase, (void *)AmiSSLBase, (void *)&g_shared_amissl_errno);
        fflush(stdout);
    }

    sock = tcp_connect_polling(role, st->host, st->port);
    if (sock == -1) return g_stop_requested ? 0 : -1;
    if (tls_handshake(role, &ctx, &ssl, sock, st->host) != 0) { rc = g_stop_requested ? 0 : -1; goto cleanup; }
    if (send_request(role, ssl, st->host, st->path) != 0) { rc = g_stop_requested ? 0 : -1; goto cleanup; }

    reads_ok = read_chunks(role, ssl, reads);
    if (reads_ok < 0) rc = -1;
    if (role_should_log(role)) { printf("%s: result attempt=%d reads_ok=%d rc=%d\n", role, attempt, reads_ok < 0 ? -1 : reads_ok, rc); fflush(stdout); }

cleanup:
    guarded_ssl_free(role, &ssl, &ssl_freed);
    guarded_ctx_free(role, &ctx, &ctx_freed);
    if (sock != -1) CloseSocket(sock);
    set_child_stage(role, CHILD_STAGE_DONE);
    return rc;
}

static void child_entry(void)
{
    int i;
    int faults;

    faults = 0;
    g_child_started = 1;
    if (g_child_log) {
        progress("child: InitAmiSSL using inherited SocketBase/shared ErrNoPtr");
        progressf_ptr("child: SocketBase=", (void *)SocketBase);
        progressf_ptr("child: AmiSSLBase=", (void *)AmiSSLBase);
        progressf_ptr("child: ErrNoPtr=", (void *)&g_shared_amissl_errno);
    }

    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&g_shared_amissl_errno,
                   TAG_DONE) != 0) {
        if (g_child_log) progress("child: InitAmiSSL failed");
        g_child_rc = 10;
        g_child_done = 1;
        return;
    }
    if (g_child_log) progress("child: InitAmiSSL OK");

    for (i = 0; i < g_child_iters && !stop_requested("child"); i++) {
        int rc;
        g_child_attempt = i + 1;
        rc = stream_once("child", &g_stations[i % STATION_COUNT], i + 1, g_child_read_target);
        if (rc != 0) faults++;
        g_child_faults = faults;
        g_child_completed = i + 1;
        if (g_child_log) { printf("child: completed=%d/%d faults=%d stop=%d\n", g_child_completed, g_child_iters, faults, g_stop_requested); fflush(stdout); }
        sleep_tick();
    }

    g_child_stage = CHILD_STAGE_CLEANUP_AMISSL;
    if (g_child_log) progress("child: CleanupAmiSSL start");
    CleanupAmiSSL(TAG_DONE);
    g_child_amissl_cleaned = 1;
    if (g_child_log) progress("child: CleanupAmiSSL done");
    g_child_rc = faults ? 2 : 0;
    g_child_done = 1;
}

static int open_parent_amissl(void)
{
    progress("parent: opening bsdsocket.library");
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) return 1;

    progress("parent: opening amisslmaster.library");
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) return 1;

    progress("parent: OpenAmiSSLTags start");
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, TRUE,
                       AmiSSL_InitAmiSSL, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&g_shared_amissl_errno,
                       TAG_DONE) != 0) {
        return 1;
    }
    progress("parent: OpenAmiSSLTags OK");
    progressf_ptr("parent: SocketBase=", (void *)SocketBase);
    progressf_ptr("parent: AmiSSLBase=", (void *)AmiSSLBase);
    progressf_ptr("parent: ErrNoPtr=", (void *)&g_shared_amissl_errno);
    return 0;
}

static void close_parent_amissl(void)
{
    if (AmiSSLBase) {
        progress("parent: CloseAmiSSL start");
        CloseAmiSSL();
        g_parent_amissl_closed = 1;
        progress("parent: CloseAmiSSL done");
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

int main(void)
{
    struct Process *child;
    int parent_faults;
    int parent_loops;
    int wait_tries;
    int last_child_completed;
    int child_stall_loops;
    int skip_parent_close;

    parent_faults = 0;
    parent_loops = 0;
    last_child_completed = 0;
    child_stall_loops = 0;
    skip_parent_close = 0;

    progress("AmiSSL parent/child shared SocketBase concurrency repro");
    load_config();
    progressf_int("config: child_iters=", g_child_iters);
    progressf_int("config: parent_max_loops=", g_parent_max_loops);
    progressf_int("config: child_reads=", g_child_read_target);
    progressf_int("config: parent_reads=", g_parent_read_target);
    progressf_int("config: connect_tries=", g_connect_tries);
    progressf_int("config: read_idle_spins=", g_read_idle_spins);
    progressf_int("config: child_log=", g_child_log);

    if (open_parent_amissl() != 0) { close_parent_amissl(); return 1; }

    progress("parent: CreateNewProcTags child start");
    child = CreateNewProcTags(NP_Entry, (ULONG)child_entry,
                              NP_Name, (ULONG)"amissl_spawn_child",
                              NP_StackSize, 65536,
                              NP_Priority, 0,
                              TAG_DONE);
    if (!child) { progress("parent: CreateNewProcTags failed"); close_parent_amissl(); return 1; }

    wait_tries = 0;
    while (!g_child_started && wait_tries < 250 && !stop_requested("parent")) { sleep_tick(); wait_tries++; }
    progressf_int("parent: child_started=", g_child_started);

    while (!g_child_done && !stop_requested("parent")) {
        int rc;
        parent_loops++;
        if (g_parent_max_loops > 0 && parent_loops > g_parent_max_loops) {
            g_stop_requested = 1;
            printf("parent: max loop cap reached, requesting clean stop parent_loops=%d cap=%d\n", parent_loops - 1, g_parent_max_loops);
            fflush(stdout);
            break;
        }

        rc = stream_once("parent-probe", &g_stations[parent_loops % STATION_COUNT], parent_loops, g_parent_read_target);
        if (rc != 0) parent_faults++;

        if (g_child_completed == last_child_completed) child_stall_loops++;
        else { last_child_completed = g_child_completed; child_stall_loops = 0; }
        if (child_stall_loops == 5 || child_stall_loops == 25 || child_stall_loops == 100 || child_stall_loops == 250) {
            printf("parent: WARNING child stalled parent_loops=%d child_attempt=%d child_completed=%d child_stage=%s child_errno=%ld child_sslerr=%d child_reads=%d stall_loops=%d\n",
                parent_loops, g_child_attempt, g_child_completed, child_stage_name(g_child_stage),
                g_child_errno, g_child_sslerr, g_child_reads, child_stall_loops);
            fflush(stdout);
        }

        printf("parent: loop=%d child_attempt=%d child_completed=%d child_stage=%s child_reads=%d child_done=%d child_faults=%d parent_faults=%d shared_errno=%d\n",
            parent_loops, g_child_attempt, g_child_completed, child_stage_name(g_child_stage),
            g_child_reads, g_child_done, g_child_faults, parent_faults, g_shared_amissl_errno);
        fflush(stdout);
        sleep_tick();
    }

    if (g_stop_requested && !g_child_done) {
        int tries;
        progress("parent: waiting for child before closing shared AmiSSL");
        for (tries = 0; tries < 500 && !g_child_done; tries++) sleep_tick();
        progressf_int("parent: child_done_after_wait=", g_child_done);
        if (!g_child_done) {
            skip_parent_close = 1;
            progress("parent: child still alive; SKIPPING CloseAmiSSL/CloseLibrary to avoid tearing shared libs from under child");
        }
    }

    printf("parent: child finished child_rc=%d child_attempt=%d child_completed=%d child_stage=%s parent_loops=%d parent_faults=%d stop=%d skip_close=%d\n",
        g_child_rc, g_child_attempt, g_child_completed, child_stage_name(g_child_stage), parent_loops, parent_faults, g_stop_requested, skip_parent_close);
    fflush(stdout);

    if (!skip_parent_close) close_parent_amissl();
    if (g_stop_requested) return skip_parent_close ? 3 : 0;
    return (g_child_rc || parent_faults) ? 2 : 0;
}

#endif

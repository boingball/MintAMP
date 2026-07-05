/* AmiSSL parent/child concurrency repro.
 *
 * This is a second harness beside amissl_ssl_free_repro.c. The first harness
 * proves single-task HTTPS connect/read/free churn is stable. This one follows
 * the app shape more closely:
 *
 *   - parent opens bsdsocket.library and the shared AmiSSL instance
 *   - parent keeps doing small HTTPS probe-style transactions
 *   - child is spawned with CreateNewProcTags()
 *   - child uses the same global SocketBase/AmiSSLBase/AmiSSLExtBase and the
 *     same AmiSSL_ErrNoPtr storage while it does its own HTTPS stream churn
 *
 * Build:
 *   make -f Makefile.amiga amissl-spawn-repro-test
 *
 * Run:
 *   amissl_spawn_repro
 *   setenv MP3_SPAWN_CHILD_ITERS 500
 *   setenv MP3_SPAWN_CHILD_READS 250
 *   setenv MP3_SPAWN_PARENT_READS 40
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

#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif
#ifndef EAGAIN
#define EAGAIN EWOULDBLOCK
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
static volatile int g_child_started = 0;
static volatile int g_child_done = 0;
static volatile int g_child_rc = 0;
static volatile int g_child_attempt = 0;

static int g_child_iters = 200;
static int g_child_reads = 200;
static int g_parent_reads = 25;

static void progress(const char *msg) { puts(msg); fflush(stdout); }
static void progressf_int(const char *prefix, int value) { printf("%s%d\n", prefix, value); fflush(stdout); }
static void progressf_ptr(const char *prefix, void *value) { printf("%s%p\n", prefix, value); fflush(stdout); }
static void sleep_tick(void) { Delay(2); }

static int env_int(const char *name, int default_val)
{
    const char *v = getenv(name);
    int n = v ? atoi(v) : 0;
    return n > 0 ? n : default_val;
}

static void load_config(void)
{
    g_child_iters = env_int("MP3_SPAWN_CHILD_ITERS", 200);
    g_child_reads = env_int("MP3_SPAWN_CHILD_READS", 200);
    g_parent_reads = env_int("MP3_SPAWN_PARENT_READS", 25);
}

static void set_nonblocking(long s)
{
    long nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
}

static long tcp_connect_blocking(const char *role, const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    long s;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        printf("%s: DNS lookup failed host=%s\n", role, host);
        fflush(stdout);
        return -1;
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        printf("%s: socket failed errno=%ld shared_errno=%d\n", role, Errno(), g_shared_amissl_errno);
        fflush(stdout);
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("%s: connect failed errno=%ld shared_errno=%d\n", role, Errno(), g_shared_amissl_errno);
        fflush(stdout);
        CloseSocket(s);
        return -1;
    }
    set_nonblocking(s);
    return s;
}

static void log_ssl_error(const char *role, const char *where, SSL *ssl, int ret)
{
    int e = SSL_get_error(ssl, ret);
    unsigned long lib_error = ERR_get_error();
    char buf[160];
    buf[0] = '\0';
    if (lib_error != 0) ERR_error_string_n(lib_error, buf, sizeof(buf));
    printf("%s: %s ret=%d SSL_get_error=%d lib_error=%08lx (%s) Errno=%ld shared_errno=%d\n",
        role, where, ret, e, lib_error, buf[0] ? buf : "none", Errno(), g_shared_amissl_errno);
    fflush(stdout);
}

static int tls_handshake(const char *role, SSL_CTX **ctx, SSL **ssl, long sock, const char *host)
{
    const SSL_METHOD *method = SSLv23_client_method();
    int tries;
    if (!method) return -1;

    *ctx = SSL_CTX_new(method);
    if (!*ctx) { printf("%s: SSL_CTX_new failed\n", role); fflush(stdout); return -1; }
    SSL_CTX_set_verify(*ctx, SSL_VERIFY_NONE, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(*ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    *ssl = SSL_new(*ctx);
    if (!*ssl) { printf("%s: SSL_new failed\n", role); fflush(stdout); return -1; }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name(*ssl, host);
#endif
    SSL_set_fd(*ssl, (int)sock);

    for (tries = 0; tries < 250; tries++) {
        int r = SSL_connect(*ssl);
        int e;
        if (r == 1) return 0;
        e = SSL_get_error(*ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error(role, "SSL_connect", *ssl, r);
        return -1;
    }
    printf("%s: SSL_connect timed out shared_errno=%d\n", role, g_shared_amissl_errno);
    fflush(stdout);
    return -1;
}

static int send_request(const char *role, SSL *ssl, const char *host, const char *path)
{
    char req[640];
    int n, sent, tries;

    n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BoingPlayer-amissl-spawn-repro/0.1 AmigaOS\r\n"
        "Icy-MetaData: 1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    if (n <= 0 || n >= (int)sizeof(req)) return -1;

    sent = 0;
    for (tries = 0; tries < 250 && sent < n; tries++) {
        int r = SSL_write(ssl, req + sent, n - sent);
        int e;
        if (r > 0) { sent += r; continue; }
        e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error(role, "SSL_write", ssl, r);
        return -1;
    }
    return sent == n ? 0 : -1;
}

static int read_chunks(const char *role, SSL *ssl, int want_reads)
{
    static char buf[READ_BUF_SIZE];
    int reads_ok = 0;
    int spin = 0;

    while (reads_ok < want_reads) {
        int n = SSL_read(ssl, buf, sizeof(buf));
        int e;
        if (n > 0) {
            reads_ok++;
            spin = 0;
            if ((reads_ok % 100) == 0) { printf("%s: reads_ok=%d\n", role, reads_ok); fflush(stdout); }
            continue;
        }
        e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (++spin > 750) { printf("%s: read idle timeout reads_ok=%d\n", role, reads_ok); fflush(stdout); return reads_ok; }
            sleep_tick();
            continue;
        }
        if (e == SSL_ERROR_ZERO_RETURN) {
            printf("%s: SSL_ERROR_ZERO_RETURN reads_ok=%d\n", role, reads_ok);
            fflush(stdout);
            return reads_ok;
        }
        log_ssl_error(role, "SSL_read", ssl, n);
        return -1;
    }
    return reads_ok;
}

static int stream_once(const char *role, const Station *st, int attempt, int reads)
{
    long sock = -1;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    int rc = 0;
    int reads_ok;

    printf("%s: attempt=%d url=https://%s:%d%s SocketBase=%p AmiSSLBase=%p ErrNoPtr=%p\n",
        role, attempt, st->host, st->port, st->path,
        (void *)SocketBase, (void *)AmiSSLBase, (void *)&g_shared_amissl_errno);
    fflush(stdout);

    sock = tcp_connect_blocking(role, st->host, st->port);
    if (sock == -1) return -1;
    if (tls_handshake(role, &ctx, &ssl, sock, st->host) != 0) { rc = -1; goto cleanup; }
    if (send_request(role, ssl, st->host, st->path) != 0) { rc = -1; goto cleanup; }

    reads_ok = read_chunks(role, ssl, reads);
    if (reads_ok < 0) rc = -1;

cleanup:
    if (ssl) {
        printf("%s: cleanup SSL_free start ssl=%p\n", role, (void *)ssl); fflush(stdout);
        SSL_free(ssl);
        printf("%s: cleanup SSL_free done\n", role); fflush(stdout);
    }
    if (ctx) {
        printf("%s: cleanup SSL_CTX_free start ctx=%p\n", role, (void *)ctx); fflush(stdout);
        SSL_CTX_free(ctx);
        printf("%s: cleanup SSL_CTX_free done\n", role); fflush(stdout);
    }
    if (sock != -1) CloseSocket(sock);
    return rc;
}

static void child_entry(void)
{
    int i;
    int faults = 0;

    g_child_started = 1;
    progress("child: InitAmiSSL using inherited SocketBase/shared ErrNoPtr");
    progressf_ptr("child: SocketBase=", (void *)SocketBase);
    progressf_ptr("child: AmiSSLBase=", (void *)AmiSSLBase);
    progressf_ptr("child: ErrNoPtr=", (void *)&g_shared_amissl_errno);

    if (InitAmiSSL(AmiSSL_SocketBase, (ULONG)SocketBase,
                   AmiSSL_ErrNoPtr, (ULONG)&g_shared_amissl_errno,
                   TAG_DONE) != 0) {
        progress("child: InitAmiSSL failed");
        g_child_rc = 10;
        g_child_done = 1;
        return;
    }
    progress("child: InitAmiSSL OK");

    for (i = 0; i < g_child_iters; i++) {
        int rc;
        g_child_attempt = i + 1;
        rc = stream_once("child", &g_stations[i % STATION_COUNT], i + 1, g_child_reads);
        if (rc != 0) faults++;
        sleep_tick();
    }

    progress("child: CleanupAmiSSL start");
    CleanupAmiSSL(TAG_DONE);
    progress("child: CleanupAmiSSL done");
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
    if (AmiSSLBase) { progress("parent: CloseAmiSSL start"); CloseAmiSSL(); progress("parent: CloseAmiSSL done"); AmiSSLBase = NULL; AmiSSLExtBase = NULL; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

int main(void)
{
    struct Process *child;
    int parent_faults = 0;
    int parent_loops = 0;
    int wait_tries;

    progress("AmiSSL parent/child shared SocketBase concurrency repro");
    load_config();
    progressf_int("config: child_iters=", g_child_iters);
    progressf_int("config: child_reads=", g_child_reads);
    progressf_int("config: parent_reads=", g_parent_reads);

    if (open_parent_amissl() != 0) { close_parent_amissl(); return 1; }

    progress("parent: CreateNewProcTags child start");
    child = CreateNewProcTags(NP_Entry, (ULONG)child_entry,
                              NP_Name, (ULONG)"amissl_spawn_child",
                              NP_StackSize, 65536,
                              NP_Priority, 0,
                              TAG_DONE);
    if (!child) { progress("parent: CreateNewProcTags failed"); close_parent_amissl(); return 1; }

    wait_tries = 0;
    while (!g_child_started && wait_tries < 250) { sleep_tick(); wait_tries++; }
    progressf_int("parent: child_started=", g_child_started);

    while (!g_child_done) {
        int rc;
        parent_loops++;
        rc = stream_once("parent-probe", &g_stations[parent_loops % STATION_COUNT], parent_loops, g_parent_reads);
        if (rc != 0) parent_faults++;
        printf("parent: loop=%d child_attempt=%d child_done=%d parent_faults=%d shared_errno=%d\n",
            parent_loops, g_child_attempt, g_child_done, parent_faults, g_shared_amissl_errno);
        fflush(stdout);
        sleep_tick();
    }

    printf("parent: child finished child_rc=%d child_attempt=%d parent_loops=%d parent_faults=%d\n",
        g_child_rc, g_child_attempt, parent_loops, parent_faults);
    fflush(stdout);

    close_parent_amissl();
    return (g_child_rc || parent_faults) ? 2 : 0;
}

#endif

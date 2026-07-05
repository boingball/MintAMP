/* Separate-executable AmiSSL parent launcher repro.
 *
 * This test launches amissl_child_worker_repro as a separate program via the
 * shell RUN command, while the parent process performs its own HTTPS probe
 * churn. The child executable opens its own bsdsocket.library and AmiSSL state.
 *
 * This contrasts with amissl_spawn_repro.c, where a CreateNewProcTags(NP_Entry)
 * child runs inside the same loaded image and reuses parent/global bases.
 *
 * Build:
 *   make -f Makefile.amiga amissl-exec-repro-test RADIO=1 SSL=1
 *
 * Run:
 *   amissl_exec_repro
 *
 * Useful settings:
 *   setenv MP3_EXEC_PARENT_LOOPS 100
 *   setenv MP3_EXEC_CHILD_ITERS 100
 *   setenv MP3_EXEC_PARENT_READS 5
 *   setenv MP3_EXEC_CHILD_READS 5
 */

#if !defined(AMIGA_M68K) || !defined(HAVE_AMISSL)
#include <stdio.h>
int main(void) { puts("amissl_exec_repro must be built for AmigaOS with HAVE_AMISSL"); return 1; }
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

#define DEFAULT_PORT 443
#define READ_BUF_SIZE 2048
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

static int g_errno_store = 0;

static int env_int(const char *name, int default_val)
{
    const char *v;
    int n;
    v = getenv(name);
    n = v ? atoi(v) : 0;
    return n > 0 ? n : default_val;
}

static void sleep_tick(void) { Delay(2); }

static int connect_errno_is_progress(long e)
{
    return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY;
}

static void set_nonblocking(long s)
{
    long nb;
    nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
}

static long tcp_connect_polling(const Station *st, int connect_tries)
{
    const struct hostent *he;
    struct sockaddr_in sa;
    long s;
    int tries;

    he = gethostbyname(st->host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        printf("parent-exec: DNS failed host=%s errno=%ld errno_store=%d\n", st->host, Errno(), g_errno_store);
        fflush(stdout);
        return -1;
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        printf("parent-exec: socket failed errno=%ld errno_store=%d\n", Errno(), g_errno_store);
        fflush(stdout);
        return -1;
    }
    set_nonblocking(s);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)st->port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    for (tries = 0; tries < connect_tries; tries++) {
        int cr;
        long e;
        cr = connect(s, (struct sockaddr *)&sa, sizeof(sa));
        if (cr == 0) return s;
        e = Errno();
        if (e == EISCONN) return s;
        if (!connect_errno_is_progress(e)) {
            printf("parent-exec: connect failed errno=%ld tries=%d\n", e, tries + 1);
            fflush(stdout);
            CloseSocket(s);
            return -1;
        }
        sleep_tick();
    }

    printf("parent-exec: connect timeout host=%s tries=%d\n", st->host, connect_tries);
    fflush(stdout);
    CloseSocket(s);
    return -1;
}

static void log_ssl_error(const char *where, SSL *ssl, int ret)
{
    int e;
    unsigned long lib_error;
    char buf[160];
    e = SSL_get_error(ssl, ret);
    lib_error = ERR_get_error();
    buf[0] = '\0';
    if (lib_error != 0) ERR_error_string_n(lib_error, buf, sizeof(buf));
    printf("parent-exec: %s ret=%d sslerr=%d lib_error=%08lx (%s) errno=%ld errno_store=%d\n",
        where, ret, e, lib_error, buf[0] ? buf : "none", Errno(), g_errno_store);
    fflush(stdout);
}

static int tls_handshake(SSL_CTX **ctx, SSL **ssl, long sock, const char *host)
{
    const SSL_METHOD *method;
    int tries;

    method = SSLv23_client_method();
    if (!method) return -1;

    *ctx = SSL_CTX_new(method);
    if (!*ctx) { printf("parent-exec: SSL_CTX_new failed\n"); fflush(stdout); return -1; }
    SSL_CTX_set_verify(*ctx, SSL_VERIFY_NONE, NULL);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(*ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    *ssl = SSL_new(*ctx);
    if (!*ssl) { printf("parent-exec: SSL_new failed\n"); fflush(stdout); return -1; }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name(*ssl, host);
#endif
    SSL_set_fd(*ssl, (int)sock);

    for (tries = 0; tries < 250; tries++) {
        int r;
        int e;
        r = SSL_connect(*ssl);
        if (r == 1) return 0;
        e = SSL_get_error(*ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error("SSL_connect", *ssl, r);
        return -1;
    }
    printf("parent-exec: SSL_connect timeout\n");
    fflush(stdout);
    return -1;
}

static int send_request(SSL *ssl, const Station *st)
{
    char req[640];
    int n;
    int sent;
    int tries;

    n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BoingPlayer-amissl-exec-parent/0.1 AmigaOS\r\n"
        "Icy-MetaData: 1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n",
        st->path, st->host);
    if (n <= 0 || n >= (int)sizeof(req)) return -1;

    sent = 0;
    for (tries = 0; tries < 250 && sent < n; tries++) {
        int r;
        int e;
        r = SSL_write(ssl, req + sent, n - sent);
        if (r > 0) { sent += r; continue; }
        e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error("SSL_write", ssl, r);
        return -1;
    }
    return sent == n ? 0 : -1;
}

static int read_chunks(SSL *ssl, int want_reads, int idle_spins)
{
    char buf[READ_BUF_SIZE];
    int reads_ok;
    int spin;

    reads_ok = 0;
    spin = 0;
    while (reads_ok < want_reads) {
        int n;
        int e;
        n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) { reads_ok++; spin = 0; continue; }
        e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (++spin > idle_spins) return reads_ok;
            sleep_tick();
            continue;
        }
        if (e == SSL_ERROR_ZERO_RETURN) return reads_ok;
        log_ssl_error("SSL_read", ssl, n);
        return -1;
    }
    return reads_ok;
}

static int stream_once(const Station *st, int attempt, int reads, int connect_tries, int idle_spins)
{
    long sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int rc;
    int reads_ok;

    sock = -1;
    ctx = NULL;
    ssl = NULL;
    rc = 0;

    printf("parent-exec: attempt=%d url=https://%s:%d%s SocketBase=%p AmiSSLBase=%p ErrNoPtr=%p\n",
        attempt, st->host, st->port, st->path, (void *)SocketBase, (void *)AmiSSLBase, (void *)&g_errno_store);
    fflush(stdout);

    sock = tcp_connect_polling(st, connect_tries);
    if (sock == -1) return -1;
    if (tls_handshake(&ctx, &ssl, sock, st->host) != 0) { rc = -1; goto cleanup; }
    if (send_request(ssl, st) != 0) { rc = -1; goto cleanup; }
    reads_ok = read_chunks(ssl, reads, idle_spins);
    if (reads_ok < 0) rc = -1;
    printf("parent-exec: result attempt=%d reads_ok=%d rc=%d\n", attempt, reads_ok, rc);
    fflush(stdout);

cleanup:
    if (ssl) { printf("parent-exec: SSL_free start ssl=%p\n", (void *)ssl); fflush(stdout); SSL_free(ssl); printf("parent-exec: SSL_free done\n"); fflush(stdout); }
    if (ctx) { printf("parent-exec: SSL_CTX_free start ctx=%p\n", (void *)ctx); fflush(stdout); SSL_CTX_free(ctx); printf("parent-exec: SSL_CTX_free done\n"); fflush(stdout); }
    if (sock != -1) CloseSocket(sock);
    return rc;
}

static int open_parent_libs(void)
{
    printf("parent-exec: OpenLibrary(bsdsocket.library)\n"); fflush(stdout);
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) return 1;

    printf("parent-exec: OpenLibrary(amisslmaster.library)\n"); fflush(stdout);
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) return 1;

    printf("parent-exec: OpenAmiSSLTags AmiSSL_InitAmiSSL TRUE\n"); fflush(stdout);
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, TRUE,
                       AmiSSL_InitAmiSSL, TRUE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&g_errno_store,
                       TAG_DONE) != 0) {
        return 1;
    }
    printf("parent-exec: OpenAmiSSLTags OK SocketBase=%p AmiSSLBase=%p ErrNoPtr=%p\n", (void *)SocketBase, (void *)AmiSSLBase, (void *)&g_errno_store);
    fflush(stdout);
    return 0;
}

static void close_parent_libs(void)
{
    if (AmiSSLBase) { printf("parent-exec: CloseAmiSSL start\n"); fflush(stdout); CloseAmiSSL(); printf("parent-exec: CloseAmiSSL done\n"); fflush(stdout); AmiSSLBase = NULL; AmiSSLExtBase = NULL; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

static int launch_worker(void)
{
    const char *cmd;
    int ok;

    cmd = "RUN >NIL: amissl_child_worker_repro";
    printf("parent-exec: Execute %s\n", cmd);
    fflush(stdout);
    ok = Execute((STRPTR)cmd, 0, 0);
    printf("parent-exec: Execute returned %d; child log is RAM:amissl_child_worker.log\n", ok);
    fflush(stdout);
    return ok ? 0 : 1;
}

int main(void)
{
    int parent_loops;
    int reads;
    int connect_tries;
    int idle_spins;
    int i;
    int faults;

    parent_loops = env_int("MP3_EXEC_PARENT_LOOPS", 100);
    reads = env_int("MP3_EXEC_PARENT_READS", 5);
    connect_tries = env_int("MP3_EXEC_CONNECT_TRIES", 150);
    idle_spins = env_int("MP3_EXEC_READ_IDLE_SPINS", 75);
    faults = 0;

    printf("parent-exec: start parent_loops=%d reads=%d connect_tries=%d idle_spins=%d\n", parent_loops, reads, connect_tries, idle_spins);
    fflush(stdout);

    if (open_parent_libs() != 0) {
        printf("parent-exec: open libs failed\n");
        fflush(stdout);
        close_parent_libs();
        return 1;
    }

    if (launch_worker() != 0) {
        printf("parent-exec: worker launch failed\n");
        fflush(stdout);
        close_parent_libs();
        return 1;
    }

    for (i = 0; i < parent_loops; i++) {
        if (stream_once(&g_stations[i % STATION_COUNT], i + 1, reads, connect_tries, idle_spins) != 0) faults++;
        sleep_tick();
    }

    printf("parent-exec: completed parent_loops=%d parent_faults=%d; check RAM:amissl_child_worker.log for child result\n", parent_loops, faults);
    fflush(stdout);
    close_parent_libs();
    return faults ? 2 : 0;
}

#endif

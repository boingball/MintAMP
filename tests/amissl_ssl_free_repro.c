/* Standalone AmiSSL reproducer / soak harness for suspected AmiSSL SSL_free()
 * heap corruption after SSL_ERROR_SYSCALL with an empty OpenSSL error queue.
 *
 * This file intentionally avoids MintAMP's production guard/quarantine logic.
 * It creates fresh SSL_CTX/SSL/socket objects, performs real HTTPS radio stream
 * handshakes/reads, then tears the objects down using the deliberately naive
 * abort path from the bug report so AmiSSL's behaviour is visible directly.
 *
 * Modes:
 *
 *   MP3_REPRO_MODE=soak   (default)
 *     Walk a built-in matrix of public HTTPS radio stream URLs repeatedly.
 *     This is the mode to leave running when one-off handshakes pass but a
 *     real player eventually faults after lots of station/connect/free churn.
 *
 *   MP3_REPRO_MODE=inject
 *     Deterministic fault injection. Connect, handshake, read a few chunks,
 *     close the raw socket behind SSL's back, then SSL_read()/SSL_free().
 *
 * Optional environment variables:
 *   MP3_REPRO_MODE           soak | inject. Default: soak.
 *   MP3_REPRO_HOST           inject-mode host. Default: ice1.somafm.com
 *   MP3_REPRO_PATH           inject-mode path. Default: /groovesalad-128-mp3
 *   MP3_REPRO_PORT           inject-mode port. Default: 443
 *   MP3_REPRO_INJECT_AFTER   inject-mode successful reads before fault. Default: 3
 *   MP3_REPRO_STATION_FILE   soak-mode text file. Accepted line formats:
 *                              https://host[:port]/path
 *                              host path [port]
 *                            Blank lines and # comments are skipped.
 *   MP3_REPRO_ITERS          soak-mode total connection attempts. Overrides passes.
 *   MP3_REPRO_PASSES         soak-mode passes over station list. Default: 3.
 *   MP3_REPRO_READS          soak-mode SSL_read chunks per attempt. Default: 250.
 *
 * Build:
 *   make -f Makefile.amiga amissl-free-repro-test
 *
 * Run examples on AmigaOS:
 *   setenv MP3_REPRO_MODE soak
 *   setenv MP3_REPRO_PASSES 20
 *   amissl_ssl_free_repro
 *
 *   setenv MP3_REPRO_STATION_FILE RAM:ssl-stations.txt
 *   setenv MP3_REPRO_ITERS 500
 *   amissl_ssl_free_repro
 *
 * WARNING: do not copy the cleanup path into production code. MintAMP's
 * radio_stream.c should keep its guarded/quarantine close path.
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
#include <ctype.h>

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
#define MAX_STATIONS 96
#define MAX_LINE 512

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
    char host[128];
    char path[256];
    int port;
} Station;

static int amissl_initialized = 0;
static Station g_stations[MAX_STATIONS];
static int g_station_count = 0;

static void progress(const char *msg) { puts(msg); fflush(stdout); }
static void progressf_long(const char *prefix, long value) { printf("%s%ld\n", prefix, value); fflush(stdout); }
static void progressf_int(const char *prefix, int value) { printf("%s%d\n", prefix, value); fflush(stdout); }
static void progressf_str(const char *prefix, const char *value) { printf("%s%s\n", prefix, value); fflush(stdout); }

static int env_int(const char *name, int default_val)
{
    const char *v;
    int n;
    v = getenv(name);
    n = v ? atoi(v) : 0;
    return n > 0 ? n : default_val;
}

static const char *cfg_host(void) { const char *v = getenv("MP3_REPRO_HOST"); return (v && *v) ? v : DEFAULT_HOST; }
static const char *cfg_path(void) { const char *v = getenv("MP3_REPRO_PATH"); return (v && *v) ? v : DEFAULT_PATH; }
static int cfg_port(void) { return env_int("MP3_REPRO_PORT", DEFAULT_PORT); }
static int cfg_inject_after(void) { return env_int("MP3_REPRO_INJECT_AFTER", 3); }
static int cfg_reads_per_attempt(void) { return env_int("MP3_REPRO_READS", 250); }
static int cfg_passes(void) { return env_int("MP3_REPRO_PASSES", 3); }
static int cfg_is_inject(void) { const char *v = getenv("MP3_REPRO_MODE"); return v && strcmp(v, "inject") == 0; }

static void sleep_tick(void) { Delay(2); }

static char *trim(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int add_station(const char *host, const char *path, int port)
{
    Station *st;
    if (!host || !*host || !path || path[0] != '/' || g_station_count >= MAX_STATIONS) return 0;
    st = &g_stations[g_station_count];
    strncpy(st->host, host, sizeof(st->host) - 1);
    st->host[sizeof(st->host) - 1] = '\0';
    strncpy(st->path, path, sizeof(st->path) - 1);
    st->path[sizeof(st->path) - 1] = '\0';
    st->port = port > 0 ? port : DEFAULT_PORT;
    g_station_count++;
    return 1;
}

static int add_station_url(const char *url)
{
    const char *p;
    const char *slash;
    const char *colon;
    char host[128];
    int host_len;
    int port;

    if (strncmp(url, "https://", 8) != 0) return 0;
    p = url + 8;
    slash = strchr(p, '/');
    if (!slash || slash == p) return 0;

    colon = strchr(p, ':');
    port = DEFAULT_PORT;
    if (colon && colon < slash) {
        host_len = (int)(colon - p);
        port = atoi(colon + 1);
    } else {
        host_len = (int)(slash - p);
    }
    if (host_len <= 0 || host_len >= (int)sizeof(host)) return 0;
    memcpy(host, p, (size_t)host_len);
    host[host_len] = '\0';
    return add_station(host, slash, port);
}

static void add_default_stations(void)
{
    static const char *urls[] = {
        "https://ice1.somafm.com/groovesalad-128-mp3",
        "https://ice2.somafm.com/groovesalad-128-mp3",
        "https://ice4.somafm.com/groovesalad-128-mp3",
        "https://ice1.somafm.com/dronezone-128-mp3",
        "https://ice2.somafm.com/dronezone-128-mp3",
        "https://ice5.somafm.com/dronezone-128-mp3",
        "https://ice1.somafm.com/lush-128-mp3",
        "https://ice4.somafm.com/lush-128-mp3",
        "https://ice1.somafm.com/deepspaceone-128-mp3",
        "https://ice5.somafm.com/deepspaceone-128-mp3",
        "https://ice1.somafm.com/secretagent-128-mp3",
        "https://ice6.somafm.com/secretagent-128-mp3",
        "https://ice1.somafm.com/beatblender-128-mp3",
        "https://ice2.somafm.com/beatblender-128-mp3",
        "https://ice1.somafm.com/cliqhop-128-mp3",
        "https://ice4.somafm.com/cliqhop-128-mp3",
        "https://ice1.somafm.com/spacestation-128-mp3",
        "https://ice2.somafm.com/spacestation-128-mp3",
        "https://ice1.somafm.com/fluid-128-mp3",
        "https://ice2.somafm.com/fluid-128-mp3",
        "https://ice1.somafm.com/indiepop-128-mp3",
        "https://ice6.somafm.com/indiepop-128-mp3",
        "https://ice1.somafm.com/digitalis-128-mp3",
        "https://ice2.somafm.com/digitalis-128-mp3",
        "https://ice1.somafm.com/u80s-128-mp3",
        "https://ice4.somafm.com/u80s-128-mp3",
        "https://ice1.somafm.com/defcon-128-mp3",
        "https://ice5.somafm.com/defcon-128-mp3"
    };
    unsigned int i;
    for (i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) add_station_url(urls[i]);
}

static void load_stations(void)
{
    const char *file;
    FILE *f;
    char line[MAX_LINE];

    g_station_count = 0;
    file = getenv("MP3_REPRO_STATION_FILE");
    if (!file || !*file) {
        add_default_stations();
        return;
    }

    f = fopen(file, "r");
    if (!f) {
        progress("failed to open MP3_REPRO_STATION_FILE -- using built-in defaults");
        add_default_stations();
        return;
    }

    while (g_station_count < MAX_STATIONS && fgets(line, sizeof(line), f)) {
        char *s;
        char host[128];
        char path[256];
        int port;
        int n;

        s = trim(line);
        if (!*s || *s == '#') continue;
        if (add_station_url(s)) continue;

        port = DEFAULT_PORT;
        n = sscanf(s, "%127s %255s %d", host, path, &port);
        if (n >= 2) add_station(host, path, (n >= 3) ? port : DEFAULT_PORT);
    }
    fclose(f);

    if (g_station_count == 0) {
        progress("station file contained no usable HTTPS stations -- using built-in defaults");
        add_default_stations();
    }
}

static void log_ssl_error(const char *where, int ssl_ret, int e)
{
    unsigned long lib_error;
    char buf[160];
    lib_error = ERR_get_error();
    buf[0] = '\0';
    if (lib_error != 0) ERR_error_string_n(lib_error, buf, sizeof(buf));
    printf("%s: SSL_ret=%d SSL_get_error=%d lib_error=%08lx (%s)\n",
        where, ssl_ret, e, lib_error, buf[0] ? buf : "none");
    fflush(stdout);
    if (e == SSL_ERROR_SYSCALL && lib_error == 0) {
        progress("*** REPRO CONDITION HIT: SSL_ERROR_SYSCALL with an EMPTY error queue ***");
    }
}

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

static void set_nonblocking(long s)
{
    long nb;
    nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
}

static int tls_handshake(SSL_CTX **ctx, SSL **ssl, long sock, const char *host)
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
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name(*ssl, host);
#endif
    SSL_set_fd(*ssl, (int)sock);

    set_nonblocking(sock);

    for (tries = 0; tries < 250; tries++) {
        int r;
        int e;
        r = SSL_connect(*ssl);
        if (r == 1) { progress("TLS handshake OK"); return 0; }
        e = SSL_get_error(*ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error("SSL_connect", r, e);
        progress("failed: SSL_connect");
        return -1;
    }
    progress("failed: SSL_connect timed out");
    return -1;
}

static int send_request(SSL *ssl, const char *host, int port, const char *path)
{
    char req[640];
    int n;
    int tries;

    if (port == DEFAULT_PORT) {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: BoingPlayer-amissl-repro/0.2 AmigaOS\r\n"
            "Icy-MetaData: 1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n\r\n",
            path, host);
    } else {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "User-Agent: BoingPlayer-amissl-repro/0.2 AmigaOS\r\n"
            "Icy-MetaData: 1\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n\r\n",
            path, host, port);
    }
    if (n <= 0 || n >= (int)sizeof(req)) { progress("failed: request buffer too small"); return -1; }

    for (tries = 0; tries < 250; tries++) {
        int written;
        int e;
        written = SSL_write(ssl, req, n);
        if (written > 0) return 0;
        e = SSL_get_error(ssl, written);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { sleep_tick(); continue; }
        log_ssl_error("SSL_write(request)", written, e);
        return -1;
    }
    progress("failed: SSL_write timed out");
    return -1;
}

static int read_loop(SSL *ssl, int max_reads, int *reads_ok)
{
    static char buf[READ_BUF_SIZE];
    int spin;
    spin = 0;
    while (*reads_ok < max_reads) {
        int n;
        int e;
        n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) {
            (*reads_ok)++;
            spin = 0;
            if ((*reads_ok % 100) == 0) progressf_int("read_loop: reads_ok=", *reads_ok);
            continue;
        }
        e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (++spin > 750) { progress("read_loop: no data timeout"); return 0; }
            sleep_tick();
            continue;
        }
        if (e == SSL_ERROR_ZERO_RETURN) { progress("read_loop: clean SSL_ERROR_ZERO_RETURN"); return 1; }
        log_ssl_error("SSL_read", n, e);
        return -1;
    }
    return 0;
}

static int run_inject(void)
{
    const char *host;
    const char *path;
    int port;
    int inject_after;
    long sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int reads_ok;
    int rc;

    host = cfg_host();
    path = cfg_path();
    port = cfg_port();
    inject_after = cfg_inject_after();
    sock = -1;
    ctx = NULL;
    ssl = NULL;
    reads_ok = 0;
    rc = 1;

    progressf_str("mode=inject host=", host);
    progressf_str("mode=inject path=", path);
    progressf_int("mode=inject port=", port);
    progressf_int("mode=inject inject_after_reads=", inject_after);

    sock = tcp_connect(host, port);
    if (sock == -1) goto out;
    if (tls_handshake(&ctx, &ssl, sock, host) != 0) goto out_close_sock;
    if (send_request(ssl, host, port, path) != 0) goto out_abort;

    if (read_loop(ssl, inject_after, &reads_ok) < 0) {
        progressf_int("inject: fault occurred organically after reads=", reads_ok);
        naive_abort_cleanup(&ssl, &ctx, &sock);
        rc = 0;
        goto out;
    }
    progressf_int("inject: successful reads before injection=", reads_ok);

    progress("inject: closing raw socket fd behind live SSL object");
    CloseSocket(sock);
    sock = -1;

    {
        static char buf[READ_BUF_SIZE];
        int n;
        int e;
        n = SSL_read(ssl, buf, sizeof(buf));
        e = SSL_get_error(ssl, n);
        log_ssl_error("inject: post-injection SSL_read", n, e);
    }

out_abort:
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

static int run_one_station(const Station *st, int attempt, int total, int reads_per_attempt, int *fault_count)
{
    long sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int reads_ok;
    int fault;

    sock = -1;
    ctx = NULL;
    ssl = NULL;
    reads_ok = 0;

    printf("soak: attempt %d/%d station=https://%s:%d%s\n",
        attempt, total, st->host, st->port, st->path);
    fflush(stdout);

    sock = tcp_connect(st->host, st->port);
    if (sock == -1) return 0;

    if (tls_handshake(&ctx, &ssl, sock, st->host) != 0) {
        naive_abort_cleanup(&ssl, &ctx, &sock);
        return 0;
    }

    if (send_request(ssl, st->host, st->port, st->path) != 0) {
        naive_abort_cleanup(&ssl, &ctx, &sock);
        return 0;
    }

    fault = read_loop(ssl, reads_per_attempt, &reads_ok);
    printf("soak: result station=%s%s reads_ok=%d fault=%d\n",
        st->host, st->path, reads_ok, fault);
    fflush(stdout);

    if (fault < 0) {
        (*fault_count)++;
        printf("soak: FATAL FAULT #%d on https://%s:%d%s -- next line is the naive SSL_free path\n",
            *fault_count, st->host, st->port, st->path);
        fflush(stdout);
        naive_abort_cleanup(&ssl, &ctx, &sock);
        progress("soak: cleanup after fault returned; continuing to hunt for repeatability");
        return -1;
    }

    naive_abort_cleanup(&ssl, &ctx, &sock);
    return 0;
}

static int run_soak(void)
{
    int reads_per_attempt;
    int total;
    int explicit_iters;
    int i;
    int fault_count;

    load_stations();
    reads_per_attempt = cfg_reads_per_attempt();
    explicit_iters = env_int("MP3_REPRO_ITERS", 0);
    total = explicit_iters > 0 ? explicit_iters : (g_station_count * cfg_passes());
    fault_count = 0;

    progress("mode=soak automatic HTTPS radio station matrix");
    progressf_int("mode=soak station_count=", g_station_count);
    progressf_int("mode=soak total_attempts=", total);
    progressf_int("mode=soak reads_per_attempt=", reads_per_attempt);

    for (i = 0; i < g_station_count; i++) {
        printf("mode=soak station[%d]=https://%s:%d%s\n", i, g_stations[i].host, g_stations[i].port, g_stations[i].path);
    }
    fflush(stdout);

    for (i = 0; i < total; i++) {
        const Station *st;
        st = &g_stations[i % g_station_count];
        run_one_station(st, i + 1, total, reads_per_attempt, &fault_count);
        sleep_tick();
    }

    printf("soak: completed attempts=%d fault_count=%d\n", total, fault_count);
    fflush(stdout);
    return fault_count ? 2 : 0;
}

int main(void)
{
    int rc;
    progress("AmiSSL SSL_free()/HTTPS radio stream repro harness");
    if (open_libraries() != 0) { close_libraries(); return 1; }

    rc = cfg_is_inject() ? run_inject() : run_soak();

    close_libraries();
    return rc;
}
#endif

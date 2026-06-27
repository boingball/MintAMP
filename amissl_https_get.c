/* Tiny standalone AmiSSL HTTPS GET smoke test.
 *
 * This intentionally does not use the player, decoders, GUI, Radio Browser, or
 * radio_stream transport code.  It exists to isolate AmiSSL library/init/vector
 * problems from playback integration bugs.
 */

#if !defined(AMIGA_M68K) || !defined(HAVE_AMISSL)
#include <stdio.h>
int main(void)
{
    puts("amissl_https_get must be built for AmigaOS with HAVE_AMISSL");
    return 1;
}
#else

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <amissl/amissl.h>

struct Library *SocketBase = NULL;
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;

#define TEST_HOST "ice1.somafm.com"
#define TEST_PATH "/groovesalad-128-mp3"
#define TEST_PORT 443
#define HEADER_BUF_SIZE 8192

static void progress(const char *msg)
{
    puts(msg);
    fflush(stdout);
}

static void progressf_long(const char *prefix, long value)
{
    printf("%s%ld\n", prefix, value);
    fflush(stdout);
}

static long test_socket = -1;
static SSL_CTX *test_ctx = NULL;
static SSL *test_ssl = NULL;
static int test_tls_connected = 0;

static void cleanup(void)
{
    progress("21. cleanup start");
    if (test_ssl) {
        if (test_tls_connected) {
            progress("21. cleanup: SSL_shutdown start");
            SSL_shutdown(test_ssl);
            progress("21. cleanup: SSL_shutdown done");
        }
        progress("21. cleanup: SSL_free start");
        SSL_free(test_ssl);
        test_ssl = NULL;
        test_tls_connected = 0;
        progress("21. cleanup: SSL_free done");
    }
    if (test_ctx) {
        progress("21. cleanup: SSL_CTX_free start");
        SSL_CTX_free(test_ctx);
        test_ctx = NULL;
        progress("21. cleanup: SSL_CTX_free done");
    }
    if (test_socket != -1) {
        progress("21. cleanup: CloseSocket start");
        CloseSocket(test_socket);
        test_socket = -1;
        progress("21. cleanup: CloseSocket done");
    }
    if (AmiSSLBase) {
        progress("21. cleanup: CloseAmiSSL start");
        CloseAmiSSL();
        AmiSSLBase = NULL;
        progress("21. cleanup: CloseAmiSSL done");
    }
    if (AmiSSLMasterBase) {
        progress("21. cleanup: CloseLibrary(amisslmaster.library) start");
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
        progress("21. cleanup: CloseLibrary(amisslmaster.library) done");
    }
    if (SocketBase) {
        progress("21. cleanup: CloseLibrary(bsdsocket.library) start");
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        progress("21. cleanup: CloseLibrary(bsdsocket.library) done");
    }
    progress("21. cleanup OK");
}

static int find_header_end(const char *buf, int len)
{
    int i;
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    return -1;
}

static int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int ascii_strncasecmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int ca = ascii_tolower((unsigned char)a[i]);
        int cb = ascii_tolower((unsigned char)b[i]);
        if (ca != cb || ca == 0 || cb == 0) return ca - cb;
    }
    return 0;
}

static void print_line_value(const char *headers, int header_len, const char *name)
{
    int name_len = (int)strlen(name);
    int i = 0;
    while (i < header_len) {
        int line_start = i;
        int line_end;
        while (i < header_len && headers[i] != '\n') i++;
        line_end = i;
        if (line_end > line_start && headers[line_end - 1] == '\r') line_end--;
        if (line_end - line_start >= name_len && ascii_strncasecmp(headers + line_start, name, name_len) == 0) {
            int v = line_start + name_len;
            while (v < line_end && (headers[v] == ' ' || headers[v] == '\t')) v++;
            printf("%s%.*s\n", name, line_end - v, headers + v);
            fflush(stdout);
            return;
        }
        i++;
    }
    printf("%s<not present>\n", name);
    fflush(stdout);
}

static int open_libraries(void)
{
    progress("2. opening bsdsocket.library");
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    if (!SocketBase) { progress("failed: OpenLibrary(bsdsocket.library)"); return 1; }
    progress("3. bsdsocket opened OK");

    progress("4. opening amisslmaster.library");
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) { progress("failed: OpenLibrary(amisslmaster.library)"); return 1; }
    progress("5. amisslmaster opened OK");

    progress("6. InitAmiSSLMaster start");
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        progress("failed: InitAmiSSLMaster version/init mismatch");
        return 1;
    }
    progress("7. InitAmiSSLMaster OK");

    progress("8. opening AmiSSL");
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) { progress("failed: OpenAmiSSL"); return 1; }
    progress("9. AmiSSL opened OK");
    return 0;
}

static int tcp_connect(void)
{
    struct hostent *he;
    struct sockaddr_in sa;

    progress("10. DNS lookup start");
    he = gethostbyname(TEST_HOST);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) { progress("failed: DNS lookup"); return 1; }
    progress("11. DNS lookup OK");

    progress("12. socket create start");
    test_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (test_socket == -1) { progressf_long("failed: socket errno=", Errno()); return 1; }
    progress("13. socket create OK");

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TEST_PORT);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    progress("14. TCP connect start");
    if (connect(test_socket, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        progressf_long("failed: TCP connect errno=", Errno());
        return 1;
    }
    progress("15. TCP connect OK");
    return 0;
}

static int tls_connect(void)
{
    progress("16. TLS context/session start: SSL_CTX_new");
    test_ctx = SSL_CTX_new(TLS_client_method());
    if (!test_ctx) { progress("failed: SSL_CTX_new"); return 1; }
    progress("16. TLS context/session OK: SSL_CTX_new");

    progress("16. TLS context/session start: SSL_CTX_set_verify");
    SSL_CTX_set_verify(test_ctx, SSL_VERIFY_NONE, NULL);
    progress("16. TLS context/session OK: SSL_CTX_set_verify");

    progress("16. TLS context/session start: SSL_new");
    test_ssl = SSL_new(test_ctx);
    if (!test_ssl) { progress("failed: SSL_new"); return 1; }
    progress("16. TLS context/session OK: SSL_new");

    progress("16. TLS context/session start: SSL_set_fd");
    SSL_set_fd(test_ssl, (int)test_socket);
    progress("16. TLS context/session OK: SSL_set_fd");

    progress("17. TLS handshake start");
    if (SSL_connect(test_ssl) != 1) {
        progress("failed: SSL_connect");
        return 1;
    }
    test_tls_connected = 1;
    progress("18. TLS handshake OK");
    return 0;
}

int main(void)
{
    static char headers[HEADER_BUF_SIZE + 1];
    const char *request =
        "GET " TEST_PATH " HTTP/1.1\r\n"
        "Host: " TEST_HOST "\r\n"
        "User-Agent: BoingPlayer/0.1 AmigaOS\r\n"
        "Icy-MetaData: 1\r\n"
        "Connection: close\r\n"
        "\r\n";
    int total = 0;
    int header_end = -1;
    int rc = 1;
    int written;

    progress("1. start");
    progress("AmiSSL HTTPS GET smoke test: https://" TEST_HOST TEST_PATH);
    if (open_libraries() != 0) goto out;
    if (tcp_connect() != 0) goto out;
    if (tls_connect() != 0) goto out;

    progress("19. write request start");
    written = SSL_write(test_ssl, request, (int)strlen(request));
    if (written <= 0) {
        progress("failed: SSL_write request");
        goto out;
    }
    progressf_long("19. write request OK bytes=", (long)written);

    progress("20. read headers start");
    while (total < HEADER_BUF_SIZE) {
        int n;
        progress("20. read headers: SSL_read start");
        n = (int)SSL_read(test_ssl, headers + total, HEADER_BUF_SIZE - total);
        if (n <= 0) { progress("20. read headers: SSL_read returned <= 0"); break; }
        progressf_long("20. read headers: SSL_read bytes=", (long)n);
        total += n;
        header_end = find_header_end(headers, total);
        if (header_end >= 0) break;
    }
    if (header_end < 0) { progress("failed: response headers not complete"); goto out; }
    progress("20. read headers OK");
    headers[header_end] = '\0';

    printf("Status: %.*s\n", (int)(strchr(headers, '\n') ? strchr(headers, '\n') - headers : strlen(headers)), headers);
    fflush(stdout);
    print_line_value(headers, header_end, "Content-Type:");
    rc = 0;

out:
    cleanup();
    return rc;
}
#endif

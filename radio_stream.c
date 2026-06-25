#ifndef ENABLE_RADIO
#define ENABLE_RADIO 0
#endif
#if ENABLE_RADIO
#include "radio_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef RADIO_RING_BYTES
#define RADIO_RING_BYTES 65536UL
#endif
#ifndef RADIO_START_THRESHOLD
#define RADIO_START_THRESHOLD (RADIO_RING_BYTES / 4)
#endif
#ifndef RADIO_LOW_WATER_BYTES
#define RADIO_LOW_WATER_BYTES 4096UL
#endif
#ifndef RADIO_RECONNECT_MAX
#define RADIO_RECONNECT_MAX 10
#endif
#ifndef RADIO_RECONNECT_BACKOFF_PUMPS
#define RADIO_RECONNECT_BACKOFF_PUMPS 16
#endif
#define RADIO_HEADER_MAX 4096
#define RADIO_META_MAX 512
#ifndef RADIO_ZERO_BYTE_PUMP_MAX
#define RADIO_ZERO_BYTE_PUMP_MAX 64
#endif
#ifdef RADIO_DEBUG_OPEN
#define RADIO_OPEN_DEBUG_PRINTF(x) printf x
#else
#define RADIO_OPEN_DEBUG_PRINTF(x) ((void)0)
#endif


#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
struct Library *SocketBase = NULL;
#define RADIO_SOCKET long
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) CloseSocket(s)
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#define RADIO_SOCKET int
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) close(s)
#endif

/* Non-blocking ioctl request and "no data yet" errno values.  Define local
 * fallbacks rather than pulling in <sys/ioctl.h>/<sys/errno.h>, whose presence
 * varies across the m68k netinclude (and to avoid the header churn that broke
 * an earlier attempt).  0x8004667E is the standard BSD/bsdsocket FIONBIO. */
#ifndef FIONBIO
#define FIONBIO 0x8004667EUL
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif
#ifndef EAGAIN
#define EAGAIN 35
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef EISCONN
#define EISCONN 56
#endif

typedef enum {
    RADIO_PARSE_HEADER,
    RADIO_PARSE_AUDIO,
    RADIO_PARSE_META_LEN,
    RADIO_PARSE_META_PAYLOAD
} RadioParseState;

struct RadioStream {
    RADIO_SOCKET sock;
    RadioStatus status;
    char url[256], host[128], path[192];
    int port, bitrate, metaint, audioUntilMeta, headerDone;
    char contentType[64], title[128], stationName[128], genre[64], streamUrl[128], error[128];
    unsigned char *ring;
    unsigned long rpos, wpos, used, size;
    char header[RADIO_HEADER_MAX];
    int headerLen;
    RadioParseState parseState;
    unsigned char meta[RADIO_META_MAX];
    int metaLen, metaGot, metaLeft;
    int reconnectAttempts, reconnectDelay;
    int zeroBytePumps;
    int everPlayed;
    int stopping;
    struct in_addr hostAddr;   /* cached DNS result so reconnects skip gethostbyname() */
    int haveHostAddr;
};

static int radio_is_stopping(const RadioStream *rs) { return !rs || rs->stopping || rs->status == RADIO_STATUS_STOPPING || rs->status == RADIO_STATUS_CLOSED; }

/* Yield the CPU briefly during reconnect backoff.  reconnect_http() is the only
 * pump path that does not block on the socket, so without this the player
 * process spins at 100% CPU while the stream is down or re-buffering, starving
 * Workbench and the GUI's window redraws (the "desktop/mouse locks up while the
 * internet buffers" symptom). */
static void radio_backoff_sleep(void)
{
#if defined(AMIGA_M68K)
    Delay(2); /* ~40ms (2 ticks @ 50Hz) */
#else
    usleep(40000);
#endif
}

/* Put the stream socket into non-blocking mode.  WinUAE's built-in
 * bsdsocket.library runs a *blocking* socket call synchronously and freezes the
 * whole emulation (mouse included) until it returns - so a blocking recv() that
 * waits for the ring to refill freezes the machine for ~1s every time the
 * stream re-buffers.  With a non-blocking socket recv() returns immediately and
 * we yield with Delay() (a pure timer wait the emulator does not stall on). */
static void radio_set_nonblocking(RADIO_SOCKET s)
{
#if defined(AMIGA_M68K)
    long nb = 1;
    IoctlSocket(s, FIONBIO, (char *)&nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* True when the last socket call failed only because no data is ready yet. */
static int radio_would_block(void)
{
#if defined(AMIGA_M68K)
    long e = Errno();
    return e == EWOULDBLOCK || e == EAGAIN;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

static long radio_sock_errno(void)
{
#if defined(AMIGA_M68K)
    return Errno();
#else
    return errno;
#endif
}

/* Drive a non-blocking connect() to completion by re-issuing connect() and
 * yielding with Delay() between tries, so the connect never blocks (and so
 * never freezes WinUAE's emulation).  Returns 0 on success, -1 on failure or
 * stop/timeout.  Success is connect()==0 or EISCONN; anything else is treated
 * as "still connecting" until the timeout, which keeps us robust even if a
 * particular stack reports a non-standard in-progress errno. */
static int radio_wait_connected(RadioStream *rs, struct sockaddr_in *sa)
{
    int tries;
    /* ~6s budget at 40ms/poll; generous for a slow stream server. */
    for (tries = 0; tries < 150; tries++) {
        long e;
        if (radio_is_stopping(rs))
            return -1;
        radio_backoff_sleep();
        if (radio_is_stopping(rs))
            return -1;
        if (connect(rs->sock, (struct sockaddr *)sa, sizeof(*sa)) == 0)
            return 0;
        e = radio_sock_errno();
        if (e == EISCONN)
            return 0;
    }
    return -1;
}

/* Send the whole request on the (now non-blocking) socket, yielding on a
 * would-block partial send instead of failing. */
static int radio_send_all(RadioStream *rs, const char *buf, int len)
{
    int sent = 0, tries = 0;
    while (sent < len && tries < 150) {
        int r;
        if (radio_is_stopping(rs))
            return -1;
        r = (int)send(rs->sock, (char *)buf + sent, len - sent, 0);
        if (r > 0) { sent += r; continue; }
        if (r < 0 && radio_would_block()) { radio_backoff_sleep(); tries++; continue; }
        return -1;
    }
    return sent == len ? 0 : -1;
}

static void set_status(RadioStream *rs, RadioStatus status) { if (rs && rs->status != RADIO_STATUS_ERROR && rs->status != RADIO_STATUS_CLOSED && rs->status != RADIO_STATUS_STOPPING) rs->status = status; }
static void radio_copy_string(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, dstSize, "%s", src);
    dst[dstSize - 1] = 0;
}

static void radio_copy_bytes(char *dst, size_t dstSize, const unsigned char *src, int srcLen)
{
    size_t copyLen;
    if (!dst || dstSize == 0)
        return;
    dst[0] = 0;
    if (!src || srcLen <= 0)
        return;
    copyLen = (size_t)srcLen;
    if (copyLen >= dstSize)
        copyLen = dstSize - 1;
    memcpy(dst, src, copyLen);
    dst[copyLen] = 0;
}

static void set_error(RadioStream *rs, const char *msg) { if (rs) { radio_copy_string(rs->error,sizeof(rs->error),msg); rs->status = RADIO_STATUS_ERROR; RADIO_OPEN_DEBUG_PRINTF(("radio-open: %s\n", msg ? msg : "error")); } }
static int ring_write(RadioStream *rs, const unsigned char *p, int n) { int i=0; while (i<n && rs->used<rs->size) { rs->ring[rs->wpos++]=p[i++]; if(rs->wpos>=rs->size)rs->wpos=0; rs->used++; } return i; }
static int ring_read(RadioStream *rs, unsigned char *p, int n) { int i=0; while (i<n && rs->used) { p[i++]=rs->ring[rs->rpos++]; if(rs->rpos>=rs->size)rs->rpos=0; rs->used--; } return i; }
static int ci_starts(const char *s,const char *p){ while(*p) { if(tolower((unsigned char)*s++)!=tolower((unsigned char)*p++)) return 0; } return 1; }
static int ci_equals(const char *a,const char *b){ while(*a&&*b){ if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++)) return 0; } return *a==0&&*b==0; }
static char *trim(char *s){ char *e; while(*s&&isspace((unsigned char)*s))s++; e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }
static void close_current_socket(RadioStream *rs);

static int parse_url(RadioStream *rs,const char *url){ const char *p,*slash,*colon; int hl; if(!url||strncmp(url,"http://",7)) return -1; p=url+7; slash=strchr(p,'/'); if(!slash) slash=p+strlen(p); colon=memchr(p,':',(size_t)(slash-p)); hl=(int)((colon?colon:slash)-p); if(hl<=0||hl>=(int)sizeof(rs->host)) return -1; memcpy(rs->host,p,hl); rs->host[hl]=0; rs->port=colon?atoi(colon+1):80; if(rs->port<=0)rs->port=80; radio_copy_string(rs->path,sizeof(rs->path),*slash?slash:"/"); radio_copy_string(rs->url,sizeof(rs->url),url); return 0; }

static void reset_parser(RadioStream *rs)
{
    rs->headerDone = 0; rs->headerLen = 0; rs->header[0] = 0;
    rs->parseState = RADIO_PARSE_HEADER;
    rs->metaint = 0; rs->audioUntilMeta = 0;
    rs->metaLen = rs->metaGot = rs->metaLeft = 0;
    rs->contentType[0] = 0; rs->bitrate = 0;
    rs->stationName[0] = 0; rs->genre[0] = 0; rs->streamUrl[0] = 0;
}

static int connect_http(RadioStream *rs){
    struct sockaddr_in sa; char req[512]; int n; int cr;
#if defined(AMIGA_M68K)
    if(!SocketBase) SocketBase=OpenLibrary("bsdsocket.library",4); if(!SocketBase){ set_error(rs,"bsdsocket.library unavailable"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: bsdsocket open failed\n")); return -1; }
#endif
    if (radio_is_stopping(rs)) return -1;
    /* Resolve once and cache it; gethostbyname() is blocking and would freeze
     * the emulator on every reconnect if we re-resolved each time. */
    if(!rs->haveHostAddr){
        struct hostent *he=gethostbyname(rs->host);
        if(!he || !he->h_addr){ set_error(rs,"cannot resolve stream host"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: DNS failed for %s\n", rs->host)); return -1; }
        memcpy(&rs->hostAddr, he->h_addr, sizeof(rs->hostAddr));
        rs->haveHostAddr=1;
    }
    if (radio_is_stopping(rs)) return -1;
    rs->sock=socket(AF_INET,SOCK_STREAM,0); if(rs->sock==RADIO_INVALID_SOCKET){ set_error(rs,"cannot create socket"); return -1; }
    /* Go non-blocking BEFORE connect so the connect never stalls the machine. */
    radio_set_nonblocking(rs->sock);
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons((unsigned short)rs->port); sa.sin_addr=rs->hostAddr;
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    cr=connect(rs->sock,(struct sockaddr*)&sa,sizeof(sa));
    if(cr<0 && radio_wait_connected(rs,&sa)!=0){ close_current_socket(rs); set_error(rs,"cannot connect to stream"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: connect failed to %s:%d\n", rs->host, rs->port)); return -1; }
    if (radio_is_stopping(rs)) { close_current_socket(rs); return -1; }
    n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MiniAMP3/experimental\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n",rs->path,rs->host);
    if(radio_send_all(rs,req,n)!=0){ close_current_socket(rs); set_error(rs,"cannot send HTTP request"); return -1; }
    reset_parser(rs);
    return 0;
}

static void close_current_socket(RadioStream *rs){ if(rs && rs->sock!=RADIO_INVALID_SOCKET){ radio_close_socket(rs->sock); rs->sock=RADIO_INVALID_SOCKET; RADIO_STOP_DEBUG_PRINTF(("radio-stop: socket closed\n")); } }

static int reconnect_http(RadioStream *rs)
{
    close_current_socket(rs);
    if (radio_is_stopping(rs)) { if (rs) rs->status = RADIO_STATUS_CLOSED; return -1; }
    if (rs->reconnectAttempts >= RADIO_RECONNECT_MAX) { set_error(rs,"radio reconnect attempts exhausted"); return -1; }
    if (rs->reconnectDelay > 0) { rs->reconnectDelay--; set_status(rs, RADIO_STATUS_RECONNECTING); radio_backoff_sleep(); return 0; }
    rs->reconnectAttempts++;
    set_status(rs, rs->reconnectAttempts == 1 ? RADIO_STATUS_CONNECTING : RADIO_STATUS_RECONNECTING);
    if (connect_http(rs) == 0) { set_status(rs, RADIO_STATUS_BUFFERING); return 1; }
    rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS * rs->reconnectAttempts;
    set_status(rs, RADIO_STATUS_RECONNECTING);
    return 0;
}

static void parse_headers(RadioStream *rs,char *h){ char *line=strtok(h,"\r\n"); int code=0; if(line && ci_starts(line,"ICY")) code=200; else if(line && ci_starts(line,"HTTP/")) sscanf(line,"HTTP/%*s %d",&code); else { set_error(rs,"invalid HTTP stream response"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return; } if(code<200||code>299){ set_error(rs,"HTTP stream returned non-success status"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed status %d\n", code)); return; } while((line=strtok(NULL,"\r\n"))){ char *v=strchr(line,':'); if(!v) continue; *v++=0; line=trim(line); v=trim(v); if(ci_equals(line,"Content-Type")) radio_copy_string(rs->contentType,sizeof(rs->contentType),v); else if(ci_equals(line,"icy-metaint")){ rs->metaint=atoi(v); rs->audioUntilMeta=rs->metaint; } else if(ci_equals(line,"icy-br")) rs->bitrate=atoi(v); else if(ci_equals(line,"icy-name")) radio_copy_string(rs->stationName,sizeof(rs->stationName),v); else if(ci_equals(line,"icy-genre")) radio_copy_string(rs->genre,sizeof(rs->genre),v); else if(ci_equals(line,"icy-url")) radio_copy_string(rs->streamUrl,sizeof(rs->streamUrl),v); } if(rs->contentType[0] && !ci_starts(rs->contentType,"audio/mpeg")) { set_error(rs,"unsupported stream Content-Type (expected audio/mpeg)"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: content type unsupported: %s\n", rs->contentType)); } }

static void parse_meta(RadioStream *rs,const unsigned char *m,int n)
{
    static const char key[] = "StreamTitle='";
    const unsigned char *p, *end;
    int i, keyLen = (int)sizeof(key) - 1;
    if (!rs || !m || n <= 0)
        return;
    end = m + n;
    for (i = 0; i + keyLen <= n; i++) {
        if (!memcmp(m + i, key, (size_t)keyLen)) {
            p = m + i + keyLen;
            for (i = 0; p + i < end && p[i] != '\''; i++)
                ;
            radio_copy_bytes(rs->title, sizeof(rs->title), p, i);
            break;
        }
    }
}

static int process_bytes(RadioStream *rs, const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (rs->parseState == RADIO_PARSE_HEADER) {
            if (rs->headerLen >= RADIO_HEADER_MAX - 1) { set_error(rs,"HTTP header too large"); return -1; }
            rs->header[rs->headerLen++] = (char)b[i]; rs->header[rs->headerLen] = 0;
            if (rs->headerLen >= 4 && !memcmp(rs->header + rs->headerLen - 4, "\r\n\r\n", 4)) {
                rs->headerDone = 1; parse_headers(rs, rs->header); if (rs->status == RADIO_STATUS_ERROR) return -1; rs->parseState = RADIO_PARSE_AUDIO;
            }
            continue;
        }
        if (rs->metaint > 0 && rs->parseState == RADIO_PARSE_AUDIO && rs->audioUntilMeta == 0) rs->parseState = RADIO_PARSE_META_LEN;
        if (rs->parseState == RADIO_PARSE_META_LEN) {
            rs->metaLen = b[i] * 16; rs->metaGot = 0; rs->metaLeft = rs->metaLen;
            rs->parseState = rs->metaLen ? RADIO_PARSE_META_PAYLOAD : RADIO_PARSE_AUDIO;
            if (!rs->metaLen) rs->audioUntilMeta = rs->metaint;
            continue;
        }
        if (rs->parseState == RADIO_PARSE_META_PAYLOAD) {
            if (rs->metaGot < RADIO_META_MAX) rs->meta[rs->metaGot++] = b[i];
            rs->metaLeft--;
            if (rs->metaLeft <= 0) { if (rs->metaGot > 0) parse_meta(rs, rs->meta, rs->metaGot); rs->audioUntilMeta = rs->metaint; rs->parseState = RADIO_PARSE_AUDIO; }
            continue;
        }
        if (rs->parseState == RADIO_PARSE_AUDIO) {
            ring_write(rs, &b[i], 1);
            if (rs->metaint > 0 && rs->audioUntilMeta > 0) rs->audioUntilMeta--;
        }
    }
    return 0;
}

RadioStream *Radio_Open(const char *url){ RadioStream *rs=(RadioStream*)calloc(1,sizeof(*rs)); if(!rs) return NULL; rs->sock=RADIO_INVALID_SOCKET; rs->status=RADIO_STATUS_CONNECTING; rs->size=RADIO_RING_BYTES; rs->ring=(unsigned char*)malloc(rs->size); if(!rs->ring){ rs->size=0; set_error(rs,"not enough memory for radio buffer"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n")); return rs; } if(parse_url(rs,url)){ set_error(rs,"only direct http:// stream URLs are supported"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n")); return rs; } if(connect_http(rs)==0) rs->status=RADIO_STATUS_BUFFERING; else { close_current_socket(rs); if(rs->status!=RADIO_STATUS_ERROR) set_error(rs, rs->error[0] ? rs->error : "cannot open radio stream"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: Radio_Open returning error\n")); } return rs; }
void Radio_RequestStop(RadioStream *rs){ if(!rs)return; RADIO_STOP_DEBUG_PRINTF(("radio-stop: stop requested\n")); rs->stopping=1; rs->reconnectAttempts=RADIO_RECONNECT_MAX; rs->reconnectDelay=0; rs->status=RADIO_STATUS_STOPPING; close_current_socket(rs); RADIO_STOP_DEBUG_PRINTF(("radio-stop: marked stopping\n")); }
void Radio_Close(RadioStream *rs){ if(!rs)return; RADIO_STOP_DEBUG_PRINTF(("radio-stop: Radio_Close entered\n")); Radio_RequestStop(rs); rs->status=RADIO_STATUS_CLOSED; free(rs->ring); rs->ring=NULL; rs->size=rs->used=rs->rpos=rs->wpos=0; RADIO_STOP_DEBUG_PRINTF(("radio-stop: Radio_Close exited\n")); free(rs); }
int Radio_Pump(RadioStream *rs){ unsigned char b[1024]; int n; if(!rs||rs->status==RADIO_STATUS_ERROR) return -1; if(radio_is_stopping(rs)) { close_current_socket(rs); rs->status=RADIO_STATUS_CLOSED; return 0; } if(rs->sock==RADIO_INVALID_SOCKET) { if(!rs->everPlayed){ set_error(rs,"radio stream closed before playback started"); return -1; } return reconnect_http(rs); } n=(int)recv(rs->sock,(char*)b,sizeof(b),0); if(radio_is_stopping(rs)) { close_current_socket(rs); rs->status=RADIO_STATUS_CLOSED; return 0; } if(n<0 && radio_would_block()){ radio_backoff_sleep(); return 0; } /* non-blocking socket: no data yet - yield instead of freezing the emulator */ if(n<=0){ close_current_socket(rs); if(!rs->headerDone){ set_error(rs,"HTTP header read failed"); RADIO_OPEN_DEBUG_PRINTF(("radio-open: HTTP header failed\n")); return -1; } if(!rs->everPlayed){ set_error(rs,"radio stream ended before audio buffered"); return -1; } rs->reconnectDelay = RADIO_RECONNECT_BACKOFF_PUMPS; set_status(rs, RADIO_STATUS_RECONNECTING); return 0; } rs->zeroBytePumps=0; if(process_bytes(rs,b,n)<0) return -1; if(radio_is_stopping(rs)) { close_current_socket(rs); rs->status=RADIO_STATUS_CLOSED; return 0; } if(rs->headerDone && rs->used >= RADIO_START_THRESHOLD) { rs->reconnectAttempts = 0; rs->reconnectDelay = 0; rs->everPlayed = 1; rs->status=RADIO_STATUS_PLAYING; } else if(rs->headerDone && rs->status!=RADIO_STATUS_PLAYING) set_status(rs,RADIO_STATUS_BUFFERING); return n; }
int Radio_ReadAudio(RadioStream *rs,unsigned char *buf,int maxBytes){ int got; if(!rs||!buf||maxBytes<=0)return 0; if(radio_is_stopping(rs)) return 0; while(!radio_is_stopping(rs) && rs->status!=RADIO_STATUS_PLAYING && rs->used<RADIO_START_THRESHOLD && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && ++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX) { set_error(rs,"radio stream did not buffer audio"); break; } } while(!radio_is_stopping(rs) && rs->used==0 && rs->status!=RADIO_STATUS_ERROR) { if(Radio_Pump(rs)<=0 && !rs->everPlayed && ++rs->zeroBytePumps>=RADIO_ZERO_BYTE_PUMP_MAX) { set_error(rs,"radio stream did not deliver audio"); break; } } if(radio_is_stopping(rs)) return 0; got=ring_read(rs,buf,maxBytes); if(rs->status==RADIO_STATUS_PLAYING && rs->used<RADIO_LOW_WATER_BYTES) rs->status=RADIO_STATUS_BUFFERING; if(rs->status==RADIO_STATUS_BUFFERING && rs->used>=RADIO_START_THRESHOLD) rs->status=RADIO_STATUS_PLAYING; return got; }
RadioStatus Radio_GetStatus(RadioStream *rs){ return rs?rs->status:RADIO_STATUS_CLOSED; }
const char *Radio_GetTitle(RadioStream *rs){ return rs?rs->title:""; }
const char *Radio_GetStationName(RadioStream *rs){ return rs?rs->stationName:""; }
const char *Radio_GetGenre(RadioStream *rs){ return rs?rs->genre:""; }
const char *Radio_GetStreamUrl(RadioStream *rs){ return rs?rs->streamUrl:""; }
int Radio_GetMetaInt(RadioStream *rs){ return rs?rs->metaint:0; }
const char *Radio_GetContentType(RadioStream *rs){ return rs?rs->contentType:""; }
const char *Radio_GetError(RadioStream *rs){ return rs?(rs->error[0]?rs->error:""):"radio not open"; }
int Radio_GetBitrate(RadioStream *rs){ return rs?rs->bitrate:0; }
int Radio_GetBufferedBytes(RadioStream *rs){ return rs?(int)rs->used:0; }
const char *Radio_StatusText(RadioStatus s){ switch(s){case RADIO_STATUS_CONNECTING:return "Connecting";case RADIO_STATUS_BUFFERING:return "Buffering";case RADIO_STATUS_PLAYING:return "Playing";case RADIO_STATUS_RECONNECTING:return "Reconnecting";case RADIO_STATUS_STOPPING:return "Stopping";case RADIO_STATUS_CLOSED:return "Closed";case RADIO_STATUS_ERROR:return "Error";default:return "Idle";} }

#endif /* ENABLE_RADIO */

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
#define RADIO_HEADER_MAX 4096
#define RADIO_META_MAX 512

#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#define RADIO_SOCKET long
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) CloseSocket(s)
#else
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#define RADIO_SOCKET int
#define RADIO_INVALID_SOCKET (-1)
#define radio_close_socket(s) close(s)
#endif

struct RadioStream {
    RADIO_SOCKET sock;
    RadioStatus status;
    char url[256], host[128], path[192];
    int port, bitrate, metaint, metaLeft, audioUntilMeta, headerDone;
    char contentType[64], title[128], error[128];
    unsigned char *ring;
    unsigned long rpos, wpos, used, size;
};

static void set_error(RadioStream *rs, const char *msg) { if (rs) { strncpy(rs->error,msg,sizeof(rs->error)-1); rs->status = RADIO_STATUS_ERROR; } }
static int ring_write(RadioStream *rs, const unsigned char *p, int n) { int i=0; while (i<n && rs->used<rs->size) { rs->ring[rs->wpos++]=p[i++]; if(rs->wpos>=rs->size)rs->wpos=0; rs->used++; } return i; }
static int ring_read(RadioStream *rs, unsigned char *p, int n) { int i=0; while (i<n && rs->used) { p[i++]=rs->ring[rs->rpos++]; if(rs->rpos>=rs->size)rs->rpos=0; rs->used--; } return i; }
static int ci_starts(const char *s,const char *p){ while(*p) { if(tolower((unsigned char)*s++)!=tolower((unsigned char)*p++)) return 0; } return 1; }
static char *trim(char *s){ char *e; while(*s&&isspace((unsigned char)*s))s++; e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

static int parse_url(RadioStream *rs,const char *url){ const char *p,*slash,*colon; int hl; if(strncmp(url,"http://",7)) return -1; p=url+7; slash=strchr(p,'/'); if(!slash) slash=p+strlen(p); colon=memchr(p,':',(size_t)(slash-p)); hl=(int)((colon?colon:slash)-p); if(hl<=0||hl>=(int)sizeof(rs->host)) return -1; memcpy(rs->host,p,hl); rs->host[hl]=0; rs->port=colon?atoi(colon+1):80; if(rs->port<=0)rs->port=80; snprintf(rs->path,sizeof(rs->path),"%s",*slash?slash:"/"); snprintf(rs->url,sizeof(rs->url),"%s",url); return 0; }

static int connect_http(RadioStream *rs){
    struct hostent *he; struct sockaddr_in sa; char req[512]; int n;
#if defined(AMIGA_M68K)
    static struct Library *SocketBase; if(!SocketBase) SocketBase=OpenLibrary("bsdsocket.library",4); if(!SocketBase){ set_error(rs,"bsdsocket.library unavailable"); return -1; }
#endif
    he=gethostbyname(rs->host); if(!he){ set_error(rs,"cannot resolve stream host"); return -1; }
    rs->sock=socket(AF_INET,SOCK_STREAM,0); if(rs->sock==RADIO_INVALID_SOCKET){ set_error(rs,"cannot create socket"); return -1; }
    memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons((unsigned short)rs->port); memcpy(&sa.sin_addr,he->h_addr,he->h_length);
    if(connect(rs->sock,(struct sockaddr*)&sa,sizeof(sa))<0){ radio_close_socket(rs->sock); rs->sock=RADIO_INVALID_SOCKET; set_error(rs,"cannot connect to stream"); return -1; }
    n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MiniAMP3/experimental\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n",rs->path,rs->host);
    if(send(rs->sock,req,n,0)!=n){ set_error(rs,"cannot send HTTP request"); return -1; }
    return 0;
}

static void parse_headers(RadioStream *rs,char *h){ char *line=strtok(h,"\r\n"); int code=0; if(line) sscanf(line,"HTTP/%*s %d",&code); if(code<200||code>299){ set_error(rs,"HTTP stream returned non-success status"); return; } while((line=strtok(NULL,"\r\n"))){ char *v=strchr(line,':'); if(!v) continue; *v++=0; v=trim(v); if(ci_starts(line,"Content-Type")) snprintf(rs->contentType,sizeof(rs->contentType),"%s",v); else if(ci_starts(line,"icy-metaint")){ rs->metaint=atoi(v); rs->audioUntilMeta=rs->metaint; } else if(ci_starts(line,"icy-br")) rs->bitrate=atoi(v); else if(ci_starts(line,"icy-name") && !rs->title[0]) snprintf(rs->title,sizeof(rs->title),"%s",v); } if(!ci_starts(rs->contentType,"audio/mpeg")) set_error(rs,"unsupported stream Content-Type (expected audio/mpeg)"); }

static void parse_meta(RadioStream *rs,const unsigned char *m,int n){ const char *key="StreamTitle='"; const unsigned char *p; int i; for(i=0;i+13<n;i++){ if(!memcmp(m+i,key,13)){ p=m+i+13; for(i=0; p+i<m+n && p[i] != '\'' && i<(int)sizeof(rs->title)-1; i++) rs->title[i]=(char)p[i]; rs->title[i]=0; break; } } }

RadioStream *Radio_Open(const char *url){ RadioStream *rs=(RadioStream*)calloc(1,sizeof(*rs)); if(!rs) return NULL; rs->sock=RADIO_INVALID_SOCKET; rs->size=RADIO_RING_BYTES; rs->ring=(unsigned char*)malloc(rs->size); rs->status=RADIO_STATUS_CONNECTING; if(!rs->ring){ set_error(rs,"not enough memory for radio buffer"); return rs; } if(parse_url(rs,url)){ set_error(rs,"only direct http:// stream URLs are supported"); return rs; } if(connect_http(rs)==0) rs->status=RADIO_STATUS_BUFFERING; return rs; }
void Radio_Close(RadioStream *rs){ if(!rs)return; if(rs->sock!=RADIO_INVALID_SOCKET) radio_close_socket(rs->sock); free(rs->ring); free(rs); }
int Radio_Pump(RadioStream *rs){ unsigned char b[1024]; int n,i=0; static unsigned char meta[RADIO_META_MAX]; if(!rs||rs->status==RADIO_STATUS_ERROR) return -1; n=(int)recv(rs->sock,(char*)b,sizeof(b),0); if(n<=0){ rs->status=RADIO_STATUS_RECONNECTING; set_error(rs,"stream connection closed"); return -1; } if(!rs->headerDone){ static char hdr[RADIO_HEADER_MAX]; static int hp; for(i=0;i<n&&hp<RADIO_HEADER_MAX-1;i++){ hdr[hp++]=(char)b[i]; hdr[hp]=0; if(hp>=4&&!memcmp(hdr+hp-4,"\r\n\r\n",4)){ rs->headerDone=1; parse_headers(rs,hdr); hp=0; i++; break; } } if(rs->status==RADIO_STATUS_ERROR) return -1; }
    for(;i<n;i++){ if(rs->metaint>0){ if(rs->audioUntilMeta==0){ int ml=b[i]*16; if(ml>0){ int got=0; while(got<ml){ int chunk=ml-got; if(chunk>(int)sizeof(meta)) chunk=sizeof(meta); n=(int)recv(rs->sock,(char*)meta,chunk,0); if(n<=0){ set_error(rs,"stream ended inside ICY metadata"); return -1; } if(got==0) parse_meta(rs,meta,n); got+=n; } } rs->audioUntilMeta=rs->metaint; continue; } rs->audioUntilMeta--; } ring_write(rs,&b[i],1); }
    if(rs->used > rs->size/4) rs->status=RADIO_STATUS_PLAYING;
    return n; }
int Radio_ReadAudio(RadioStream *rs,unsigned char *buf,int maxBytes){ int got; if(!rs||!buf||maxBytes<=0)return 0; while(rs->used==0 && rs->status!=RADIO_STATUS_ERROR) if(Radio_Pump(rs)<0) break; got=ring_read(rs,buf,maxBytes); if(rs->status==RADIO_STATUS_PLAYING && rs->used<4096) rs->status=RADIO_STATUS_BUFFERING; return got; }
RadioStatus Radio_GetStatus(RadioStream *rs){ return rs?rs->status:RADIO_STATUS_IDLE; }
const char *Radio_GetTitle(RadioStream *rs){ return rs?rs->title:""; }
const char *Radio_GetContentType(RadioStream *rs){ return rs?rs->contentType:""; }
const char *Radio_GetError(RadioStream *rs){ return rs?rs->error:""; }
int Radio_GetBitrate(RadioStream *rs){ return rs?rs->bitrate:0; }
int Radio_GetBufferedBytes(RadioStream *rs){ return rs?(int)rs->used:0; }
const char *Radio_StatusText(RadioStatus s){ switch(s){case RADIO_STATUS_CONNECTING:return "Connecting";case RADIO_STATUS_BUFFERING:return "Buffering";case RADIO_STATUS_PLAYING:return "Playing";case RADIO_STATUS_RECONNECTING:return "Reconnecting";case RADIO_STATUS_ERROR:return "Error";default:return "Idle";} }

#endif /* ENABLE_RADIO */

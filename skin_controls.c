#include "skin_controls.h"
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <stdlib.h>

SkinAudioState gSkinAudio;

static int valid_count(int n) { return n<0 ? 0 : n>SKIN_LIST_MAX ? SKIN_LIST_MAX : n; }
static unsigned long random_value(SkinOrder *o)
{ o->seed=(o->seed*1664525UL+1013904223UL)&0xffffffffUL; return o->seed; }
void skin_order_init(SkinOrder *o,unsigned long seed)
{ memset(o,0,sizeof(*o)); o->seed=seed; o->last=o->cursor=-1; }
void skin_order_reset(SkinOrder *o,int count,int current)
{
    o->count=valid_count(count); o->used=0; o->cursor=-1; o->last=-1;
    memset(o->seen,0,sizeof(o->seen));
    skin_order_observe(o,count,current);
}
void skin_order_observe(SkinOrder *o,int count,int current)
{
    count=valid_count(count);
    if (o->count!=count) { skin_order_reset(o,count,current); return; }
    if (current<0 || current>=count || current==o->last) return;
    if (o->cursor+1<o->used && o->history[o->cursor+1]==current) ++o->cursor;
    else {
        o->used=o->cursor+1;
        if (o->used==SKIN_LIST_MAX) {
            memmove(o->history,o->history+1,(SKIN_LIST_MAX-1)*sizeof(int)); --o->used;
        }
        o->history[o->used++]=current; o->cursor=o->used-1;
    }
    o->last=current; o->seen[current]=1;
}
int skin_order_next(SkinOrder *o,int count,int current,int natural)
{
    int i,n=0,choice,index=-1;
    skin_order_observe(o,count,current); count=o->count;
    if (!count) return -1;
    if (natural && o->repeat==SKIN_REPEAT_TRACK && current>=0 && current<count) return current;
    if (!o->shuffle) {
        index=current+1;
        if (index<0) index=0;
        if (index>=count) index=o->repeat==SKIN_REPEAT_LIST ? 0 : -1;
    } else if (o->cursor+1<o->used) {
        index=o->history[++o->cursor]; o->last=index; return index;
    } else {
        for (i=0;i<count;++i) if (!o->seen[i]) ++n;
        if (!n && o->repeat==SKIN_REPEAT_LIST) {
            memset(o->seen,0,sizeof(o->seen));
            if (count>1 && current>=0 && current<count) o->seen[current]=1;
            for (i=0;i<count;++i) if (!o->seen[i]) ++n;
        }
        if (n) {
            choice=(int)(random_value(o)%(unsigned)n);
            for (i=0;i<count;++i) if (!o->seen[i] && choice--==0) { index=i; break; }
        }
    }
    if (index>=0) skin_order_observe(o,count,index);
    return index;
}
int skin_order_previous(SkinOrder *o,int count,int current)
{
    skin_order_observe(o,count,current);
    if (!o->shuffle) return current>0 ? current-1 : -1;
    if (o->cursor<=0) return -1;
    o->last=o->history[--o->cursor]; return o->last;
}
void skin_order_queue_init(SkinOrderQueue *q,unsigned long seed)
{ memset(q,0,sizeof(*q)); skin_order_init(&q->order,seed); q->pending=q->actual=-1; }
void skin_order_queue_cancel(SkinOrderQueue *q) { q->pending=-1; }
void skin_order_queue_reset(SkinOrderQueue *q,int count,int current)
{ skin_order_reset(&q->order,count,current); q->pending=-1; q->actual=current; }
void skin_order_queue_observe(SkinOrderQueue *q,int count,int current)
{
    if (q->pending>=0) {
        if (current==q->pending) {
            q->proposed.shuffle=q->order.shuffle; q->proposed.repeat=q->order.repeat;
            q->order=q->proposed; q->pending=-1;
        } else if (current!=q->actual || count!=q->order.count) q->pending=-1;
    }
    skin_order_observe(&q->order,count,current); q->actual=current;
}
int skin_order_queue_request(SkinOrderQueue *q,int count,int current,int direction,int natural)
{
    SkinOrder candidate=q->pending>=0 ? q->proposed : q->order;
    int index;
    candidate.shuffle=q->order.shuffle; candidate.repeat=q->order.repeat;
    if (q->pending>=0) current=q->pending;
    index=direction<0 ? skin_order_previous(&candidate,count,current) : skin_order_next(&candidate,count,current,natural);
    if (index>=0) { q->proposed=candidate; q->pending=index; }
    return index;
}
void skin_selection_sync(SkinSelection *s,int count,int focus)
{
    count=valid_count(count);
    if (s->count!=count || s->focus!=focus) {
        memset(s->bits,0,sizeof(s->bits));
        if (focus>=0 && focus<count) s->bits[focus]=1;
        s->anchor=focus; s->focus=focus; s->count=count;
    }
}
void skin_selection_set(SkinSelection *s,int index,int mode)
{
    int i,from,to;
    if (index<0 || index>=s->count) return;
    if (mode==SKIN_SELECT_FOCUS) { s->focus=index; return; }
    if (mode==SKIN_SELECT_RANGE && s->anchor>=0 && s->anchor<s->count) {
        from=index<s->anchor ? index : s->anchor; to=index>s->anchor ? index : s->anchor;
        memset(s->bits,0,sizeof(s->bits));
        for (i=from;i<=to;++i) s->bits[i]=1;
    } else if (mode==SKIN_SELECT_TOGGLE) { s->bits[index]^=1; s->anchor=index; }
    else { memset(s->bits,0,sizeof(s->bits)); s->bits[index]=1; s->anchor=index; }
    s->focus=index;
}
void skin_selection_all(SkinSelection *s,int mode)
{
    int i;
    for (i=0;i<s->count;++i) s->bits[i]=(unsigned char)(mode==2 ? !s->bits[i] : mode!=0);
}
struct SkinListSnapshot {
    int count,current,selected;
    SkinSelection selection;
    unsigned char data[1];
};
static unsigned snapshot_width(const SkinList *l)
{ return l->path_size+(l->titles ? l->title_size : 0)+(l->names ? l->name_size : 0)+(l->durations ? sizeof(int) : 0); }
SkinListSnapshot *skin_list_snapshot(const SkinList *l)
{
    SkinListSnapshot *s; unsigned char *at; size_t bytes; int count;
    if (!l || !l->count || !l->paths || l->path_size>512 || l->title_size>512 || l->name_size>512) return NULL;
    count=*l->count; if (count<0 || count>SKIN_LIST_MAX) return NULL;
    bytes=(size_t)count*snapshot_width(l); s=(SkinListSnapshot *)malloc(sizeof(*s)+bytes);
    if (!s) return NULL;
    s->count=count; s->current=*l->current; s->selected=*l->selected;
    if (l->selection) s->selection=*l->selection;
    at=s->data; bytes=(size_t)count*l->path_size; memcpy(at,l->paths,bytes); at+=bytes;
    if (l->titles) { bytes=(size_t)count*l->title_size; memcpy(at,l->titles,bytes); at+=bytes; }
    if (l->names) { bytes=(size_t)count*l->name_size; memcpy(at,l->names,bytes); at+=bytes; }
    if (l->durations) memcpy(at,l->durations,(size_t)count*sizeof(int));
    return s;
}
void skin_list_restore(SkinList *l,const SkinListSnapshot *s)
{
    const unsigned char *at=s->data; size_t bytes; int count=s->count;
    bytes=(size_t)count*l->path_size; memcpy(l->paths,at,bytes); at+=bytes;
    if (l->titles) { bytes=(size_t)count*l->title_size; memcpy(l->titles,at,bytes); at+=bytes; }
    if (l->names) { bytes=(size_t)count*l->name_size; memcpy(l->names,at,bytes); at+=bytes; }
    if (l->durations) memcpy(l->durations,at,(size_t)count*sizeof(int));
    *l->count=count; *l->current=s->current; *l->selected=s->selected;
    if (l->selection) *l->selection=s->selection;
}
void skin_list_snapshot_free(SkinListSnapshot *s) { free(s); }
static void swap_text(char *base,unsigned width,int a,int b)
{
    char temp[512];
    if (!base || !width || width>sizeof(temp)) return;
    memcpy(temp,base+a*width,width); memcpy(base+a*width,base+b*width,width);
    memcpy(base+b*width,temp,width);
}
static void list_swap(SkinList *l,int i,int j)
{
    int t; unsigned char bit;
    swap_text(l->paths,l->path_size,i,j); swap_text(l->titles,l->title_size,i,j);
    swap_text(l->names,l->name_size,i,j);
    if (l->durations) { t=l->durations[i]; l->durations[i]=l->durations[j]; l->durations[j]=t; }
    if (l->selection) { bit=l->selection->bits[i]; l->selection->bits[i]=l->selection->bits[j]; l->selection->bits[j]=bit; }
    if (*l->current==i) *l->current=j; else if (*l->current==j) *l->current=i;
    if (*l->selected==i) *l->selected=j; else if (*l->selected==j) *l->selected=i;
}
int skin_list_move(SkinList *l,int from,int to)
{
    int i,step;
    if (!l || from<0 || to<0 || from>=*l->count || to>=*l->count ||
        l->path_size>512 || l->title_size>512 || l->name_size>512) return 0;
    step=to>from ? 1 : -1;
    for (i=from;i!=to;i+=step) {
        list_swap(l,i,i+step);
    }
    if (l->selection) { l->selection->focus=*l->selected; l->selection->anchor=*l->selected; }
    return 1;
}
/* Stable selected-block insertion. Permute the borrowed arrays on release,
 * using bounded index storage, never a second playlist or heap allocation. */
int skin_list_move_selected(SkinList *l,int from,int to)
{
    int order[SKIN_LIST_MAX],positions[SKIN_LIST_MAX];
    int count,i,n=0,boundary,insert=0,chosen=0,changed=0;
    if (!l || !l->count || !l->selection) return 0;
    count=*l->count;
    if (count<1 || count>SKIN_LIST_MAX || from<0 || from>=count || to<0 || to>=count ||
        l->path_size>512 || l->title_size>512 || l->name_size>512) return 0;
    if (from==to) return 0;
    if (!l->selection->bits[from]) return skin_list_move(l,from,to);
    boundary=to>from ? to+1 : to;
    for (i=0;i<count;++i) {
        chosen+=l->selection->bits[i]!=0;
        if (i<boundary && !l->selection->bits[i]) ++insert;
        positions[i]=i;
    }
    if (!chosen || chosen==count) return 0;
    for (i=0;i<count;++i) if (!l->selection->bits[i]) {
        if (insert--==0) { int j; for (j=0;j<count;++j) if (l->selection->bits[j]) order[n++]=j; }
        order[n++]=i;
    }
    if (n<count) for (i=0;i<count;++i) if (l->selection->bits[i]) order[n++]=i;
    for (i=0;i<count;++i) {
        int j=i,identity;
        while (j<count && positions[j]!=order[i]) ++j;
        if (j==i) continue;
        list_swap(l,i,j);
        identity=positions[j]; positions[j]=positions[i]; positions[i]=identity; changed=1;
    }
    l->selection->focus=*l->selected; l->selection->anchor=*l->selected;
    return changed;
}
int skin_list_remove(SkinList *l,int index)
{
    int i,current,selected;
    if (!l || index<0 || index>=*l->count) return 0;
    current=*l->current; selected=*l->selected;
    if (!skin_list_move(l,index,*l->count-1)) return 0;
    --*l->count;
    *l->current=current==index ? -1 : current>index ? current-1 : current;
    *l->selected=selected==index ? (index<*l->count ? index : *l->count-1) : selected>index ? selected-1 : selected;
    if (l->selection) {
        for (i=*l->count;i<SKIN_LIST_MAX;++i) l->selection->bits[i]=0;
        l->selection->count=*l->count; l->selection->focus=*l->selected; l->selection->anchor=*l->selected;
    }
    return 1;
}
static int compare_text(const char *a,const char *b)
{
    while (*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)) { ++a; ++b; }
    return tolower((unsigned char)*a)-tolower((unsigned char)*b);
}
static const char *list_key(SkinList *l,int i,int by_path)
{
    const char *path=l->paths+i*l->path_size,*name;
    if (by_path) return path;
    if (l->titles && l->titles[i*l->title_size]) return l->titles+i*l->title_size;
    name=path;
    while (*path) { if (*path=='/' || *path==':') name=path+1; ++path; }
    return name;
}
void skin_list_sort(SkinList *l,int by_path)
{
    int i,j;
    for (i=1;i<*l->count;++i)
        for (j=i;j>0 && compare_text(list_key(l,j-1,by_path),list_key(l,j,by_path))>0;--j)
            skin_list_move(l,j,j-1);
}
int skin_balance_volume(int volume,int balance,int channel)
{
    int factor;
    if (volume<0) volume=0;
    if (volume>100) volume=100;
    if (balance<-100) balance=-100;
    if (balance>100) balance=100;
    factor=channel==0 && balance>0 ? 100-balance : channel==1 && balance<0 ? 100+balance : 100;
    return (volume*factor+50)/100;
}
void skin_eq_flat(void)
{
    int i;
    gSkinAudio.eq_preamp=0;
    for (i=0;i<SKIN_EQ_BANDS;++i) gSkinAudio.eq_bands[i]=0;
    ++gSkinAudio.eq_sequence;
}
/* Offline-generated Q12 amplitude gains for integer dB values -24..+12.
 * Clamp combined preamp/band gain to +12 dB to bound arithmetic/headroom. */
static const unsigned short gains[37]={258,290,325,365,410,460,516,579,649,728,817,917,
    1029,1155,1295,1453,1631,1830,2053,2304,2584,2899,3254,3651,4096,
    4596,5157,5786,6492,7284,8173,9170,10289,11544,12953,14533,16306};
int skin_eq_gain(int value,int gain)
{
    int product;
    if (gain==4096) return value;
    if (gain<0) gain=0;
    if (gain>16384) gain=16384;
    /* Truncate toward zero. All products fit 32 bits, including INT_MIN. */
    product=(value/16384)*gain;
    if (product>INT_MAX/4) return INT_MAX;
    if (product<INT_MIN/4) return INT_MIN;
    return product*4;
}
int skin_eq_apply(int *spectrum,int count,int rate)
{
    static int cached[576],last_rate,last_count;
    static int active;
    static unsigned long last_sequence=~0UL;
    static const int edges[9]={101,230,431,775,1732,4243,8485,12961,14967};
    unsigned long seq; int i,band,db;
    if (!gSkinAudio.eq_enabled || !spectrum || count<=0 || count>576 || rate<=0 || rate>192000) return 0;
    seq=gSkinAudio.eq_sequence;
    if (seq!=last_sequence || rate!=last_rate || count!=last_count) {
        active=0;
        for (i=0;i<count;++i) {
            int frequency=i*rate/(count*2);
            for (band=0;band<9 && frequency>=edges[band];++band) {}
            db=gSkinAudio.eq_preamp+gSkinAudio.eq_bands[band];
            if (db<-24) db=-24;
    if (db>12) db=12;
            cached[i]=gains[db+24];
            if (db) active=1;
        }
        last_sequence=seq; last_rate=rate; last_count=count;
    }
    if (!active) return 0;
    for (i=0;i<count;++i) {
        int value;
        if (!spectrum[i]) continue;
        value=skin_eq_gain(spectrum[i],cached[i]);
        /* Preserve the headroom expected by Helix's alias/IMDCT stages. */
        if (value>0x3fffffff) value=0x3fffffff;
        if (value<-0x3fffffff) value=-0x3fffffff;
        spectrum[i]=value;
    }
    return 1;
}
/* Visualiser analysis runs on the GUI task, never in the decoder/audio
 * submission loop. 256 windowed 8-bit samples are packed as 128 complex
 * points, so one 128-point fixed-point FFT (448 butterflies, 16x16 multiplies)
 * yields 127 real-spectrum bins. Every stage halves its outputs: magnitudes
 * never leave 16 bits, so 68020/030 MULS.W suffices and nothing overflows. */
static const short skin_vis_sin[65]={0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,
    6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,
    11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,
    15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,16384};
static const short skin_vis_hann[128]={1,11,31,60,100,149,208,277,355,443,541,648,765,891,1027,1171,
    1325,1488,1660,1841,2030,2229,2435,2650,2874,3105,3345,3592,3847,4110,4380,4657,
    4942,5233,5531,5835,6146,6463,6786,7115,7449,7789,8134,8484,8838,9197,9561,9929,
    10300,10675,11054,11436,11820,12208,12598,12990,13385,13781,14179,14578,14978,15379,15780,16182,
    16585,16987,17388,17789,18189,18588,18986,19382,19777,20169,20559,20947,21331,21713,22092,22467,
    22838,23206,23570,23929,24283,24633,24978,25318,25652,25981,26304,26621,26932,27236,27534,27825,
    28110,28387,28657,28920,29175,29422,29662,29893,30117,30332,30538,30737,30926,31107,31279,31442,
    31596,31740,31876,32002,32119,32226,32324,32412,32490,32559,32618,32667,32707,32736,32756,32766};
/* Bin edges of the 19 bars: one bin each at the bottom, then log-spaced to
 * Nyquist, so bass and treble share the 76-pixel window as in Winamp. */
static const unsigned char skin_vis_edges[SKIN_VIS_BARS+1]={1,2,3,4,5,6,7,8,9,10,13,17,21,28,36,46,59,77,99,128};
/* Q14 e^(-2 pi i k/256) parts for k = 0..255. */
static int skin_vis_cos(int k) { k&=255; return k<=64 ? skin_vis_sin[64-k] : k<=128 ? -skin_vis_sin[k-64] : k<=192 ? -skin_vis_sin[192-k] : skin_vis_sin[k-192]; }
static int skin_vis_sine(int k) { k&=255; return k<=64 ? skin_vis_sin[k] : k<=128 ? skin_vis_sin[128-k] : k<=192 ? -skin_vis_sin[k-128] : -skin_vis_sin[256-k]; }
#define SKIN_VIS_MUL(a,b) ((int)(short)(a)*(int)(short)(b))
static unsigned skin_vis_magnitude(int re,int im)
{
    unsigned a=(unsigned)(re<0 ? -re : re), b=(unsigned)(im<0 ? -im : im);
    return a>b ? a+(b>>1)-(b>>3) : b+(a>>1)-(a>>3); /* within 4% of hypot */
}
void skin_visual_spectrum(const signed char *pcm,unsigned char *levels)
{
    short re[128],im[128]; unsigned bins[128];
    int i,j,k,len,step;
    /* Hann window; even samples real, odd samples imaginary, bit-reversed. */
    for (i=0;i<128;++i) {
        int r=0,b=i;
        for (j=0;j<7;++j) { r=(r<<1)|(b&1); b>>=1; }
        re[r]=(short)(SKIN_VIS_MUL(pcm[2*i],skin_vis_hann[i<64 ? 2*i : 255-2*i])>>9);
        im[r]=(short)(SKIN_VIS_MUL(pcm[2*i+1],skin_vis_hann[i<64 ? 2*i+1 : 254-2*i])>>9);
    }
    for (len=2,step=64;len<=128;len<<=1,step>>=1) {
        int half=len>>1;
        for (j=0;j<half;++j) {
            int c=skin_vis_cos(2*j*step), s=-skin_vis_sine(2*j*step);
            for (i=j;i<128;i+=len) {
                int tr=(SKIN_VIS_MUL(re[i+half],c)-SKIN_VIS_MUL(im[i+half],s))>>14;
                int ti=(SKIN_VIS_MUL(re[i+half],s)+SKIN_VIS_MUL(im[i+half],c))>>14;
                int ar=re[i], ai=im[i];
                re[i]=(short)((ar+tr)>>1); im[i]=(short)((ai+ti)>>1);
                re[i+half]=(short)((ar-tr)>>1); im[i+half]=(short)((ai-ti)>>1);
            }
        }
    }
    /* Split the packed transform: X[k]=E+W^k O with E=(A+B)/2,
     * O=-i(A-B)/2, A=Z[k], B=conj(Z[128-k]). */
    for (k=1;k<128;++k) {
        int ar=re[k], ai=im[k], br=re[128-k], bi=-im[128-k];
        int er=(ar+br)>>1, ei=(ai+bi)>>1, or_=(ai-bi)>>1, oi=(br-ar)>>1;
        int c=skin_vis_cos(k), s=-skin_vis_sine(k);
        int xr=er+((SKIN_VIS_MUL(or_,c)-SKIN_VIS_MUL(oi,s))>>14);
        int xi=ei+((SKIN_VIS_MUL(or_,s)+SKIN_VIS_MUL(oi,c))>>14);
        bins[k]=skin_vis_magnitude(xr,xi);
    }
    /* Strongest bin per bar, 3 dB per pixel row. A full-scale sine is about
     * 2^12 and tops out 12 dB early, as music sits well below full scale;
     * the lowest row is near -36 dBFS. High bars get a gentle lift so
     * typical music, whose energy falls with frequency, fills the window. */
    for (i=0;i<SKIN_VIS_BARS;++i) {
        unsigned peak=0,m; int level=0;
        for (k=skin_vis_edges[i];k<skin_vis_edges[i+1];++k) if (bins[k]>peak) peak=bins[k];
        peak+=peak*(unsigned)i/12U;
        if (peak) {
            int e=0;
            for (m=peak;m>1;m>>=1) ++e;
            level=2*e+(peak*peak>=(1UL<<(2*e+1)) ? 1 : 0)-4;
        }
        levels[i]=(unsigned char)(level<0 ? 0 : level>SKIN_VIS_HEIGHT ? SKIN_VIS_HEIGHT : level);
    }
}
/* 76 columns from the 152 samples around the play position, as pixel rows
 * 0 (top) .. 15 (bottom), centre between rows 7 and 8, at twice full scale. */
void skin_visual_scope(const signed char *pcm,unsigned char *rows)
{
    int x;
    for (x=0;x<SKIN_VIS_WIDTH;++x) {
        const signed char *s=pcm+52+2*x;
        int v=(int)s[0]+s[1], row=7-(v>>4); /* x2: music sits well below full scale */
        rows[x]=(unsigned char)(row<0 ? 0 : row>15 ? 15 : row);
    }
}
/* Bars rise at once and fall at about 40 rows a second; peaks hold for
 * half a second then fall at about 16 rows a second, whatever the frame
 * rate. State is in 1/16 rows. */
void skin_visual_decay(SkinVisualDecay *d,const unsigned char *target,unsigned char *levels,
                       unsigned char *peaks,int ms)
{
    int i;
    if (ms<1) ms=1;
    if (ms>1000) ms=1000;
    for (i=0;i<SKIN_VIS_BARS;++i) {
        unsigned want=(unsigned)target[i]<<4, fall=(unsigned)ms*2U/3U+1U;
        d->bar[i]=want>=d->bar[i] ? want : d->bar[i]-want>fall ? d->bar[i]-fall : want;
        if (d->bar[i]>=d->peak[i]) { d->peak[i]=d->bar[i]; d->hold[i]=500; }
        else if (d->hold[i]>ms) d->hold[i]-=(unsigned short)ms;
        else {
            unsigned drop=(unsigned)(ms-d->hold[i])/4U+1U;
            d->hold[i]=0;
            d->peak[i]=d->peak[i]>d->bar[i]+drop ? d->peak[i]-drop : d->bar[i];
        }
        levels[i]=(unsigned char)((d->bar[i]+8)>>4);
        peaks[i]=(unsigned char)((d->peak[i]+8)>>4);
        if (peaks[i]<=levels[i]) peaks[i]=levels[i] ? levels[i] : 0;
    }
}
/* Which published playback buffer is audible at now_ms, and how far into it. */
int skin_visual_pick(const SkinVisualSlot *slots,int count,unsigned long now_ms,unsigned long *offset)
{
    int i;
    for (i=0;i<count;++i) {
        unsigned long into=(now_ms-slots[i].start_ms)&0xffffffffUL; /* 32-bit clock */
        if (!slots[i].left || !slots[i].length_ms || !slots[i].bytes) continue;
        if ((into&0x80000000UL) || into>=slots[i].length_ms) continue;
        if (offset) *offset=slots[i].length_ms<65536UL ? into*slots[i].bytes/slots[i].length_ms
                                                     : into/(slots[i].length_ms/slots[i].bytes+1);
        return i;
    }
    return -1;
}

static int near_edge(int a,int b,int tolerance)
{ int d=a-b; return d>=-tolerance && d<=tolerance; }
static int overlap(int a,int aw,int b,int bw)
{ return a<b+bw && b<a+aw; }
static int docked(const SkinRect *a,const SkinRect *b,int tolerance)
{
    if (!a->w || !b->w) return 0;
    return (overlap(a->x,a->w,b->x,b->w) &&
        (near_edge(a->y+a->h,b->y,tolerance) || near_edge(b->y+b->h,a->y,tolerance))) ||
        (overlap(a->y,a->h,b->y,b->h) &&
        (near_edge(a->x+a->w,b->x,tolerance) || near_edge(b->x+b->w,a->x,tolerance)));
}
unsigned skin_window_group(const SkinRect *r,int count,int root,int tolerance)
{
    unsigned mask,old; int i,j;
    if (!r || count<1 || count>SKIN_WINDOW_MAX || root<0 || root>=count || !r[root].w) return 0;
    mask=1U<<root;
    do { old=mask;
        for (i=0;i<count;++i) if (mask&(1U<<i))
            for (j=0;j<count;++j) if (docked(&r[i],&r[j],tolerance)) mask|=1U<<j;
    } while (old!=mask);
    return mask;
}
void skin_window_snap(const SkinRect *r,int count,int root,unsigned moving,int tolerance,int *dx,int *dy)
{
    int i,j,bestx=tolerance+1,besty=tolerance+1;
    *dx=*dy=0;
    if (!r || root<0 || root>=count || count>SKIN_WINDOW_MAX) return;
    for (i=0;i<count;++i) if ((moving&(1U<<i)) && r[i].w)
        for (j=0;j<count;++j) if (!(moving&(1U<<j)) && r[j].w) {
            int delta[4],k;
            if (overlap(r[i].y,r[i].h,r[j].y,r[j].h)) {
                delta[0]=r[j].x-r[i].x-r[i].w; delta[1]=r[j].x+r[j].w-r[i].x;
                for (k=0;k<2;++k) { int d=delta[k]<0 ? -delta[k] : delta[k]; if (d<bestx) { bestx=d; *dx=delta[k]; } }
            }
            if (overlap(r[i].x,r[i].w,r[j].x,r[j].w)) {
                delta[0]=r[j].y-r[i].y-r[i].h; delta[1]=r[j].y+r[j].h-r[i].y;
                for (k=0;k<2;++k) { int d=delta[k]<0 ? -delta[k] : delta[k]; if (d<besty) { besty=d; *dy=delta[k]; } }
            }
        }
}

void skin_clock_advance(SkinClock *c,unsigned long frames,int rate)
{
    unsigned long units;
    if (!c || rate<=0 || rate>192000 || frames>1000000UL) return;
    if (c->rate!=rate) { c->rate=rate; c->remainder=0; }
    units=frames*1000UL+c->remainder;
    c->milliseconds+=units/(unsigned long)rate;
    c->remainder=units%(unsigned long)rate;
}

void skin_settings_row(SkinSettingsRow *rows,int *count,int kind,int id,const char *label,const char *value)
{
    SkinSettingsRow *r;
    if (!rows || !count || *count<0 || *count>=SKIN_SETTINGS_MAX) return;
    r=&rows[(*count)++]; memset(r,0,sizeof(*r));
    r->kind=(short)kind; r->id=(short)id;
    if (label) strncpy(r->label,label,sizeof(r->label)-1);
    if (value) strncpy(r->value,value,sizeof(r->value)-1);
}
int skin_settings_cycle(int value,int direction,int min,int max)
{
    value+=direction<0 ? -1 : 1;
    if (value>max) value=min;
    if (value<min) value=max;
    return value;
}

/* Independent assertions for classic sprite source/destination geometry. */
#include "winamp_skin.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct Call { int id,sx,sy,w,h,x,y; } Call;
static Call calls[1024]; static int count;
static void blit(void *ctx,int id,int sx,int sy,int w,int h,int x,int y)
{
    Call *c; (void)ctx; assert(count<1024); c=&calls[count++];
    c->id=id;c->sx=sx;c->sy=sy;c->w=w;c->h=h;c->x=x;c->y=y;
}
static void fill(void *ctx,unsigned rgb,int x,int y,int w,int h)
{ (void)ctx;(void)rgb; assert(x>=0 && y>=0 && x+w<=275 && y+h<=232); }
static int has(int id,int sx,int sy,int w,int h,int x,int y)
{
    int i; for (i=0;i<count;++i) { Call *c=&calls[i];
        if (c->id==id && c->sx==sx && c->sy==sy && c->w==w && c->h==h && c->x==x && c->y==y) return 1;
    } return 0;
}
int main(void)
{
    WinampSkin skin; SkinState s; SkinEqState eq; int i;
    memset(&skin,0,sizeof(skin)); memset(&s,0,sizeof(s)); memset(&eq,0,sizeof(eq));
    for (i=0;i<SKIN_ASSET_COUNT;++i) { skin.assets[i].rgb=(unsigned char *)"x"; skin.assets[i].width=512; skin.assets[i].height=512; }
    s.active=1; s.elapsed=125; s.remaining=1; s.total=300;
    skin_render(&skin,&s,NULL,blit,fill,NULL);
    assert(has(SKIN_TITLEBAR,27,0,275,14,0,0));
    assert(has(SKIN_NUMBERS,20,6,5,1,38,32));
    assert(has(SKIN_BUTTONS,92,0,23,18,108,88));
    assert(has(SKIN_MONOSTER,29,12,27,12,212,41));
    assert(has(SKIN_MONOSTER,0,0,29,12,239,41));
    count=0; s.pressed=SKIN_NEXT; s.active=0; skin_render(&skin,&s,NULL,blit,fill,NULL);
    assert(has(SKIN_BUTTONS,92,18,22,18,108,88)); assert(has(SKIN_TITLEBAR,27,15,275,14,0,0));
    count=0; s.shaded=1; s.pressed=SKIN_SHADE; skin_render(&skin,&s,NULL,blit,fill,NULL);
    assert(has(SKIN_TITLEBAR,27,42,275,14,0,0)); assert(has(SKIN_TITLEBAR,9,27,9,9,254,3));
    count=0; eq.active=1; eq.enabled=1; eq.pressed=-5; skin_eq_render(&skin,&eq,NULL,blit,fill,NULL);
    assert(has(SKIN_EQMAIN,187,119,26,12,14,18));
    count=0; eq.pressed=-4; skin_eq_render(&skin,&eq,NULL,blit,fill,NULL);
    assert(has(SKIN_EQMAIN,224,176,44,12,217,18));
    count=0; eq.shaded=1; eq.pressed=0; skin_eq_render(&skin,&eq,NULL,blit,fill,NULL);
    assert(has(SKIN_EQ_EX,0,0,275,14,0,0));
    assert(skin_hit_test(39,32)==SKIN_TIMER && skin_playlist_hit_test(254,4)==SKIN_PLAYLIST_SHADE);
    puts("Classic sprite geometry: timer sign, transport widths, titlebars, mono/stereo, EQ controls and shade passed"); return 0;
}

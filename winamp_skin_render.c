/* Standard Winamp 2 sprite coordinates. Layout reference: Webamp's MIT-
 * licensed skinSprites.ts (captbaritone/webamp). Rendering is native C. */
#include "winamp_skin.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static int inside(int x,int y,int left,int top,int w,int h)
{ return x>=left && y>=top && x<left+w && y<top+h; }
int skin_hit_test(int x, int y)
{
    if (x<0 || y<0 || x>=SKIN_WIDTH || y>=SKIN_HEIGHT) return SKIN_NONE;
    if (inside(x,y,264,3,9,9)) return SKIN_QUIT;
    if (inside(x,y,244,3,19,9)) return SKIN_SIZE;
    if (inside(x,y,6,3,9,9)) return SKIN_SETTINGS;
    if (inside(x,y,16,88,23,18)) return SKIN_PREVIOUS;
    if (inside(x,y,39,88,23,18)) return SKIN_PLAY;
    if (inside(x,y,62,88,23,18)) return SKIN_UNSUPPORTED;
    if (inside(x,y,85,88,23,18)) return SKIN_STOP;
    if (inside(x,y,108,88,22,18)) return SKIN_NEXT;
    if (inside(x,y,136,89,22,16)) return SKIN_BROWSE;
    if (inside(x,y,107,57,68,13)) return SKIN_VOLUME_SET;
    if (inside(x,y,16,72,248,10)) return SKIN_SEEK;
    if (inside(x,y,219,58,23,12)) return SKIN_SETTINGS;
    if (inside(x,y,242,58,23,12)) return SKIN_PLAYLIST;
    if (inside(x,y,177,57,38,13) || inside(x,y,164,89,103,15)) return SKIN_UNSUPPORTED;
    if (inside(x,y,24,43,76,20)) return SKIN_RADIO;
    if (y<14) return SKIN_DRAG;
    return SKIN_NONE;
}
int skin_slider_value(int action,int x)
{
    int value=action==SKIN_VOLUME_SET ? (x-107-7)*100/54 : (x-16-14)*100/219;
    return value<0 ? 0 : value>100 ? 100 : value;
}
static void sprite(const WinampSkin *skin,SkinBlit blit,void *ctx,
                   int id,int sx,int sy,int w,int h,int x,int y)
{
    const SkinBitmap *b=&skin->assets[id];
    if (b->rgb && sx>=0 && sy>=0 && (unsigned)(sx+w)<=b->width &&
        (unsigned)(sy+h)<=b->height) blit(ctx,id,sx,sy,w,h,x,y);
}
static void text(const WinampSkin *skin,SkinBlit blit,void *ctx,
                 const char *s,int x,int y,int width,int scroll)
{
    int cell, len=(int)strlen(s), total=len+5;
    const char *punct=" .:()-'!_+\\/[]^&%,=$#";
    for (cell=0;cell<width/5+2;++cell) {
        int dx=x+cell*5-scroll%5, sx, row=0, col=30, l=0, w=5;
        int index=(cell+scroll/5)%total;
        unsigned char c=index<len ? (unsigned char)s[index] : ' ';
        const char *p;
        c=(unsigned char)toupper(c);
        if (c>='A' && c<='Z') col=c-'A';
        else if (c>='0' && c<='9') { row=1; col=c-'0'; }
        else if (c=='"') col=26;
        else if (c=='@') col=27;
        else if (c!=' ' && (p=strchr(punct,c))!=NULL) { row=1; col=10+(int)(p-punct); }
        if (dx<x) { l=x-dx; dx=x; w-=l; }
        if (dx+w>x+width) w=x+width-dx;
        sx=col*5+l;
        if (w>0) sprite(skin,blit,ctx,SKIN_TEXT,sx,row*6,w,6,dx,y);
    }
}

void skin_render(const WinampSkin *skin,const SkinState *s,const SkinState *old,
                 SkinBlit blit,SkinFill fill,void *ctx)
{
    int all=old==NULL, i;
    if (all) {
        sprite(skin,blit,ctx,SKIN_MAIN,0,0,275,116,0,0);
        sprite(skin,blit,ctx,SKIN_TITLEBAR,27,0,275,14,0,0);
        sprite(skin,blit,ctx,SKIN_SHUFREP,28,0,47,15,164,89);
        sprite(skin,blit,ctx,SKIN_SHUFREP,0,0,28,15,210,89);
        sprite(skin,blit,ctx,SKIN_BALANCE,9,13*15,38,13,177,57);
        sprite(skin,blit,ctx,SKIN_BALANCE,15,422,14,11,189,58);
    }
    if (all || s->pressed!=old->pressed || s->playlist_visible!=old->playlist_visible) {
        static const int actions[5]={SKIN_PREVIOUS,SKIN_PLAY,SKIN_UNSUPPORTED,SKIN_STOP,SKIN_NEXT};
        for (i=0;i<5;++i)
            sprite(skin,blit,ctx,SKIN_BUTTONS,i*23,s->pressed==actions[i] ? 18 : 0,
                   i==4 ? 22 : 23,18,16+i*23,88);
        sprite(skin,blit,ctx,SKIN_BUTTONS,114,s->pressed==SKIN_BROWSE ? 16 : 0,22,16,136,89);
        sprite(skin,blit,ctx,SKIN_SHUFREP,s->pressed==SKIN_SETTINGS ? 46 : 0,61,23,12,219,58);
        sprite(skin,blit,ctx,SKIN_SHUFREP,s->pressed==SKIN_PLAYLIST ? 69 : 23,
               s->playlist_visible ? 73 : 61,23,12,242,58);
    }
    if (all || s->elapsed!=old->elapsed) {
        int elapsed=s->elapsed<0 ? 0 : s->elapsed;
        int digits[4];
        if (elapsed>5999) elapsed=5999;
        digits[0]=elapsed/600; digits[1]=elapsed/60%10; digits[2]=elapsed/10%6; digits[3]=elapsed%10;
        if (skin->assets[SKIN_NUMBERS].rgb) {
            for (i=0;i<4;++i) sprite(skin,blit,ctx,SKIN_NUMBERS,digits[i]*9,0,9,13,
                                    48+i*12+(i>=2 ? 6 : 0),26);
        } else {
            char time[16]; snprintf(time,sizeof(time),"%02d:%02d",elapsed/60,elapsed%60);
            text(skin,blit,ctx,time,48,30,50,0);
        }
    }
    if (all || s->elapsed!=old->elapsed || s->total!=old->total || s->pressed!=old->pressed) {
        int pos=s->total>0 ? (int)((long)s->elapsed*219/s->total) : 0;
        if (pos<0) pos=0;
        if (pos>219) pos=219;
        if (skin->assets[SKIN_POSITION].rgb) {
            sprite(skin,blit,ctx,SKIN_POSITION,0,0,248,10,16,72);
            sprite(skin,blit,ctx,SKIN_POSITION,s->pressed==SKIN_SEEK ? 278 : 248,0,29,10,16+pos,72);
        } else { fill(ctx,0x202020,16,72,248,10); fill(ctx,0xaaaaaa,16+pos,72,29,10); }
    }
    if (all || s->volume!=old->volume || s->pressed!=old->pressed) {
        int volume=s->volume<0 ? 0 : s->volume>100 ? 100 : s->volume;
        if (skin->assets[SKIN_VOLUME].rgb) {
            sprite(skin,blit,ctx,SKIN_VOLUME,0,(volume*27/100)*15,68,13,107,57);
            sprite(skin,blit,ctx,SKIN_VOLUME,s->pressed==SKIN_VOLUME_SET ? 0 : 15,422,14,11,
                   107+volume*54/100,58);
        } else { fill(ctx,0x202020,107,57,68,13); fill(ctx,0xaaaaaa,107+volume*54/100,58,14,11); }
    }
    if (all || s->playing!=old->playing || s->mono!=old->mono) {
        sprite(skin,blit,ctx,SKIN_PLAYPAUS,s->playing ? 0 : 18,0,9,9,24,28);
        sprite(skin,blit,ctx,SKIN_MONOSTER,29,s->mono ? 0 : 12,27,12,212,41);
        sprite(skin,blit,ctx,SKIN_MONOSTER,0,s->mono ? 12 : 0,29,12,239,41);
    }
    if (all || s->bitrate!=old->bitrate || s->rate!=old->rate) {
        char number[16];
        snprintf(number,sizeof(number),"%3d",s->bitrate>999 ? 999 : s->bitrate);
        text(skin,blit,ctx,number,111,43,15,0);
        snprintf(number,sizeof(number),"%2d",s->rate/1000);
        text(skin,blit,ctx,number,156,43,10,0);
    }
    if (all || strcmp(s->title,old->title) || s->scroll!=old->scroll)
        text(skin,blit,ctx,s->title[0] ? s->title : "MINTAMP",111,27,155,
             strlen(s->title)>31 ? s->scroll : 0);
}

int skin_playlist_top(int top,int count)
{
    int maximum=count>SKIN_PLAYLIST_ROWS ? count-SKIN_PLAYLIST_ROWS : 0;
    return top<0 ? 0 : top>maximum ? maximum : top;
}
int skin_playlist_hit_test(int x,int y)
{
    if (x<0 || x>=275 || y<0 || y>=SKIN_PLAYLIST_HEIGHT) return SKIN_NONE;
    if (inside(x,y,264,3,9,9)) return SKIN_PLAYLIST_CLOSE;
    if (inside(x,y,254,3,9,9)) return SKIN_PLAYLIST_OPTIONS;
    if (y<20) return SKIN_DRAG;
    if (inside(x,y,12,22,243,170)) return SKIN_TRACK_SELECT;
    if (inside(x,y,258,20,8,174)) return SKIN_PLAYLIST_SCROLL;
    if (y>=194) return SKIN_PLAYLIST_OPTIONS;
    return SKIN_NONE;
}
void skin_playlist_render(const WinampSkin *skin,const SkinPlaylistState *s,
                          const SkinPlaylistState *old,SkinBlit blit,
                          SkinFill fill,SkinLabel label,void *ctx)
{
    int all=old==NULL, i;
    if (all) {
        for (i=25;i<250;i+=25)
            sprite(skin,blit,ctx,SKIN_PLEDIT,127,0,25,20,i,0);
        sprite(skin,blit,ctx,SKIN_PLEDIT,0,0,25,20,0,0);
        sprite(skin,blit,ctx,SKIN_PLEDIT,26,0,100,20,87,0);
        sprite(skin,blit,ctx,SKIN_PLEDIT,153,0,25,20,250,0);
        for (i=20;i<194;i+=29) {
            sprite(skin,blit,ctx,SKIN_PLEDIT,0,42,12,29,0,i);
            sprite(skin,blit,ctx,SKIN_PLEDIT,31,42,20,29,255,i);
        }
        sprite(skin,blit,ctx,SKIN_PLEDIT,0,72,125,38,0,194);
        sprite(skin,blit,ctx,SKIN_PLEDIT,126,72,150,38,125,194);
        fill(ctx,skin->playlist_background,12,20,243,174);
    }
    for (i=0;i<SKIN_PLAYLIST_ROWS;++i) {
        int index=s->top+i, old_index=old ? old->top+i : -1;
        int selected=index<s->count && index==s->selected;
        unsigned bg=selected ? skin->playlist_selected : skin->playlist_background;
        if (all || s->top!=old->top || s->count!=old->count ||
            (index==s->selected)!=(old_index==old->selected) ||
            (index==s->current)!=(old_index==old->current) || strcmp(s->rows[i],old->rows[i])) {
            char line[96];
            fill(ctx,bg,12,22+i*10,243,10);
            if (index<s->count) {
                snprintf(line,sizeof(line),"%d. %s",index+1,s->rows[i]);
                label(ctx,line,index==s->current ? skin->playlist_current : skin->playlist_normal,
                      bg,14,23+i*10,239);
            } else if (!s->count && i==0) {
                label(ctx,"List opts: add or load tracks",skin->playlist_normal,bg,14,23,239);
            }
        }
    }
    if (all || s->top!=old->top || s->count!=old->count) {
        int maximum=skin_playlist_top(s->count,s->count);
        for (i=20;i<194;i+=29)
            sprite(skin,blit,ctx,SKIN_PLEDIT,34,42,8,29,258,i);
        sprite(skin,blit,ctx,SKIN_PLEDIT,52,53,8,18,258,
               20+(maximum ? s->top*156/maximum : 0));
    }
}

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
    if (inside(x,y,254,3,9,9)) return SKIN_SHADE;
    if (inside(x,y,244,3,9,9)) return SKIN_SIZE;
    if (inside(x,y,6,3,9,9)) return SKIN_SETTINGS;
    if (inside(x,y,16,88,23,18)) return SKIN_PREVIOUS;
    if (inside(x,y,39,88,23,18)) return SKIN_PLAY;
    if (inside(x,y,62,88,23,18)) return SKIN_PAUSE;
    if (inside(x,y,85,88,23,18)) return SKIN_STOP;
    if (inside(x,y,108,88,22,18)) return SKIN_NEXT;
    if (inside(x,y,136,89,22,16)) return SKIN_BROWSE;
    if (inside(x,y,107,57,68,13)) return SKIN_VOLUME_SET;
    if (inside(x,y,16,72,248,10)) return SKIN_SEEK;
    if (inside(x,y,219,58,23,12)) return SKIN_EQ;
    if (inside(x,y,242,58,23,12)) return SKIN_PLAYLIST;
    if (inside(x,y,177,57,38,13)) return SKIN_BALANCE_SET;
    if (inside(x,y,164,89,47,15)) return SKIN_SHUFFLE;
    if (inside(x,y,210,89,28,15)) return SKIN_REPEAT;
    if (inside(x,y,48,26,51,13)) return SKIN_TIMER;
    if (inside(x,y,24,43,76,20)) return SKIN_VISUAL;
    if (y<14) return SKIN_DRAG;
    return SKIN_NONE;
}
int skin_slider_value(int action,int x)
{
    int value;
    if (action==SKIN_BALANCE_SET) {
        value=(x-177-7)*200/24-100;
        return value<-100 ? -100 : value>100 ? 100 : value;
    }
    value=action==SKIN_VOLUME_SET ? (x-107-7)*100/54 : (x-16-14)*100/219;
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
    if (s->shaded) {
        if (all || !old->shaded || strcmp(s->title,old->title)) {
            if (skin->assets[SKIN_TITLEBAR].height>=56)
                sprite(skin,blit,ctx,SKIN_TITLEBAR,27,29,275,14,0,0);
            else { fill(ctx,0x202020,0,0,275,14); text(skin,blit,ctx,"MINTAMP",20,4,220,0); }
        }
        return;
    }
    if (old && old->shaded) all=1;
    if (all) {
        sprite(skin,blit,ctx,SKIN_MAIN,0,0,275,116,0,0);
        sprite(skin,blit,ctx,SKIN_TITLEBAR,27,0,275,14,0,0);
        sprite(skin,blit,ctx,SKIN_SHUFREP,28,0,47,15,164,89);
        sprite(skin,blit,ctx,SKIN_SHUFREP,0,0,28,15,210,89);
        sprite(skin,blit,ctx,SKIN_BALANCE,9,13*15,38,13,177,57);
        sprite(skin,blit,ctx,SKIN_BALANCE,15,422,14,11,189,58);
    }
    if (all || s->pressed!=old->pressed || s->playlist_visible!=old->playlist_visible || s->eq_visible!=old->eq_visible) {
        static const int actions[5]={SKIN_PREVIOUS,SKIN_PLAY,SKIN_PAUSE,SKIN_STOP,SKIN_NEXT};
        for (i=0;i<5;++i)
            sprite(skin,blit,ctx,SKIN_BUTTONS,i*23,s->pressed==actions[i] ? 18 : 0,
                   i==4 ? 22 : 23,18,16+i*23,88);
        sprite(skin,blit,ctx,SKIN_BUTTONS,114,s->pressed==SKIN_BROWSE ? 16 : 0,22,16,136,89);
        sprite(skin,blit,ctx,SKIN_SHUFREP,s->pressed==SKIN_EQ ? 46 : 0,s->eq_visible ? 73 : 61,23,12,219,58);
        sprite(skin,blit,ctx,SKIN_SHUFREP,s->pressed==SKIN_PLAYLIST ? 69 : 23,
               s->playlist_visible ? 73 : 61,23,12,242,58);
    }
    if (all || s->elapsed!=old->elapsed || s->remaining!=old->remaining || s->total!=old->total) {
        int elapsed=s->remaining && s->total>0 ? s->total-s->elapsed : s->elapsed;
        if (elapsed<0) elapsed=0;
        fill(ctx,0x000000,36,26,9,13);
        if (s->remaining && s->total>0) fill(ctx,0x00ff00,38,32,5,1);
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
    if (all || s->balance!=old->balance || s->pressed!=old->pressed) {
        int balance=s->balance<-100 ? -100 : s->balance>100 ? 100 : s->balance;
        int frame=(balance<0 ? -balance : balance)*13/100;
        if (skin->assets[SKIN_BALANCE].rgb) {
            sprite(skin,blit,ctx,SKIN_BALANCE,9,frame*15,38,13,177,57);
            sprite(skin,blit,ctx,SKIN_BALANCE,s->pressed==SKIN_BALANCE_SET ? 0 : 15,422,14,11,
                   177+(balance+100)*24/200,58);
        } else { fill(ctx,0x202020,177,57,38,13); fill(ctx,0xaaaaaa,177+(balance+100)*24/200,58,14,11); }
    }
    if (all || s->shuffle!=old->shuffle || s->repeat!=old->repeat || s->pressed!=old->pressed) {
        sprite(skin,blit,ctx,SKIN_SHUFREP,28,(s->shuffle ? 30 : 0)+(s->pressed==SKIN_SHUFFLE ? 15 : 0),47,15,164,89);
        sprite(skin,blit,ctx,SKIN_SHUFREP,0,(s->repeat ? 30 : 0)+(s->pressed==SKIN_REPEAT ? 15 : 0),28,15,210,89);
        sprite(skin,blit,ctx,SKIN_MAIN,240,89,7,15,240,89);
        if (s->repeat==2) text(skin,blit,ctx,"1",240,94,5,0);
    }
    if (all || s->visual_mode!=old->visual_mode || memcmp(s->levels,old->levels,16) || memcmp(s->scope,old->scope,64)) {
        fill(ctx,0x000000,24,43,76,16);
        if (s->visual_mode==1) {
            for (i=0;i<16;++i) { int h=s->levels[i]>15 ? 15 : s->levels[i];
                if (h) fill(ctx,0x00bb00,24+i*4,59-h,3,h); }
        } else if (s->visual_mode==2) {
            for (i=0;i<64;++i) fill(ctx,0x00cc00,24+i,51+s->scope[i]/16,1,1);
        }
    }
    if (all || s->playing!=old->playing || s->mono!=old->mono || s->paused!=old->paused) {
        sprite(skin,blit,ctx,SKIN_PLAYPAUS,s->paused ? 9 : s->playing ? 0 : 18,0,9,9,24,28);
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
    if (inside(x,y,14,203,22,18)) return SKIN_LIST_ADD;
    if (inside(x,y,43,203,22,18)) return SKIN_LIST_REM;
    if (inside(x,y,72,203,22,18)) return SKIN_LIST_SEL;
    if (inside(x,y,101,203,22,18)) return SKIN_LIST_MISC;
    if (inside(x,y,228,203,23,18)) return SKIN_LIST_FILE;
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
        int selected=index<s->count && s->selection[index];
        unsigned bg=selected ? skin->playlist_selected : skin->playlist_background;
        if (all || s->top!=old->top || s->count!=old->count ||
            (index<128 && s->selection[index])!=(old_index>=0 && old_index<128 && old->selection[old_index]) ||
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
    if (all || s->total_seconds!=old->total_seconds || s->unknown_durations!=old->unknown_durations) {
        char total[24];
        snprintf(total,sizeof(total),"%ld:%02d%s",(long)s->total_seconds/60,s->total_seconds%60,s->unknown_durations ? "+" : "");
        fill(ctx,skin->playlist_background,143,204,80,8);
        label(ctx,total,skin->playlist_normal,skin->playlist_background,143,204,80);
    }

    if (all || s->top!=old->top || s->count!=old->count) {
        int maximum=skin_playlist_top(s->count,s->count);
        for (i=20;i<194;i+=29)
            sprite(skin,blit,ctx,SKIN_PLEDIT,34,42,8,29,258,i);
        sprite(skin,blit,ctx,SKIN_PLEDIT,52,53,8,18,258,
               20+(maximum ? s->top*156/maximum : 0));
    }
}

void skin_eq_render(const WinampSkin *skin,const SkinEqState *s,const SkinEqState *old,
                    SkinBlit blit,SkinFill fill,void *ctx)
{
    int i,all=!old;
    (void)fill;
    if (all) { sprite(skin,blit,ctx,SKIN_EQMAIN,0,0,275,116,0,0);
        sprite(skin,blit,ctx,SKIN_EQMAIN,0,134,275,14,0,0); }
    if (all || s->enabled!=old->enabled)
        sprite(skin,blit,ctx,SKIN_EQMAIN,s->enabled ? 69 : 10,119,26,12,14,18);
    for (i=0;i<11;++i) {
        int db=i ? s->bands[i-1] : s->preamp;
        int x=i ? 78+(i-1)*18 : 21;
        int olddb=old ? (i ? old->bands[i-1] : old->preamp) : 99;
        int frame=(12-db)*27/24;
        if (all || db!=olddb || s->pressed!=old->pressed) {
            if (frame<0) frame=0;
            if (frame>27) frame=27;
            sprite(skin,blit,ctx,SKIN_EQMAIN,13+(frame%14)*15,164+(frame/14)*65,14,63,x-1,38);
            sprite(skin,blit,ctx,SKIN_EQMAIN,0,s->pressed==i+1 ? 176 : 164,11,11,x,38+(12-db)*50/24);
        }
    }
}

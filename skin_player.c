/* Shared Intuition window for the SGT and SR editions. Native utility
 * windows remain owned by the respective GadTools/ReAction frontend. */
#include "skin_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <graphics/scale.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <libraries/asl.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <proto/icon.h>

extern struct GfxBase *GfxBase;

typedef struct SkinResources {
    WinampSkin skin;
    struct BitMap *bitmaps[SKIN_ASSET_COUNT];
    int modern[SKIN_ASSET_COUNT];
    LONG pens[4096];
    UWORD owned[128];
    int owned_count, scale;
} SkinResources;

struct SkinPlayer {
    struct Screen *screen;
    struct Window *win, *plwin, *eqwin, *target;
    struct BitMap *text_source, *text_scaled;
    struct TextFont *playlist_font;
    struct MsgPort *visual_port;
    struct timerequest *visual_timer;
    int visual_device,visual_pending;
    SkinResources *resources;
    struct Menu menu;
    struct MenuItem items[12];
    struct IntuiText labels[12];
    struct Menu playlist_menu;
    struct MenuItem playlist_items[12];
    struct IntuiText playlist_labels[12];
    SkinState now, drawn;
    SkinOrderQueue sequence;
    SkinSelection selection;
    SkinEqState eq_drawn;
    int eq_have_drawn,eq_pressed,eq_drag_x,eq_drag_y;
    int remaining,shaded,visual_mode,visual_interval,visual_ticks,debug;
    int pl_left,pl_top,eq_left,eq_top,pl_position,eq_position;
    int move_row,move_target,move_active;
    int saved_left,saved_top,restore_playlist,restore_eq;
    unsigned drag_group;
    unsigned long playlist_identity;
    int preserve_selection;
    int have_drawn, pressed, drag_x, drag_y,seek_preview;
    int notice_ticks;
    SkinPlaylistState playlist, playlist_drawn;
    int playlist_have_drawn, playlist_pressed, playlist_drag_x, playlist_drag_y;
    int click_row, pending_options,pending_double;
    ULONG click_seconds, click_micros;
    const int *durations;
    SkinPlaylistName playlist_name;
    void *playlist_ctx;
    char notice[256], name[32], path[512];
};

static void save_state(SkinPlayer *p);

static int suffix(const char *path,const char *ext)
{
    size_t n=strlen(path), e=strlen(ext), i;
    if (n<e) return 0;
    for (i=0;i<e;++i) if (tolower((unsigned char)path[n-e+i])!=ext[i]) return 0;
    return 1;
}
int skin_player_is_skin(const char *path) { return suffix(path,".wsz") || suffix(path,".zip"); }
const char *skin_player_audio_arg(int argc,char **argv)
{
    int i;
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--skin")) { ++i; continue; }
        if (argv[i][0]!='-' && !skin_player_is_skin(argv[i])) return argv[i];
    }
    return NULL;
}
static void error_request(SkinPlayer *p,const char *message)
{
    struct EasyStruct es;
    memset(&es,0,sizeof(es)); es.es_StructSize=sizeof(es);
    es.es_Title=(STRPTR)p->name; es.es_TextFormat=(STRPTR)message;
    es.es_GadgetFormat=(STRPTR)"OK";
    /* Format is fixed: a skin error must never become a printf format. */
    es.es_TextFormat=(STRPTR)"%s";
    EasyRequestArgs(p->win,&es,NULL,(APTR)&message);
}
static void env_name(SkinPlayer *p,char *key,const char *option)
{ snprintf(key,64,"%s/%s",p->name,option); }
static void save_choice(SkinPlayer *p)
{
    char key[64], size[2];
    env_name(p,key,"Skin"); SetVar(key,p->path,-1,GVF_GLOBAL_ONLY|GVF_SAVE_VAR);
    size[0]=(char)('0'+p->resources->scale); size[1]=0;
    env_name(p,key,"Scale"); SetVar(key,size,-1,GVF_GLOBAL_ONLY|GVF_SAVE_VAR);
}
static LONG color_pen(SkinPlayer *p,SkinResources *r,unsigned rgb)
{
    unsigned key=((rgb>>12)&0xf00)|((rgb>>8)&0xf0)|((rgb>>4)&15);
    unsigned red=(rgb>>16)&255, green=(rgb>>8)&255, blue=rgb&255;
    LONG pen;
    if (r->pens[key]>=0) return r->pens[key];
    if (GfxBase->LibNode.lib_Version>=39) {
        ULONG rr=red*0x01010101UL, gg=green*0x01010101UL, bb=blue*0x01010101UL;
        pen=r->owned_count<128 ? ObtainBestPen(p->screen->ViewPort.ColorMap,rr,gg,bb,
              OBP_Precision,PRECISION_IMAGE,TAG_DONE) : -1;
        if (pen>=0) r->owned[r->owned_count++]=(UWORD)pen;
        else pen=FindColor(p->screen->ViewPort.ColorMap,rr,gg,bb,-1);
    } else {
        unsigned count=p->screen->ViewPort.ColorMap->Count, i; unsigned long best=~0UL;
        pen=0;
        for (i=0;i<count;++i) {
            ULONG c=GetRGB4(p->screen->ViewPort.ColorMap,i);
            long dr=(long)red-((c>>8)&15)*17, dg=(long)green-((c>>4)&15)*17, db=(long)blue-(c&15)*17;
            unsigned long distance=(unsigned long)(dr*dr+dg*dg+db*db);
            if (distance<best) { best=distance; pen=(LONG)i; }
        }
    }
    if (pen<0) pen=0;
    r->pens[key]=pen; return pen;
}
static void free_resources(SkinPlayer *p,SkinResources *r)
{
    int i,j;
    if (!r) return;
    WaitBlit();
    for (i=0;i<SKIN_ASSET_COUNT;++i) if (r->bitmaps[i]) {
        if (r->modern[i]) FreeBitMap(r->bitmaps[i]);
        else {
            for (j=0;j<r->bitmaps[i]->Depth;++j) if (r->bitmaps[i]->Planes[j])
                FreeRaster(r->bitmaps[i]->Planes[j],r->skin.assets[i].width*r->scale,
                           r->skin.assets[i].height*r->scale);
            free(r->bitmaps[i]);
        }
    }
    for (i=0;i<r->owned_count;++i) ReleasePen(p->screen->ViewPort.ColorMap,r->owned[i]);
    skin_free(&r->skin); free(r);
}
static SkinResources *load_resources(SkinPlayer *p,const char *path,int scale,char *error)
{
    SkinResources *r=(SkinResources *)calloc(1,sizeof(*r));
    int i,j; unsigned x,y,depth=p->screen->RastPort.BitMap->Depth;
    if (!r) { strcpy(error,"Not enough memory for skin"); return NULL; }
    r->scale=scale;
    for (i=0;i<4096;++i) r->pens[i]=-1;
    if (!skin_load_file(&r->skin,path,error,160)) { free(r); return NULL; }
    for (i=0;i<SKIN_ASSET_COUNT;++i) if (r->skin.assets[i].rgb) {
        SkinBitmap *b=&r->skin.assets[i];
        struct RastPort rp;
        if (GfxBase->LibNode.lib_Version>=39) {
            r->modern[i]=1;
            r->bitmaps[i]=AllocBitMap(b->width*scale,b->height*scale,depth,BMF_CLEAR,
                                    p->screen->RastPort.BitMap);
        } else {
            r->bitmaps[i]=(struct BitMap *)calloc(1,sizeof(struct BitMap));
            if (r->bitmaps[i]) {
                InitBitMap(r->bitmaps[i],depth,b->width*scale,b->height*scale);
                for (j=0;j<(int)depth;++j) {
                    r->bitmaps[i]->Planes[j]=AllocRaster(b->width*scale,b->height*scale);
                    if (!r->bitmaps[i]->Planes[j]) break;
                }
                if (j!=(int)depth) goto no_memory;
            }
        }
        if (!r->bitmaps[i]) goto no_memory;
        InitRastPort(&rp); rp.BitMap=r->bitmaps[i]; SetDrMd(&rp,JAM1);
        /* Paint equal-colour runs once, rather than decoding/converting any
         * images in the playback loop. Blits later copy tiny changed areas. */
        for (y=0;y<b->height;++y) {
            x=0;
            while (x<b->width) {
                unsigned start=x, rgb; LONG pen;
                const unsigned char *pixel=b->rgb+((size_t)y*b->width+x)*3;
                rgb=((unsigned)pixel[0]<<16)|((unsigned)pixel[1]<<8)|pixel[2];
                pen=color_pen(p,r,rgb); ++x;
                while (x<b->width) {
                    pixel=b->rgb+((size_t)y*b->width+x)*3;
                    rgb=((unsigned)pixel[0]<<16)|((unsigned)pixel[1]<<8)|pixel[2];
                    if (color_pen(p,r,rgb)!=pen) break;
                    ++x;
                }
                SetAPen(&rp,(ULONG)pen);
                RectFill(&rp,start*scale,y*scale,x*scale-1,(y+1)*scale-1);
            }
        }
    }
    return r;
no_memory:
    strcpy(error,"Not enough graphics memory to cache skin"); free_resources(p,r); return NULL;
}
static void blit(void *ctx,int id,int sx,int sy,int w,int h,int x,int y)
{
    SkinPlayer *p=(SkinPlayer *)ctx; int s=p->resources->scale;
    if (p->target && p->resources->bitmaps[id])
        BltBitMapRastPort(p->resources->bitmaps[id],sx*s,sy*s,p->target->RPort,x*s,y*s,w*s,h*s,0xc0);
}
static void fill(void *ctx,unsigned rgb,int x,int y,int w,int h)
{
    SkinPlayer *p=(SkinPlayer *)ctx; int s=p->resources->scale;
    SetAPen(p->target->RPort,(ULONG)color_pen(p,p->resources,rgb));
    RectFill(p->target->RPort,x*s,y*s,(x+w)*s-1,(y+h)*s-1);
}
static void window_rects(SkinPlayer *p,SkinRect *r,struct Window **windows)
{
    int i;
    windows[0]=p->win; windows[1]=p->plwin; windows[2]=p->eqwin;
    memset(r,0,3*sizeof(*r));
    for (i=0;i<3;++i) if (windows[i]) {
        r[i].x=windows[i]->LeftEdge; r[i].y=windows[i]->TopEdge;
        r[i].w=windows[i]->Width; r[i].h=windows[i]->Height;
    }
}
static void drag_begin(SkinPlayer *p,int root,UWORD qualifier)
{
    SkinRect r[3]; struct Window *windows[3];
    window_rects(p,r,windows);
    p->drag_group=(qualifier&IEQUALIFIER_CONTROL) ? 1U<<root : skin_window_group(r,3,root,2*p->resources->scale);
}
static void drag_move(SkinPlayer *p,int dx,int dy)
{
    SkinRect r[3]; struct Window *windows[3]; int i;
    window_rects(p,r,windows);
    for (i=0;i<3;++i) if (p->drag_group&(1U<<i)) {
        if (r[i].x+dx<0) dx=-r[i].x;
        if (r[i].y+dy<0) dy=-r[i].y;
        if (r[i].x+r[i].w+dx>p->screen->Width) dx=p->screen->Width-r[i].x-r[i].w;
        if (r[i].y+r[i].h+dy>p->screen->Height) dy=p->screen->Height-r[i].y-r[i].h;
    }
    for (i=0;i<3;++i) if ((p->drag_group&(1U<<i)) && windows[i]) MoveWindow(windows[i],dx,dy);
}
static void drag_end(SkinPlayer *p,int root)
{
    SkinRect r[3]; struct Window *windows[3]; int dx,dy;
    window_rects(p,r,windows);
    skin_window_snap(r,3,root,p->drag_group,8*p->resources->scale,&dx,&dy);
    drag_move(p,dx,dy); p->drag_group=0;
}
static void repaint(SkinPlayer *p,int all)
{
    SkinState display;
    if (!p->win || !p->resources) return;
    p->target=p->win;
    p->now.pressed=p->pressed;
    p->now.playlist_visible=p->plwin!=NULL;
    p->now.eq_visible=p->eqwin!=NULL;
    display=p->now;
    if (p->pressed==SKIN_SEEK && display.total>0) display.elapsed=(int)((long)display.total*p->seek_preview/100);
    skin_render(&p->resources->skin,&display,(all || !p->have_drawn) ? NULL : &p->drawn,blit,fill,p);
    p->drawn=display; p->have_drawn=1;
}
static struct BitMap *text_bitmap(SkinPlayer *p,int width,int height)
{
    struct BitMap *bm; int i;
    if (GfxBase->LibNode.lib_Version>=39)
        return AllocBitMap(width,height,p->screen->RastPort.BitMap->Depth,
                           BMF_CLEAR,p->screen->RastPort.BitMap);
    bm=(struct BitMap *)calloc(1,sizeof(*bm));
    if (!bm) return NULL;
    InitBitMap(bm,p->screen->RastPort.BitMap->Depth,width,height);
    for (i=0;i<bm->Depth;++i) {
        bm->Planes[i]=AllocRaster(width,height);
        if (!bm->Planes[i]) {
            while (i--) FreeRaster(bm->Planes[i],width,height);
            free(bm); return NULL;
        }
    }
    return bm;
}
static void free_text_bitmap(struct BitMap *bm,int width,int height)
{
    int i;
    if (!bm) return;
    WaitBlit();
    if (GfxBase->LibNode.lib_Version>=39) { FreeBitMap(bm); return; }
    for (i=0;i<bm->Depth;++i) FreeRaster(bm->Planes[i],width,height);
    free(bm);
}
static void playlist_label(void *ctx,const char *text,unsigned rgb,unsigned bg,int x,int y,int width)
{
    SkinPlayer *p=(SkinPlayer *)ctx; struct RastPort rp;
    struct BitScaleArgs args; char line[96]; int i,n,scale=p->resources->scale;
    n=(int)strlen(text); if (n>width/8) n=width/8;
    for (i=0;i<n;++i) line[i]=(unsigned char)text[i]<32 ? ' ' : text[i];
    InitRastPort(&rp); rp.BitMap=p->text_source; SetFont(&rp,p->playlist_font);
    SetAPen(&rp,(ULONG)color_pen(p,p->resources,bg)); RectFill(&rp,0,0,242,7);
    SetDrMd(&rp,JAM1); SetAPen(&rp,(ULONG)color_pen(p,p->resources,rgb));
    Move(&rp,0,p->playlist_font->tf_Baseline); Text(&rp,line,n);
    if (scale==2) {
        memset(&args,0,sizeof(args));
        args.bsa_SrcWidth=width; args.bsa_SrcHeight=8;
        args.bsa_XSrcFactor=args.bsa_YSrcFactor=1;
        args.bsa_XDestFactor=args.bsa_YDestFactor=2;
        args.bsa_SrcBitMap=p->text_source; args.bsa_DestBitMap=p->text_scaled;
        WaitBlit(); BitMapScale(&args);
    }
    BltBitMapRastPort(scale==2 ? p->text_scaled : p->text_source,0,0,
                     p->plwin->RPort,x*scale,y*scale,width*scale,8*scale,0xc0);
    /* The next row reuses the source, and must not race a queued blit. */
    WaitBlit();
}
static void playlist_repaint(SkinPlayer *p,int all)
{
    if (!p->plwin) return;
    p->target=p->plwin;
    skin_playlist_render(&p->resources->skin,&p->playlist,
        all || !p->playlist_have_drawn ? NULL : &p->playlist_drawn,
        blit,fill,playlist_label,p);
    p->playlist_drawn=p->playlist; p->playlist_have_drawn=1;
}
static void playlist_close(SkinPlayer *p)
{
    struct Message *m;
    if (!p->plwin) return;
    ClearMenuStrip(p->plwin);
    while ((m=GetMsg(p->plwin->UserPort))!=NULL) ReplyMsg(m);
    p->pl_left=p->plwin->LeftEdge; p->pl_top=p->plwin->TopEdge; p->pl_position=1;
    CloseWindow(p->plwin); p->plwin=NULL;
    p->playlist_pressed=0; p->click_row=-1; p->playlist_have_drawn=0;
    repaint(p,0);
}
static int playlist_open(SkinPlayer *p)
{
    struct TextAttr font={(STRPTR)"topaz.font",8,FS_NORMAL,FPF_ROMFONT};
    int scale=p->resources->scale, left,top,i;
    if (!p->win || !p->resources->skin.assets[SKIN_PLEDIT].rgb ||
        275*scale>p->screen->Width || SKIN_PLAYLIST_HEIGHT*scale>p->screen->Height) return 0;
    if (!p->playlist_font) p->playlist_font=OpenFont(&font);
    if (!p->text_source) p->text_source=text_bitmap(p,243,8);
    if (!p->text_scaled) p->text_scaled=text_bitmap(p,486,16);
    if (!p->playlist_font || !p->text_source || !p->text_scaled) return 0;
    left=p->pl_position ? p->pl_left : p->win->LeftEdge; top=p->pl_position ? p->pl_top : p->win->TopEdge+p->win->Height;
    if (left<0) left=0;
    if (top<0) top=0;
    if (left+275*scale>p->screen->Width) left=p->screen->Width-275*scale;
    if (top+SKIN_PLAYLIST_HEIGHT*scale>p->screen->Height) top=p->screen->Height-SKIN_PLAYLIST_HEIGHT*scale;
    p->plwin=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,
        WA_Left,left,WA_Top,top,WA_Width,275*scale,WA_Height,SKIN_PLAYLIST_HEIGHT*scale,
        WA_Borderless,TRUE,WA_Activate,TRUE,WA_RMBTrap,FALSE,
        WA_DetailPen,p->win->DetailPen,WA_BlockPen,p->win->BlockPen,
        WA_NewLookMenus,(ULONG)(GfxBase->LibNode.lib_Version>=39),
        WA_ReportMouse,TRUE,WA_SimpleRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|
                   IDCMP_MENUPICK|IDCMP_VANILLAKEY|IDCMP_RAWKEY|IDCMP_INACTIVEWINDOW,TAG_DONE);
    if (!p->plwin) return 0;
    /* Intuition may annotate menu structures. Each window owns its strip. */
    p->playlist_menu=p->menu; p->playlist_menu.FirstItem=p->playlist_items;
    for (i=0;i<12;++i) {
        p->playlist_items[i]=p->items[i]; p->playlist_labels[i]=p->labels[i];
        p->playlist_items[i].NextItem=i<11 ? &p->playlist_items[i+1] : NULL;
        p->playlist_items[i].ItemFill=&p->playlist_labels[i];
    }
    SetMenuStrip(p->plwin,&p->playlist_menu); p->click_row=-1;
    playlist_repaint(p,1); repaint(p,0); return 1;
}
int skin_player_playlist_toggle(SkinPlayer *p)
{
    if (!p) return 0;
    if (p->plwin) { playlist_close(p); save_state(p); return 1; }
    { int ok=playlist_open(p); save_state(p); return ok; }
}
void skin_player_playlist_durations(SkinPlayer *p,const int *durations)
{ if (p) p->durations=durations; }
void skin_player_playlist_update(SkinPlayer *p,int count,int selected,int current,
                                SkinPlaylistName name,void *ctx)
{
    int i, top;
    if (!p) return;
    p->playlist_name=name; p->playlist_ctx=ctx;
    top=skin_playlist_top(p->playlist.top,count);
    if (selected!=p->playlist.selected && selected>=0 && selected<count) {
        if (selected<top) top=selected;
        if (selected>=top+SKIN_PLAYLIST_ROWS) top=selected-SKIN_PLAYLIST_ROWS+1;
    }
    if (count!=p->playlist.count || selected!=p->playlist.selected || current!=p->playlist.current)
        p->click_row=-1;
    skin_order_queue_observe(&p->sequence,count,current);
    skin_selection_sync(&p->selection,count,selected);
    memcpy(p->playlist.selection,p->selection.bits,sizeof(p->playlist.selection));
    p->playlist.count=count; p->playlist.selected=selected; p->playlist.current=current;
    p->playlist.top=skin_playlist_top(top,count);
    p->playlist.total_seconds=0; p->playlist.unknown_durations=0;
    for (i=0;i<count;++i) {
        if (p->durations && p->durations[i]>=0) p->playlist.total_seconds+=p->durations[i];
        else ++p->playlist.unknown_durations;
    }
    for (i=0;i<SKIN_PLAYLIST_ROWS;++i) {
        int index=p->playlist.top+i;
        const char *s=index<count && name ? name(ctx,index) : "";
        if (strncmp(p->playlist.rows[i],s,79)) p->click_row=-1;
        if (index<count && p->durations && p->durations[index]>=0)
            snprintf(p->playlist.rows[i],80,"%.60s  %d:%02d",s,p->durations[index]/60,p->durations[index]%60);
        else { strncpy(p->playlist.rows[i],s,79); p->playlist.rows[i][79]=0; }
    }
    playlist_repaint(p,0);
}
static void close_window(SkinPlayer *p)
{
    struct Message *m;
    if (!p->win) return;
    ClearMenuStrip(p->win);
    while ((m=GetMsg(p->win->UserPort))!=NULL) ReplyMsg(m);
    CloseWindow(p->win); p->win=NULL;
}
static void build_menu(SkinPlayer *p,UWORD text_pen,UWORD background_pen)
{
    static const char * const labels[]={"Load skin...","Double size","Settings","Internet Radio","Playlist","Full playlist options...","Open audio...","Quit","Equaliser","Visualisation","Visual rate","Playback statistics"};
    int i, h=p->screen->Font->ta_YSize+4, width=0;
    memset(&p->menu,0,sizeof(p->menu));
    p->menu.Flags=MENUENABLED; p->menu.MenuName=(STRPTR)"MintAMP";
    p->menu.Width=TextLength(&p->screen->RastPort,p->menu.MenuName,strlen((const char *)p->menu.MenuName))+16;
    p->menu.Height=h; p->menu.FirstItem=p->items;
    for (i=0;i<12;++i) {
        int w=TextLength(&p->screen->RastPort,(STRPTR)labels[i],strlen(labels[i]))+16;
        if (w>width) width=w;
    }
    for (i=0;i<12;++i) {
        memset(&p->items[i],0,sizeof(p->items[i])); memset(&p->labels[i],0,sizeof(p->labels[i]));
        p->items[i].NextItem=i<11 ? &p->items[i+1] : NULL;
        p->items[i].TopEdge=i*h; p->items[i].Width=width; p->items[i].Height=h;
        p->items[i].Flags=ITEMTEXT|ITEMENABLED|HIGHCOMP;
        p->items[i].ItemFill=&p->labels[i]; p->items[i].NextSelect=MENUNULL;
        p->labels[i].FrontPen=(UBYTE)text_pen;
        p->labels[i].BackPen=(UBYTE)background_pen; p->labels[i].DrawMode=JAM1;
        p->labels[i].ITextFont=p->screen->Font;
        p->labels[i].LeftEdge=4; p->labels[i].TopEdge=2; p->labels[i].IText=(UBYTE *)labels[i];
    }
}
static int open_window(SkinPlayer *p,WORD left,WORD top)
{
    int scale=p->resources->scale;
    struct DrawInfo *dri;
    UWORD text_pen=p->screen->DetailPen, background_pen=p->screen->BlockPen;
    if (SKIN_WIDTH*scale>p->screen->Width || SKIN_HEIGHT*scale>p->screen->Height) return 0;
    if (left<0) left=0;
    if (top<0) top=0;
    if (left+275*scale>p->screen->Width) left=p->screen->Width-275*scale;
    if (top+116*scale>p->screen->Height) top=p->screen->Height-116*scale;
    /* Menus use window pens even on a borderless window. Zero defaults make
     * the menu title disappear. V39 adds dedicated menu-bar pens; V37/38
     * have the standard text/background pens. Never assume palette indices. */
    dri=GetScreenDrawInfo(p->screen);
    if (dri) {
        text_pen=dri->dri_Pens[dri->dri_NumPens>BARDETAILPEN ? BARDETAILPEN : TEXTPEN];
        background_pen=dri->dri_Pens[dri->dri_NumPens>BARBLOCKPEN ? BARBLOCKPEN : BACKGROUNDPEN];
        FreeScreenDrawInfo(p->screen,dri);
    }
    p->win=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,
        WA_Left,(ULONG)left,WA_Top,(ULONG)top,
        WA_Width,SKIN_WIDTH*scale,WA_Height,SKIN_HEIGHT*scale,
        WA_Borderless,TRUE,WA_Activate,TRUE,WA_RMBTrap,FALSE,
        WA_DetailPen,(ULONG)text_pen,WA_BlockPen,(ULONG)background_pen,
        WA_NewLookMenus,(ULONG)(GfxBase->LibNode.lib_Version>=39),
        WA_ReportMouse,TRUE,WA_SimpleRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|
                   IDCMP_MENUPICK|IDCMP_VANILLAKEY|IDCMP_INACTIVEWINDOW,
        TAG_DONE);
    if (!p->win) return 0;
    build_menu(p,text_pen,background_pen); SetMenuStrip(p->win,&p->menu);
    p->have_drawn=0; repaint(p,1); return 1;
}
static int request_path(SkinPlayer *p,char *path)
{
    struct FileRequester *fr=(struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText,(ULONG)"Select a classic Winamp skin",
        ASLFR_DoPatterns,TRUE,ASLFR_InitialPattern,(ULONG)"#?.(wsz|zip)",
        ASLFR_InitialDrawer,(ULONG)"PROGDIR:Skins",TAG_DONE);
    int ok=0;
    if (!fr) return 0;
    if (AslRequestTags(fr,ASLFR_Window,(ULONG)p->win,ASLFR_SleepWindow,TRUE,TAG_DONE)) {
        strncpy(path,(const char *)fr->fr_Drawer,511); path[511]=0;
        ok=AddPart(path,fr->fr_File,512)!=0;
    }
    FreeAslRequest(fr); return ok;
}
static void save_state(SkinPlayer *p)
{
    char key[64],text[512]; int i,n;
    if (!p->win) return;
    if (p->plwin) { p->pl_left=p->plwin->LeftEdge; p->pl_top=p->plwin->TopEdge; p->pl_position=1; }
    if (p->eqwin) { p->eq_left=p->eqwin->LeftEdge; p->eq_top=p->eqwin->TopEdge; p->eq_position=1; }
    n=snprintf(text,sizeof(text),"2 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
        p->win->LeftEdge,p->win->TopEdge,p->pl_left,p->pl_top,p->eq_left,p->eq_top,
        p->sequence.order.shuffle,p->sequence.order.repeat,gSkinAudio.balance,p->remaining,p->shaded,
        p->visual_mode,p->visual_interval,p->debug,gSkinAudio.eq_enabled,gSkinAudio.eq_preamp);
    for (i=0;i<10 && n>0 && n<(int)sizeof(text)-16;++i)
        n+=snprintf(text+n,sizeof(text)-n," %d",gSkinAudio.eq_bands[i]);
    n+=snprintf(text+n,sizeof(text)-n," %d %d %d %d",p->plwin!=NULL,p->eqwin!=NULL,p->pl_position,p->eq_position);
    env_name(p,key,"State"); SetVar(key,text,-1,GVF_GLOBAL_ONLY|GVF_SAVE_VAR);
}
static void restore_state(SkinPlayer *p)
{
    char key[64],text[512],*at,*end; long v[27]; int i;
    p->saved_left=40; p->saved_top=30; p->visual_interval=2;
    env_name(p,key,"State");
    if (GetVar(key,text,sizeof(text),GVF_GLOBAL_ONLY)<=0) return;
    text[sizeof(text)-1]=0; at=text;
    for (i=0;i<27;++i) { v[i]=strtol(at,&end,10); if (end==at || v[i]<-32768 || v[i]>32767) return; at=end; }
    if ((v[0]!=1 && v[0]!=2) || v[7]<0 || v[7]>1 || v[8]<0 || v[8]>2 || v[9]<-100 || v[9]>100 ||
        v[12]<0 || v[12]>2 || v[13]<1 || v[13]>4 || v[16]<-12 || v[16]>12) return;
    for (i=17;i<27;++i) if (v[i]<-12 || v[i]>12) return;
    p->saved_left=(int)v[1]; p->saved_top=(int)v[2];
    p->pl_left=(int)v[3]; p->pl_top=(int)v[4]; p->eq_left=(int)v[5]; p->eq_top=(int)v[6];
    p->pl_position=p->eq_position=1; p->sequence.order.shuffle=(int)v[7]; p->sequence.order.repeat=(int)v[8];
    gSkinAudio.balance=(int)v[9]; p->remaining=v[10]!=0; p->shaded=v[11]!=0;
    p->visual_mode=(int)v[12]; p->visual_interval=(int)v[13]; p->debug=v[14]!=0;
    gSkinAudio.eq_enabled=v[15]!=0; gSkinAudio.eq_preamp=(int)v[16];
    for (i=0;i<10;++i) gSkinAudio.eq_bands[i]=(int)v[17+i];
    if (v[0]==2) {
        long visible=strtol(at,&end,10);
        if (end!=at) { p->restore_playlist=visible==1; at=end; visible=strtol(at,&end,10); if (end!=at) { p->restore_eq=visible==1; at=end;
            visible=strtol(at,&end,10); if (end!=at) { p->pl_position=visible==1; at=end;
                visible=strtol(at,&end,10); if (end!=at) p->eq_position=visible==1; }
        } }
    }
    ++gSkinAudio.eq_sequence;
}
static void eq_repaint(SkinPlayer *p,int all)
{
    SkinEqState s; int i;
    if (!p->eqwin) return;
    memset(&s,0,sizeof(s)); s.enabled=gSkinAudio.eq_enabled && gSkinAudio.eq_supported;
    s.preamp=gSkinAudio.eq_preamp; s.pressed=p->eq_pressed;
    for (i=0;i<10;++i) s.bands[i]=gSkinAudio.eq_bands[i];
    p->target=p->eqwin;
    skin_eq_render(&p->resources->skin,&s,(all || !p->eq_have_drawn) ? NULL : &p->eq_drawn,blit,fill,p);
    p->eq_drawn=s; p->eq_have_drawn=1;
}
static void eq_close(SkinPlayer *p)
{
    struct Message *m;
    if (!p->eqwin) return;
    p->eq_left=p->eqwin->LeftEdge; p->eq_top=p->eqwin->TopEdge; p->eq_position=1;
    while ((m=GetMsg(p->eqwin->UserPort))!=NULL) ReplyMsg(m);
    CloseWindow(p->eqwin); p->eqwin=NULL; p->eq_pressed=0; p->eq_have_drawn=0;
    repaint(p,0);
}
static void eq_toggle(SkinPlayer *p)
{
    int scale=p->resources->scale,left,top;
    if (p->eqwin) { eq_close(p); return; }
    if (!p->resources->skin.assets[SKIN_EQMAIN].rgb) { skin_player_notice(p,"THIS SKIN HAS NO EQMAIN ARTWORK"); return; }
    left=p->eq_position ? p->eq_left : p->win->LeftEdge;
    top=p->eq_position ? p->eq_top : p->win->TopEdge+p->win->Height;
    if (left<0) left=0;
    if (top<0) top=0;
    if (left+275*scale>p->screen->Width) left=p->screen->Width-275*scale;
    if (top+116*scale>p->screen->Height) top=p->screen->Height-116*scale;
    p->eqwin=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)p->screen,WA_Left,left,WA_Top,top,
        WA_Width,275*scale,WA_Height,116*scale,WA_Borderless,TRUE,WA_Activate,TRUE,
        WA_RMBTrap,TRUE,WA_ReportMouse,TRUE,WA_SimpleRefresh,TRUE,
        WA_IDCMP,IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_REFRESHWINDOW|IDCMP_VANILLAKEY|IDCMP_INACTIVEWINDOW,TAG_DONE);
    if (!p->eqwin) { skin_player_notice(p,"COULD NOT OPEN EQUALISER"); return; }
    eq_repaint(p,1); repaint(p,0);
}
int skin_player_choice(SkinPlayer *p,const char *text,const char *choices)
{
    struct EasyStruct es;
    memset(&es,0,sizeof(es)); es.es_StructSize=sizeof(es); es.es_Title=(STRPTR)p->name;
    es.es_TextFormat=(STRPTR)"%s"; es.es_GadgetFormat=(STRPTR)choices;
    return (int)EasyRequestArgs(p->win,&es,NULL,(APTR)&text);
}
static void eq_presets(SkinPlayer *p)
{
    int choice=skin_player_choice(p,"Equaliser presets (MP3 only)","Flat|Bass cut|Load|Save|Cancel"),i;
    struct FileRequester *fr; char path[512],text[256],*at,*end; BPTR fh; LONG len;
    long values[11]; int n;
    if (choice==1 || choice==2) { skin_eq_flat(); if (choice==2) { gSkinAudio.eq_bands[0]=-6; gSkinAudio.eq_bands[1]=-3; ++gSkinAudio.eq_sequence; } save_state(p); eq_repaint(p,0); return; }
    if (choice!=3 && choice!=4) return;
    fr=(struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,ASLFR_TitleText,(ULONG)"MintAMP EQ preset",
        ASLFR_DoSaveMode,(ULONG)(choice==4),ASLFR_InitialFile,(ULONG)"mintamp.eq",TAG_DONE);
    if (!fr) return;
    path[0]=0;
    if (AslRequestTags(fr,ASLFR_Window,(ULONG)p->eqwin,ASLFR_SleepWindow,TRUE,TAG_DONE)) {
        strncpy(path,(const char *)fr->fr_Drawer,sizeof(path)-1); path[sizeof(path)-1]=0;
        if (!AddPart(path,fr->fr_File,sizeof(path))) path[0]=0;
    }
    FreeAslRequest(fr); if (!path[0]) return;
    if (choice==4) {
        n=snprintf(text,sizeof(text),"MintAMP-EQ-1 %d",gSkinAudio.eq_preamp);
        for (i=0;i<10;++i) n+=snprintf(text+n,sizeof(text)-n," %d",gSkinAudio.eq_bands[i]);
        text[n++]='\n'; fh=Open(path,MODE_NEWFILE);
        if (!fh) { error_request(p,"Could not create preset."); return; }
        len=Write(fh,text,n); Close(fh); if (len!=n) error_request(p,"Preset write failed.");
    } else {
        fh=Open(path,MODE_OLDFILE); if (!fh) { error_request(p,"Could not open preset."); return; }
        len=Read(fh,text,sizeof(text)-1); Close(fh);
        if (len<13 || len>=(LONG)sizeof(text)-1) { error_request(p,"Invalid MintAMP preset."); return; }
        text[len]=0; if (strncmp(text,"MintAMP-EQ-1 ",12)) { error_request(p,"Invalid MintAMP preset."); return; }
        at=text+12;
        for (i=0;i<11;++i) { values[i]=strtol(at,&end,10); if (at==end || values[i]<-12 || values[i]>12) break; at=end; }
        if (i!=11) { error_request(p,"Invalid MintAMP preset."); return; }
        gSkinAudio.eq_preamp=(int)values[0];
        for (i=0;i<10;++i) gSkinAudio.eq_bands[i]=(int)values[i+1];
        ++gSkinAudio.eq_sequence; eq_repaint(p,0); save_state(p);
    }
}
static int eq_poll(SkinPlayer *p)
{
    struct IntuiMessage *m;
    while (p->eqwin && (m=(struct IntuiMessage *)GetMsg(p->eqwin->UserPort))!=NULL) {
        ULONG type=m->Class; UWORD code=m->Code;
        int x=m->MouseX/p->resources->scale,y=m->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY,i=-1; UWORD qualifier=m->Qualifier;
        ReplyMsg((struct Message *)m);
        if (type==IDCMP_REFRESHWINDOW) { BeginRefresh(p->eqwin); eq_repaint(p,1); EndRefresh(p->eqwin,TRUE); }
        else if (type==IDCMP_INACTIVEWINDOW) { p->eq_pressed=0; eq_repaint(p,0); }
        else if (type==IDCMP_VANILLAKEY && (code==27 || code=='e' || code=='E')) { eq_close(p); save_state(p); }
        else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            if (x>=264 && y<14) { eq_close(p); save_state(p); continue; }
            if (y<14) { p->eq_pressed=-1; drag_begin(p,2,qualifier); p->eq_drag_x=sx; p->eq_drag_y=sy; continue; }
            if (x>=14 && x<40 && y>=18 && y<30) {
                if (!gSkinAudio.eq_supported) skin_player_notice(p,"EQ REQUIRES MP3 PLAYBACK - OTHER DECODERS BYPASS");
                else { gSkinAudio.eq_enabled=!gSkinAudio.eq_enabled; eq_repaint(p,0); save_state(p); }
            } else if (x>=40 && x<72 && y>=18 && y<30) skin_player_notice(p,"AUTOMATIC PER-TRACK EQ PRESETS NOT SUPPORTED");
            else if (x>=224 && x<268 && y>=18 && y<30) eq_presets(p);
            else if (y>=38 && y<101) {
                if (x>=20 && x<35) i=0;
                else if (x>=77 && x<253 && (x-77)%18<14) i=1+(x-77)/18;
                if (i>=0 && i<=10) {
                    static const int lower[10]={0,101,230,431,775,1732,4243,8485,12961,14967};
                    int rate=p->now.rate;
                    if (gSkinAudio.output_rate>0 && (!rate || gSkinAudio.output_rate<rate)) rate=gSkinAudio.output_rate;
                    if (i && rate>0 && lower[i-1]>=rate/2)
                        skin_player_notice(p,"EQ BAND OUTSIDE OUTPUT BANDWIDTH");
                    else p->eq_pressed=i+1;
                }
            }
        } else if (type==IDCMP_MOUSEMOVE && p->eq_pressed==-1) {
            drag_move(p,sx-p->eq_drag_x,sy-p->eq_drag_y); p->eq_drag_x=sx; p->eq_drag_y=sy;
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) { if (p->eq_pressed==-1) drag_end(p,2); p->eq_pressed=0; eq_repaint(p,0); save_state(p); }
        if ((type==IDCMP_MOUSEMOVE || (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN)) && p->eq_pressed>0) {
            int db=12-(y-43)*24/50;
            if (db<-12) db=-12;
            if (db>12) db=12;
            if (p->eq_pressed==1) gSkinAudio.eq_preamp=db; else gSkinAudio.eq_bands[p->eq_pressed-2]=db;
            ++gSkinAudio.eq_sequence; eq_repaint(p,0);
        }
    }
    return 0;
}
static void visual_arm(SkinPlayer *p)
{
    if (!p->visual_device || p->visual_pending || p->shaded) return;
    if (!(strlen(p->now.title)>31 || p->notice_ticks || (p->visual_mode && p->now.playing && !p->now.paused))) return;
    p->visual_timer->tr_node.io_Command=TR_ADDREQUEST;
    p->visual_timer->tr_time.tv_secs=0;
    p->visual_timer->tr_time.tv_micro=250000;
    if (p->visual_timer->tr_time.tv_micro>=1000000) {
        p->visual_timer->tr_time.tv_secs=1; p->visual_timer->tr_time.tv_micro=0;
    }
    SendIO((struct IORequest *)p->visual_timer); p->visual_pending=1;
}
static void visual_tick(SkinPlayer *p)
{
    int n;
    if (p->shaded) return;
    if (p->notice_ticks) --p->notice_ticks;
    else if (strlen(p->now.title)>31) p->now.scroll=(p->now.scroll+2)%((strlen(p->now.title)+5)*5);
    repaint(p,0);
    if (!p->visual_mode || ++p->visual_ticks<p->visual_interval) return;
    p->visual_ticks=0;
    if (!p->now.playing || p->now.paused) { memset(p->now.levels,0,16); memset(p->now.scope,0,64); repaint(p,0); return; }
    if (gSkinAudio.visual_request==gSkinAudio.visual_sequence) {
        for (n=0;n<64;++n) p->now.scope[n]=gSkinAudio.visual_pcm[n];
        if (p->visual_mode==1) skin_visual_analyse(p->now.scope,p->now.levels);
        if (p->now.playing && !p->now.paused) ++gSkinAudio.visual_request;
    }
    if (!p->now.playing || p->now.paused) { memset(p->now.levels,0,16); memset(p->now.scope,0,64); }
    p->now.visual_mode=p->visual_mode; repaint(p,0);
}
static int internal_action(SkinPlayer *p,int action)
{
    if (action==SKIN_EQ) eq_toggle(p);
    else if (action==SKIN_TIMER) p->remaining=!p->remaining;
    else if (action==SKIN_SHUFFLE) { p->sequence.order.shuffle=!p->sequence.order.shuffle; skin_order_queue_reset(&p->sequence,p->playlist.count,p->playlist.current); skin_player_notice(p,p->sequence.order.shuffle ? "SHUFFLE ON" : "SHUFFLE OFF"); }
    else if (action==SKIN_REPEAT) { p->sequence.order.repeat=(p->sequence.order.repeat+1)%3; skin_player_notice(p,p->sequence.order.repeat==2 ? "REPEAT TRACK" : p->sequence.order.repeat==1 ? "REPEAT PLAYLIST" : "REPEAT OFF"); }
    else if (action==SKIN_VISUAL) {
        if (!p->visual_device) skin_player_notice(p,"VISUALISATION TIMER UNAVAILABLE");
        else p->visual_mode=(p->visual_mode+1)%3;
    }
    else if (action==SKIN_VIS_RATE) { int n=skin_player_choice(p,"Visualisation rate (GUI timer)","1 Hz|2 Hz|4 Hz|Cancel"); if (n>=1 && n<=3) p->visual_interval=n==1 ? 4 : n==2 ? 2 : 1; }
    else if (action==SKIN_SHADE) {
        p->shaded=!p->shaded;
        ChangeWindowBox(p->win,p->win->LeftEdge,p->win->TopEdge,275*p->resources->scale,(p->shaded ? 14 : 116)*p->resources->scale);
        p->have_drawn=0;
    } else return 0;
    p->now.remaining=p->remaining; p->now.shaded=p->shaded;
    p->now.shuffle=p->sequence.order.shuffle; p->now.repeat=p->sequence.order.repeat;
    p->now.visual_mode=p->shaded ? 0 : p->visual_mode;
    visual_arm(p); repaint(p,0); save_state(p); return 1;
}
void skin_player_save(SkinPlayer *p) { if (p) save_state(p); }
void skin_player_wake(const char *name,int pause)
{
    struct Task *child;
    gSkinAudio.pause_requested=pause;
    if (pause) return;
    Forbid(); child=FindTask((STRPTR)name); if (child) Signal(child,SIGBREAKF_CTRL_D); Permit();
}
int skin_player_order(SkinPlayer *p,int count,int current,int direction,int natural)
{ return skin_order_queue_request(&p->sequence,count,current,direction,natural); }
int skin_player_paths(SkinPlayer *p,const char *paths,unsigned stride,int count,int current,int focus)
{
    unsigned long hash=2166136261UL; int i; unsigned n;
    if (!p) return 0;
    for (i=0;i<count;++i) {
        for (n=0;n<stride && paths[i*stride+n];++n)
            hash=((hash^(unsigned char)paths[i*stride+n])*16777619UL)&0xffffffffUL;
        hash=((hash^0xffUL)*16777619UL)&0xffffffffUL;
    }
    hash^=(unsigned long)count;
    if (hash==p->playlist_identity) { p->preserve_selection=0; return 0; }
    p->playlist_identity=hash; skin_order_queue_reset(&p->sequence,count,current);
    if (!p->preserve_selection) { memset(&p->selection,0,sizeof(p->selection)); p->selection.focus=-1; }
    p->preserve_selection=0;
    skin_selection_sync(&p->selection,count,focus); p->click_row=-1; p->pending_double=0;
    return 1;
}
void skin_player_order_cancel(SkinPlayer *p) { skin_order_queue_cancel(&p->sequence); }
void skin_player_order_reset(SkinPlayer *p,int count,int current)
{ skin_order_queue_reset(&p->sequence,count,current); p->preserve_selection=1; }
SkinSelection *skin_player_selection(SkinPlayer *p,int count,int focus)
{ skin_selection_sync(&p->selection,count,focus); return &p->selection; }
void skin_player_debug(SkinPlayer *p,long buffer_ms,long spare_ms,unsigned long underruns,unsigned long bytes,const char *station)
{
    char text[512];
    snprintf(text,sizeof(text),"Station: %.24s\nBuffer: %ld ms\nSpare: %ld ms\nUnderruns: %lu\nRadio buffered: %lu bytes\nSource: %d ch, %d kbit/s\nPaula output: %s\nPause: %s",station,buffer_ms,spare_ms,underruns,bytes,gSkinAudio.channels,gSkinAudio.bitrate,gSkinAudio.output_stereo ? "stereo" : "mono",gSkinAudio.paused ? "paused" : gSkinAudio.pause_requested ? "pending" : "running");
    if (skin_player_choice(p,text,"OK|Toggle overlay")==0) { p->debug=!p->debug; save_state(p); }
}
static void change_skin(SkinPlayer *p,int choose)
{
    char path[512], error[160]; SkinResources *next, *old; int scale;
    WORD left=p->win->LeftEdge,top=p->win->TopEdge;
    int had_playlist=p->plwin!=NULL,had_eq=p->eqwin!=NULL;
    strcpy(path,p->path); scale=p->resources->scale;
    if (choose) { if (!request_path(p,path)) return; }
    else scale=scale==1 ? 2 : 1;
    if (275*scale>p->screen->Width || 116*scale>p->screen->Height) {
        error_request(p,"Double size does not fit this screen."); return;
    }
    next=load_resources(p,path,scale,error);
    if (!next) { error_request(p,error); return; }
    save_state(p); old=p->resources; eq_close(p); playlist_close(p); close_window(p); p->resources=next;
    if (!open_window(p,left,top)) {
        p->resources=old; free_resources(p,next);
        if (!open_window(p,left,top)) p->win=NULL;
        if (had_playlist && p->win && !playlist_open(p)) p->pending_options=1;
        error_request(p,"Could not open the new skin window.");
        return;
    }
    free_resources(p,old); strcpy(p->path,path); save_choice(p);
    if (had_playlist && !playlist_open(p)) p->pending_options=1;
    if (had_eq) eq_toggle(p);
}
SkinPlayer *skin_player_open(const char *name,int argc,char **argv)
{
    SkinPlayer *p=(SkinPlayer *)calloc(1,sizeof(*p));
    char key[64], value[512], error[160]; int i,scale=1;
    struct DiskObject *icon=NULL;
    if (!p) return NULL;
    strncpy(p->name,name,sizeof(p->name)-1);
    { ULONG seconds,micros; CurrentTime(&seconds,&micros); skin_order_queue_init(&p->sequence,seconds^micros); }
    restore_state(p);
    p->selection.focus=p->selection.anchor=-1; p->move_row=-1;
    p->screen=LockPubScreen(NULL);
    if (!p->screen) { free(p); return NULL; }
    strcpy(p->path,"PROGDIR:Skins/base-2.91.wsz");
    env_name(p,key,"Skin");
    if (GetVar(key,value,sizeof(value),GVF_GLOBAL_ONLY)>0) {
        value[sizeof(value)-1]=0; strcpy(p->path,value);
    }
    env_name(p,key,"Scale");
    if (GetVar(key,value,sizeof(value),GVF_GLOBAL_ONLY)>0 && value[0]=='2') scale=2;
    if (argc==0 && argv) {
        struct WBStartup *wb=(struct WBStartup *)argv;
        BPTR previous=CurrentDir(wb->sm_ArgList[0].wa_Lock);
        icon=GetDiskObject(wb->sm_ArgList[0].wa_Name); CurrentDir(previous);
        if (icon) {
            STRPTR v=FindToolType(icon->do_ToolTypes,"SKIN");
            if (v) { strncpy(p->path,(const char *)v,sizeof(p->path)-1); p->path[sizeof(p->path)-1]=0; }
            v=FindToolType(icon->do_ToolTypes,"DOUBLESIZE");
            if (v) scale=(!*v || !strcmp((const char *)v,"1") || !strcmp((const char *)v,"YES") || !strcmp((const char *)v,"TRUE")) ? 2 : 1;
            FreeDiskObject(icon);
        }
        if (wb->sm_NumArgs>1 && skin_player_is_skin((const char *)wb->sm_ArgList[1].wa_Name)) {
            if (NameFromLock(wb->sm_ArgList[1].wa_Lock,p->path,sizeof(p->path)))
                AddPart(p->path,wb->sm_ArgList[1].wa_Name,sizeof(p->path));
        }
    }
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--double")) scale=2;
        else if (!strcmp(argv[i],"--skin") && i+1<argc) {
            strncpy(p->path,argv[++i],sizeof(p->path)-1); p->path[sizeof(p->path)-1]=0;
        } else if (skin_player_is_skin(argv[i])) {
            strncpy(p->path,argv[i],sizeof(p->path)-1); p->path[sizeof(p->path)-1]=0;
        }
    }
    if (275*scale>p->screen->Width || 116*scale>p->screen->Height) scale=1;
    p->resources=load_resources(p,p->path,scale,error);
    if (!p->resources) {
        /* Missing default is normal on a fresh install. Choose a skin instead
         * of shipping someone else's artwork as part of the application. */
        if (!request_path(p,p->path)) { skin_player_close(p); return NULL; }
        p->resources=load_resources(p,p->path,scale,error);
        if (!p->resources) { error_request(p,error); skin_player_close(p); return NULL; }
    }
    strcpy(p->now.title,"MINTAMP - RIGHT CLICK FOR MENU");
    if (!open_window(p,p->saved_left,p->saved_top)) { skin_player_close(p); return NULL; }
    if (p->shaded) { p->shaded=0; internal_action(p,SKIN_SHADE); }
    p->visual_port=CreateMsgPort();
    if (p->visual_port) p->visual_timer=(struct timerequest *)CreateIORequest(p->visual_port,sizeof(struct timerequest));
    if (p->visual_timer && !OpenDevice((STRPTR)TIMERNAME,UNIT_VBLANK,(struct IORequest *)p->visual_timer,0)) p->visual_device=1;
    if (!p->visual_device) p->visual_mode=0;
    if (p->restore_playlist && !playlist_open(p)) p->pending_options=1;
    if (p->restore_eq) eq_toggle(p);
    visual_arm(p); save_choice(p); return p;
}
void skin_player_close(SkinPlayer *p)
{
    if (!p) return;
    save_state(p);
    if (p->visual_pending) { AbortIO((struct IORequest *)p->visual_timer); WaitIO((struct IORequest *)p->visual_timer); }
    if (p->visual_device) CloseDevice((struct IORequest *)p->visual_timer);
    if (p->visual_timer) DeleteIORequest((struct IORequest *)p->visual_timer);
    if (p->visual_port) DeleteMsgPort(p->visual_port);
    eq_close(p); playlist_close(p); close_window(p); free_resources(p,p->resources);
    free_text_bitmap(p->text_source,243,8); free_text_bitmap(p->text_scaled,486,16);
    if (p->playlist_font) CloseFont(p->playlist_font);
    if (p->screen) UnlockPubScreen(NULL,p->screen);
    free(p);
}
ULONG skin_player_signal(SkinPlayer *p)
{ return p ? (p->win ? 1UL<<p->win->UserPort->mp_SigBit : 0) |
            (p->plwin ? 1UL<<p->plwin->UserPort->mp_SigBit : 0) |
            (p->eqwin ? 1UL<<p->eqwin->UserPort->mp_SigBit : 0) |
            (p->visual_port ? 1UL<<p->visual_port->mp_SigBit : 0) : 0; }
struct Window *skin_player_window(SkinPlayer *p) { return p ? p->win : NULL; }
void skin_player_notice(SkinPlayer *p,const char *message)
{
    if (!p) return;
    strncpy(p->notice,message,sizeof(p->notice)-1); p->notice[sizeof(p->notice)-1]=0;
    p->notice_ticks=12; visual_arm(p);
}
void skin_player_update(SkinPlayer *p,const SkinState *s,int tick)
{
    int scroll;
    if (!p || !p->win) return;
    scroll=strcmp(p->now.title,s->title) ? 0 : p->now.scroll;
    if (tick && !p->visual_device && !p->notice_ticks) scroll=(scroll+2)%((strlen(s->title)+5)*5);
    { unsigned char levels[16]; signed char scope[64];
        memcpy(levels,p->now.levels,16); memcpy(scope,p->now.scope,64);
        p->now=*s; memcpy(p->now.levels,levels,16); memcpy(p->now.scope,scope,64);
        p->now.remaining=p->remaining; p->now.shaded=p->shaded;
        p->now.shuffle=p->sequence.order.shuffle; p->now.repeat=p->sequence.order.repeat; p->now.balance=gSkinAudio.balance;
        p->now.visual_mode=p->shaded ? 0 : p->visual_mode;
        if (!s->playing || s->paused) { memset(p->now.levels,0,16); memset(p->now.scope,0,64); }
        eq_repaint(p,0); visual_arm(p);
    }
    p->now.scroll=scroll;
    if (p->debug && s->debug_text[0]) { strncpy(p->now.title,s->debug_text,255); p->now.title[255]=0; p->now.scroll=0; }
    if (p->notice_ticks) {
        strcpy(p->now.title,p->notice); p->now.scroll=0;
        if (tick && !p->visual_device) --p->notice_ticks;
    }
    repaint(p,0);
}
static int menu_action(SkinPlayer *p,UWORD code,SkinEvent *event)
{
    static const int actions[12]={SKIN_SELECT,SKIN_SIZE,SKIN_SETTINGS,SKIN_RADIO,
        SKIN_PLAYLIST,SKIN_PLAYLIST_OPTIONS,SKIN_BROWSE,SKIN_QUIT,SKIN_EQ,SKIN_VISUAL,SKIN_VIS_RATE,SKIN_DEBUG};
    int item=ITEMNUM(code);
    if (code==MENUNULL || MENUNUM(code)!=0 || item>=12) return 0;
    if (item<2) { change_skin(p,item==0); return 0; }
    if (internal_action(p,actions[item])) return 0;
    event->action=actions[item]; event->released=1; return 1;
}
static void playlist_scroll(SkinPlayer *p,int top)
{
    p->playlist.top=skin_playlist_top(top,p->playlist.count); p->click_row=-1;
    skin_player_playlist_update(p,p->playlist.count,p->playlist.selected,
        p->playlist.current,p->playlist_name,p->playlist_ctx);
}
static int playlist_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *msg;
    while (p->plwin && (msg=(struct IntuiMessage *)GetMsg(p->plwin->UserPort))!=NULL) {
        ULONG type=msg->Class,seconds=msg->Seconds,micros=msg->Micros;
        UWORD code=msg->Code;
        int x=msg->MouseX/p->resources->scale,y=msg->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        UWORD qualifier=msg->Qualifier;
        ReplyMsg((struct Message *)msg); memset(event,0,sizeof(*event));
        if (type==IDCMP_REFRESHWINDOW) {
            BeginRefresh(p->plwin); playlist_repaint(p,1); EndRefresh(p->plwin,TRUE);
        } else if (type==IDCMP_INACTIVEWINDOW) p->playlist_pressed=0;
        else if (type==IDCMP_MENUPICK) {
            if (menu_action(p,code,event)) return 1;
        } else if (type==IDCMP_VANILLAKEY && (code=='\r' || code==' ')) {
            if (p->playlist.selected>=0 && p->playlist.selected<p->playlist.count) {
                event->action=SKIN_TRACK_PLAY; event->value=p->playlist.selected; return 1;
            }
        } else if (type==IDCMP_VANILLAKEY && (code=='p' || code=='P' || code==27)) {
            playlist_close(p);
        } else if (type==IDCMP_VANILLAKEY && (code=='o' || code=='O')) {
            event->action=SKIN_PLAYLIST_OPTIONS; return 1;
        } else if (type==IDCMP_RAWKEY && code==0x46) {
            event->action=SKIN_TRACK_REMOVE; return 1;
        } else if (type==IDCMP_VANILLAKEY && code==1) {
            skin_selection_all(&p->selection,1); playlist_scroll(p,p->playlist.top);
        } else if (type==IDCMP_RAWKEY && (code==0x4c || code==0x4d || code==0x3d || code==0x1d || code==0x3f || code==0x1f)) {
            int index=code==0x3d ? 0 : code==0x1d ? p->playlist.count-1 :
                p->playlist.selected+(code==0x4c ? -1 : code==0x4d ? 1 : code==0x3f ? -17 : 17);
            if ((qualifier&IEQUALIFIER_CONTROL) && (code==0x4c || code==0x4d)) {
                event->action=SKIN_TRACK_MOVE; event->value=p->playlist.selected; event->target=index; return 1;
            }
            if (index<0) index=0;
            if (index<p->playlist.count) {
                event->action=SKIN_TRACK_SELECT; event->value=index; p->click_row=-1;
                event->selection=(qualifier&(IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT)) ? SKIN_SELECT_RANGE : SKIN_SELECT_REPLACE; return 1;
            }
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            p->playlist_pressed=skin_playlist_hit_test(x,y);
            if (p->playlist_pressed==SKIN_DRAG) drag_begin(p,1,qualifier);
            p->playlist_drag_x=sx; p->playlist_drag_y=sy;
            if (p->playlist_pressed==SKIN_TRACK_SELECT) {
                int index=p->playlist.top+(y-22)/10;
                if (index<p->playlist.count) {
                    int twice=index==p->click_row && DoubleClick(p->click_seconds,p->click_micros,seconds,micros);
                    /* Selection is mirrored immediately so the backend's next
                     * refresh does not invalidate the second click. */
                    p->playlist.selected=index; p->move_row=p->move_target=index; p->move_active=0;
                    p->click_row=twice ? -1 : index;
                    p->click_seconds=seconds; p->click_micros=micros;
                    p->pending_double=twice;
                    event->action=SKIN_TRACK_SELECT;
                    event->value=index; event->selection=(qualifier&IEQUALIFIER_CONTROL) ? SKIN_SELECT_TOGGLE : (qualifier&(IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT)) ? SKIN_SELECT_RANGE : SKIN_SELECT_REPLACE; return 1;
                }
            } else if (p->playlist_pressed==SKIN_PLAYLIST_SCROLL) {
                int maximum=skin_playlist_top(p->playlist.count,p->playlist.count);
                playlist_scroll(p,(y-29)*maximum/156);
            }
        } else if (type==IDCMP_MOUSEMOVE && p->playlist_pressed==SKIN_TRACK_SELECT && p->move_row>=0) {
            if (abs(sy-p->playlist_drag_y)>3*p->resources->scale) p->move_active=1;
            if (p->move_active) { p->move_target=p->playlist.top+(y-22)/10; p->click_row=-1; }
        } else if (type==IDCMP_MOUSEMOVE && p->playlist_pressed==SKIN_DRAG) {
            drag_move(p,sx-p->playlist_drag_x,sy-p->playlist_drag_y);
            p->playlist_drag_x=sx; p->playlist_drag_y=sy;
        } else if (type==IDCMP_MOUSEMOVE && p->playlist_pressed==SKIN_PLAYLIST_SCROLL) {
            int maximum=skin_playlist_top(p->playlist.count,p->playlist.count);
            playlist_scroll(p,(y-29)*maximum/156);
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int action=p->playlist_pressed; p->playlist_pressed=0;
            if (action==SKIN_DRAG) drag_end(p,1);
            save_state(p);
            if (action==SKIN_TRACK_SELECT && p->move_active) {
                event->action=SKIN_TRACK_MOVE; event->value=p->move_row; event->target=p->move_target;
                p->move_row=-1; p->move_active=0; p->pending_double=0; return 1;
            }
            if (action==SKIN_TRACK_SELECT && p->pending_double && !p->move_active) {
                p->pending_double=0;
                if (p->move_row==p->playlist.top+(y-22)/10 && skin_playlist_hit_test(x,y)==SKIN_TRACK_SELECT) {
                    event->action=SKIN_TRACK_PLAY; event->value=p->move_row; return 1;
                }
            }
            if (action==skin_playlist_hit_test(x,y)) {
                if (action==SKIN_PLAYLIST_CLOSE) { playlist_close(p); save_state(p); }
                else if (action==SKIN_PLAYLIST_OPTIONS || (action>=SKIN_LIST_ADD && action<=SKIN_LIST_FILE)) { event->action=action; return 1; }
            }
        }
    }
    return 0;
}
int skin_player_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *msg;
    if (!p) return 0;
    if (!p->win) { event->action=SKIN_QUIT; return 1; }
    if (p->pending_options) {
        p->pending_options=0; memset(event,0,sizeof(*event));
        event->action=SKIN_PLAYLIST_OPTIONS; return 1;
    }
    if (p->visual_pending && CheckIO((struct IORequest *)p->visual_timer)) {
        WaitIO((struct IORequest *)p->visual_timer); p->visual_pending=0; visual_tick(p); visual_arm(p);
    }
    eq_poll(p);
    if (playlist_poll(p,event)) return 1;
    while ((msg=(struct IntuiMessage *)GetMsg(p->win->UserPort))!=NULL) {
        ULONG type=msg->Class; UWORD code=msg->Code;
        int x=msg->MouseX/p->resources->scale,y=msg->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        UWORD qualifier=msg->Qualifier;
        ReplyMsg((struct Message *)msg);
        memset(event,0,sizeof(*event));
        if (type==IDCMP_REFRESHWINDOW) {
            BeginRefresh(p->win); repaint(p,1); EndRefresh(p->win,TRUE);
        } else if (type==IDCMP_INACTIVEWINDOW) { p->pressed=0; repaint(p,0); }
        else if (type==IDCMP_MENUPICK && code!=MENUNULL) {
            if (menu_action(p,code,event)) return 1;
        } else if (type==IDCMP_VANILLAKEY) {
            if (code=='s' || code=='S') change_skin(p,1);
            else if (code=='d' || code=='D') change_skin(p,0);
            else {
                event->action=code==' ' || code=='c' || code=='C' ? SKIN_PAUSE : code=='x' || code=='X' ? SKIN_PLAY : code=='z' || code=='Z' ? SKIN_PREVIOUS : code=='b' || code=='B' ? SKIN_NEXT : code=='e' || code=='E' ? SKIN_EQ : code=='v' || code=='V' ? SKIN_STOP : code=='j' || code=='J' ? SKIN_SHUFFLE : code=='l' || code=='L' ? SKIN_REPEAT : code=='w' || code=='W' ? SKIN_SHADE : code=='r' || code=='R' ? SKIN_RADIO :
                    code=='o' || code=='O' ? SKIN_BROWSE : code=='p' || code=='P' ? SKIN_PLAYLIST :
                    code=='t' || code=='T' ? SKIN_SETTINGS : SKIN_NONE;
                if (event->action && !internal_action(p,event->action)) { event->released=1; return 1; }
            }
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            p->pressed=p->shaded && y>=14 ? SKIN_NONE : skin_hit_test(x,y); p->drag_x=sx; p->drag_y=sy;
            if (p->pressed==SKIN_DRAG) drag_begin(p,0,qualifier);
            if (p->pressed==SKIN_SEEK) p->seek_preview=skin_slider_value(SKIN_SEEK,x);
            repaint(p,0);
            if ((p->pressed==SKIN_VOLUME_SET || p->pressed==SKIN_BALANCE_SET)) {
                event->action=p->pressed; event->value=skin_slider_value(p->pressed,x); return 1;
            }
        } else if (type==IDCMP_MOUSEMOVE && p->pressed==SKIN_DRAG) {
            drag_move(p,sx-p->drag_x,sy-p->drag_y); p->drag_x=sx; p->drag_y=sy;
        } else if (type==IDCMP_MOUSEMOVE && p->pressed==SKIN_SEEK) {
            p->seek_preview=skin_slider_value(SKIN_SEEK,x); repaint(p,0);
        } else if (type==IDCMP_MOUSEMOVE && (p->pressed==SKIN_VOLUME_SET || p->pressed==SKIN_BALANCE_SET)) {
            event->action=p->pressed; event->value=skin_slider_value(p->pressed,x); return 1;
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int action=p->pressed; p->pressed=0;
            if (action==SKIN_DRAG) drag_end(p,0);
            repaint(p,0); save_state(p);
            if (action==SKIN_VOLUME_SET || action==SKIN_SEEK || action==SKIN_BALANCE_SET) {
                event->action=action; event->value=skin_slider_value(action,x); event->released=1; return 1;
            }
            if (action==skin_hit_test(x,y) && action!=SKIN_DRAG && action!=SKIN_NONE) {
                if (action==SKIN_SIZE) { change_skin(p,0); continue; }
                if (internal_action(p,action)) continue;
                event->action=action; event->released=1; return 1;
            }
        }
    }
    return 0;
}

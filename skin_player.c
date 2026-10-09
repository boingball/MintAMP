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
#include <intuition/intuition.h>
#include <libraries/asl.h>
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
    struct Window *win;
    SkinResources *resources;
    struct Menu menu;
    struct MenuItem items[7];
    struct IntuiText labels[7];
    SkinState now, drawn;
    int have_drawn, pressed, drag_x, drag_y;
    int notice_ticks;
    char notice[256], name[32], path[512];
};

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
    if (p->win && p->resources->bitmaps[id])
        BltBitMapRastPort(p->resources->bitmaps[id],sx*s,sy*s,p->win->RPort,x*s,y*s,w*s,h*s,0xc0);
}
static void fill(void *ctx,unsigned rgb,int x,int y,int w,int h)
{
    SkinPlayer *p=(SkinPlayer *)ctx; int s=p->resources->scale;
    SetAPen(p->win->RPort,(ULONG)color_pen(p,p->resources,rgb));
    RectFill(p->win->RPort,x*s,y*s,(x+w)*s-1,(y+h)*s-1);
}
static void repaint(SkinPlayer *p,int all)
{
    if (!p->win || !p->resources) return;
    p->now.pressed=p->pressed;
    skin_render(&p->resources->skin,&p->now,(all || !p->have_drawn) ? NULL : &p->drawn,blit,fill,p);
    p->drawn=p->now; p->have_drawn=1;
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
    static const char * const labels[]={"Load skin...","Double size","Settings","Internet Radio","Playlist","Open audio...","Quit"};
    int i, h=p->screen->Font->ta_YSize+4, width=0;
    memset(&p->menu,0,sizeof(p->menu));
    p->menu.Flags=MENUENABLED; p->menu.MenuName=(STRPTR)"MintAMP";
    p->menu.Width=TextLength(&p->screen->RastPort,p->menu.MenuName,strlen((const char *)p->menu.MenuName))+16;
    p->menu.Height=h; p->menu.FirstItem=p->items;
    for (i=0;i<7;++i) {
        int w=TextLength(&p->screen->RastPort,(STRPTR)labels[i],strlen(labels[i]))+16;
        if (w>width) width=w;
    }
    for (i=0;i<7;++i) {
        memset(&p->items[i],0,sizeof(p->items[i])); memset(&p->labels[i],0,sizeof(p->labels[i]));
        p->items[i].NextItem=i<6 ? &p->items[i+1] : NULL;
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
static void change_skin(SkinPlayer *p,int choose)
{
    char path[512], error[160]; SkinResources *next, *old; int scale;
    WORD left=p->win->LeftEdge,top=p->win->TopEdge;
    strcpy(path,p->path); scale=p->resources->scale;
    if (choose) { if (!request_path(p,path)) return; }
    else scale=scale==1 ? 2 : 1;
    if (275*scale>p->screen->Width || 116*scale>p->screen->Height) {
        error_request(p,"Double size does not fit this screen."); return;
    }
    next=load_resources(p,path,scale,error);
    if (!next) { error_request(p,error); return; }
    old=p->resources; close_window(p); p->resources=next;
    if (!open_window(p,left,top)) {
        p->resources=old; free_resources(p,next);
        if (!open_window(p,left,top)) p->win=NULL;
        error_request(p,"Could not open the new skin window.");
        return;
    }
    free_resources(p,old); strcpy(p->path,path); save_choice(p);
}
SkinPlayer *skin_player_open(const char *name,int argc,char **argv)
{
    SkinPlayer *p=(SkinPlayer *)calloc(1,sizeof(*p));
    char key[64], value[512], error[160]; int i,scale=1;
    struct DiskObject *icon=NULL;
    if (!p) return NULL;
    strncpy(p->name,name,sizeof(p->name)-1);
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
    if (!open_window(p,40,30)) { skin_player_close(p); return NULL; }
    save_choice(p); return p;
}
void skin_player_close(SkinPlayer *p)
{
    if (!p) return;
    close_window(p); free_resources(p,p->resources);
    if (p->screen) UnlockPubScreen(NULL,p->screen);
    free(p);
}
ULONG skin_player_signal(SkinPlayer *p)
{ return p && p->win ? 1UL<<p->win->UserPort->mp_SigBit : 0; }
struct Window *skin_player_window(SkinPlayer *p) { return p ? p->win : NULL; }
void skin_player_notice(SkinPlayer *p,const char *message)
{
    if (!p) return;
    strncpy(p->notice,message,sizeof(p->notice)-1); p->notice[sizeof(p->notice)-1]=0;
    p->notice_ticks=12;
}
void skin_player_update(SkinPlayer *p,const SkinState *s,int tick)
{
    int scroll;
    if (!p || !p->win) return;
    scroll=strcmp(p->now.title,s->title) ? 0 : p->now.scroll;
    if (tick && !p->notice_ticks) scroll=(scroll+2)%((strlen(s->title)+5)*5);
    p->now=*s; p->now.scroll=scroll;
    if (p->notice_ticks) {
        strcpy(p->now.title,p->notice); p->now.scroll=0;
        if (tick) --p->notice_ticks;
    }
    repaint(p,0);
}
int skin_player_poll(SkinPlayer *p,SkinEvent *event)
{
    struct IntuiMessage *msg;
    if (!p) return 0;
    if (!p->win) { event->action=SKIN_QUIT; return 1; }
    while ((msg=(struct IntuiMessage *)GetMsg(p->win->UserPort))!=NULL) {
        ULONG type=msg->Class; UWORD code=msg->Code;
        int x=msg->MouseX/p->resources->scale,y=msg->MouseY/p->resources->scale;
        int sx=p->screen->MouseX,sy=p->screen->MouseY;
        ReplyMsg((struct Message *)msg);
        memset(event,0,sizeof(*event));
        if (type==IDCMP_REFRESHWINDOW) {
            BeginRefresh(p->win); repaint(p,1); EndRefresh(p->win,TRUE);
        } else if (type==IDCMP_INACTIVEWINDOW) { p->pressed=0; repaint(p,0); }
        else if (type==IDCMP_MENUPICK && code!=MENUNULL) {
            static const int actions[7]={SKIN_SELECT,SKIN_SIZE,SKIN_SETTINGS,SKIN_RADIO,SKIN_PLAYLIST,SKIN_BROWSE,SKIN_QUIT};
            int item=ITEMNUM(code);
            if (MENUNUM(code)==0 && item<7) {
                if (item<2) { change_skin(p,item==0); continue; }
                event->action=actions[item]; event->released=1; return 1;
            }
        } else if (type==IDCMP_VANILLAKEY) {
            if (code=='s' || code=='S') change_skin(p,1);
            else if (code=='d' || code=='D') change_skin(p,0);
            else {
                event->action=code==' ' ? SKIN_PLAY : code=='r' || code=='R' ? SKIN_RADIO :
                    code=='o' || code=='O' ? SKIN_BROWSE : code=='p' || code=='P' ? SKIN_PLAYLIST :
                    code=='t' || code=='T' ? SKIN_SETTINGS : code=='x' || code=='X' ? SKIN_STOP : SKIN_NONE;
                if (event->action) { event->released=1; return 1; }
            }
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTDOWN) {
            p->pressed=skin_hit_test(x,y); p->drag_x=sx; p->drag_y=sy; repaint(p,0);
            if (p->pressed==SKIN_VOLUME_SET) {
                event->action=SKIN_VOLUME_SET; event->value=skin_slider_value(SKIN_VOLUME_SET,x); return 1;
            }
        } else if (type==IDCMP_MOUSEMOVE && p->pressed==SKIN_DRAG) {
            MoveWindow(p->win,sx-p->drag_x,sy-p->drag_y); p->drag_x=sx; p->drag_y=sy;
        } else if (type==IDCMP_MOUSEMOVE && p->pressed==SKIN_VOLUME_SET) {
            event->action=SKIN_VOLUME_SET; event->value=skin_slider_value(SKIN_VOLUME_SET,x); return 1;
        } else if (type==IDCMP_MOUSEBUTTONS && code==SELECTUP) {
            int action=p->pressed; p->pressed=0; repaint(p,0);
            if (action==SKIN_VOLUME_SET || action==SKIN_SEEK) {
                event->action=action; event->value=skin_slider_value(action,x); event->released=1; return 1;
            }
            if (action==skin_hit_test(x,y) && action!=SKIN_DRAG && action!=SKIN_NONE) {
                if (action==SKIN_SIZE) { change_skin(p,0); continue; }
                event->action=action; event->released=1; return 1;
            }
        }
    }
    return 0;
}

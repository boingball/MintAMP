#include "winamp_skin.h"
#include "lodepng.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Canvas {
    const WinampSkin *skin;
    unsigned char pixels[275*116*3];
    unsigned draws;
} Canvas;
static void blit(void *ctx,int id,int sx,int sy,int w,int h,int x,int y)
{
    Canvas *c=ctx; const SkinBitmap *b=&c->skin->assets[id]; int row;
    assert(x>=0 && y>=0 && x+w<=275 && y+h<=116);
    assert(sx>=0 && sy>=0 && sx+w<=(int)b->width && sy+h<=(int)b->height);
    for (row=0;row<h;++row)
        memcpy(c->pixels+((y+row)*275+x)*3,b->rgb+((sy+row)*b->width+sx)*3,w*3);
    ++c->draws;
}
static void fill(void *ctx,unsigned rgb,int x,int y,int w,int h)
{
    Canvas *c=ctx; int i,j;
    assert(x>=0 && y>=0 && x+w<=275 && y+h<=116);
    for (j=y;j<y+h;++j) for (i=x;i<x+w;++i) {
        unsigned char *p=c->pixels+(j*275+i)*3;
        p[0]=rgb>>16; p[1]=rgb>>8; p[2]=rgb;
    }
    ++c->draws;
}
static unsigned char *read_file(const char *name,size_t *size)
{
    FILE *f=fopen(name,"rb"); long len; unsigned char *data;
    assert(f); assert(!fseek(f,0,SEEK_END)); len=ftell(f); assert(len>=0); rewind(f);
    data=malloc(len ? (size_t)len : 1); assert(data);
    assert(fread(data,1,(size_t)len,f)==(size_t)len); fclose(f); *size=(size_t)len; return data;
}
int main(int argc,char **argv)
{
    WinampSkin skin={0}; char error[160]; int i;
    if (argc>2 && !strcmp(argv[1],"--bmp")) {
        size_t len; unsigned char *data=read_file(argv[2],&len); SkinBitmap b={0};
        int ok=skin_decode_bmp(&b,data,len,error,sizeof(error)); free(data);
        if (!ok) { fprintf(stderr,"%s\n",error); return 1; }
        printf("%u %u %08x\n",b.width,b.height,lodepng_crc32(b.rgb,(size_t)b.width*b.height*3));
        free(b.rgb); return 0;
    }
    assert(argc>=2);
    if (!skin_load_file(&skin,argv[1],error,sizeof(error))) {
        fprintf(stderr,"%s\n",error); return 1;
    }
    for (i=0;i<SKIN_ASSET_COUNT;++i) if (skin.assets[i].rgb)
        printf("%d %u %u %08x\n",i,skin.assets[i].width,skin.assets[i].height,
               lodepng_crc32(skin.assets[i].rgb,(size_t)skin.assets[i].width*skin.assets[i].height*3));
    /* A failed replacement must leave the loaded skin and its pixels alive. */
    {
        unsigned char invalid[22]={0}; unsigned char *original=skin.assets[0].rgb;
        assert(!skin_load_memory(&skin,invalid,sizeof(invalid),error,sizeof(error)));
        assert(skin.assets[0].rgb==original);
    }
    {
        Canvas *c=calloc(1,sizeof(*c)); SkinState state={0},old;
        unsigned char *full=malloc(sizeof(c->pixels)); unsigned draws;
        assert(c && full); c->skin=&skin;
        strcpy(state.title,"MINTAMP CLASSIC SKIN TEST - LIVE RADIO METADATA");
        state.elapsed=125; state.total=300; state.volume=75; state.playing=1;
        state.rate=44100; state.bitrate=128;
        skin_render(&skin,&state,NULL,blit,fill,c); assert(c->draws);
        draws=c->draws; old=state;
        skin_render(&skin,&state,&old,blit,fill,c); assert(c->draws==draws);
        /* Incremental and full renders must produce identical pixels. */
        state.elapsed=5999; state.total=6000; state.volume=0; state.scroll=7;
        state.pressed=SKIN_VOLUME_SET; state.playing=0; state.mono=1;
        skin_render(&skin,&state,&old,blit,fill,c);
        memcpy(full,c->pixels,sizeof(c->pixels));
        skin_render(&skin,&state,NULL,blit,fill,c);
        assert(!memcmp(full,c->pixels,sizeof(c->pixels)));
        assert(skin_hit_test(39,88)==SKIN_PLAY);
        assert(skin_hit_test(85,88)==SKIN_STOP);
        assert(skin_hit_test(136,89)==SKIN_BROWSE);
        assert(skin_hit_test(264,3)==SKIN_QUIT);
        assert(skin_hit_test(242,58)==SKIN_PLAYLIST);
        assert(skin_hit_test(-1,0)==SKIN_NONE && skin_hit_test(275,116)==SKIN_NONE);
        assert(skin_slider_value(SKIN_VOLUME_SET,-1000)==0);
        assert(skin_slider_value(SKIN_VOLUME_SET,1000)==100);
        assert(skin_slider_value(SKIN_SEEK,30)==0 && skin_slider_value(SKIN_SEEK,249)==100);
        if (argc>2) {
            FILE *f=fopen(argv[2],"wb"); assert(f);
            state=old; skin_render(&skin,&state,NULL,blit,fill,c);
            fprintf(f,"P6\n275 116\n255\n"); fwrite(c->pixels,1,sizeof(c->pixels),f); fclose(f);
        }
        free(full); free(c);
    }
    skin_free(&skin); return 0;
}

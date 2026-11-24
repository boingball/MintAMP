/* Host CPU measurements are diagnostic, not Amiga audio acceptance. */
#include "winamp_skin.h"
#include "skin_controls.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
static unsigned draws;
static void blit(void *ctx,int id,int sx,int sy,int w,int h,int x,int y)
{ (void)ctx;(void)id;(void)sx;(void)sy;(void)w;(void)h;(void)x;(void)y; ++draws; }
static void fill(void *ctx,unsigned color,int x,int y,int w,int h)
{ (void)ctx;(void)color;(void)x;(void)y;(void)w;(void)h; ++draws; }
int main(void)
{
    WinampSkin skin={0}; SkinState state={0},old; signed char pcm[256];
    SkinVisualDecay decay; char error[160]; clock_t start,end; unsigned long n; int i;
    assert(skin_load_file(&skin,"Skins/MintAMP-mintleaf.wsz",error,sizeof(error)));
    for (i=0;i<256;++i) pcm[i]=(signed char)((i/4)%2 ? -127 : 127);
    memset(&decay,0,sizeof(decay));
    start=clock();
    for (n=0;n<100000;++n) { unsigned char target[19]; skin_visual_spectrum(pcm,target); skin_visual_decay(&decay,target,state.levels,state.peaks,50); }
    end=clock();
    printf("Host 128-point FFT spectrum+decay: 100000 updates in %lu ms (host CPU only)\n",(unsigned long)(end-start)*1000/CLOCKS_PER_SEC);
    strcpy(state.title,"MINTAMP"); state.visual_mode=1;
    skin_render(&skin,&state,NULL,blit,fill,NULL); old=state; draws=0;
    for (n=0;n<100000;++n) skin_render(&skin,&state,&old,blit,fill,NULL);
    assert(!draws); puts("100000 stationary renders: zero graphics calls");
    for (i=0;i<19;++i) state.levels[i]=state.peaks[i]=15;
    draws=0; skin_render(&skin,&state,&old,blit,fill,NULL);
    assert(draws<=39); printf("Changed spectrum: %u graphics calls (maximum 39 with peaks)\n",draws);
    skin_free(&skin); return 0;
}

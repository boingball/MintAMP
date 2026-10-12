#include "launch_options.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    MintLaunchOptions o,before; const char *audio; char q[64];
    char *args[]={"MintAMP-Launch","Work:my song.mp3","--gui","REACTION","MODE=SKINNED","--normal","--skin","Work:my skin.wsz"};
    char *bad[]={"launch","--skin"}; char *unknown[]={"launch","--bogus"};
    char *url[]={"launch","https://station/?token=value"}; char *many[]={"launch","one","two"};
    mint_launch_defaults(&o); assert(!strcmp(mint_launch_binary(&o),"MintAMP") && !o.skin_set && !o.size_set);
    /* saved layer -> ToolTypes layer -> CLI */
    assert(mint_launch_option(&o,"GUI","GADTOOLS") && mint_launch_option(&o,"MODE","SKINNED"));
    assert(!strcmp(mint_launch_binary(&o),"MintAMP-SGT"));
    assert(mint_launch_option(&o,"DOUBLESIZE","yes"));
    assert(mint_launch_option(&o,"MODE","native")); assert(!strcmp(mint_launch_binary(&o),"MintAMP-GT"));
    assert(mint_launch_args(&o,8,args,&audio)); assert(!strcmp(audio,"Work:my song.mp3"));
    assert(!o.gadtools && o.skinned && !o.doublesize && o.size_set && o.skin_set);
    assert(!strcmp(mint_launch_binary(&o),"MintAMP-SR") && !strcmp(o.skin,"Work:my skin.wsz"));
    before=o; assert(!mint_launch_option(&o,"GUI","bad") && !memcmp(&o,&before,sizeof(o)));
    assert(!mint_launch_option(&o,"SKIN","a\nb") && !memcmp(&o,&before,sizeof(o)));
    assert(!mint_launch_args(&o,2,bad,&audio)); assert(!mint_launch_args(&o,2,unknown,&audio));
    assert(!mint_launch_args(&o,3,many,&audio)); assert(mint_launch_args(&o,2,url,&audio));
    assert(!strcmp(audio,url[1]));
    assert(mint_launch_quote(q,sizeof(q),"Work:skin *\"x\".wsz"));
    assert(!strcmp(q,"\"Work:skin ***\"x*\".wsz\""));
    assert(!mint_launch_quote(q,4,"abcdef") && !mint_launch_quote(q,sizeof(q),"a\nb"));
    assert(mint_launch_option(&o,"DOUBLESIZE","No") && !o.doublesize);
    puts("Launcher precedence, four frontends, argument validation/quoting passed"); return 0;
}

#include "radio_runtime_flags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AMIGA_M68K)
#include <exec/types.h>
#include <proto/dos.h>
#include <dos/var.h>
#endif

static int radio_flag_ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int radio_flag_ascii_equal_ignore_case(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (radio_flag_ascii_tolower((unsigned char)*a) !=
            radio_flag_ascii_tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int radio_runtime_flag_value_enabled(const char *value)
{
    if (!value || !*value) return 0;
    if (strcmp(value, "0") == 0) return 0;
    if (radio_flag_ascii_equal_ignore_case(value, "false")) return 0;
    if (radio_flag_ascii_equal_ignore_case(value, "no")) return 0;
    if (radio_flag_ascii_equal_ignore_case(value, "off")) return 0;
    return 1;
}

const char *radio_runtime_flag_raw_getenv(const char *name)
{
    if (!name || !*name) return NULL;
    return getenv(name);
}

#if defined(AMIGA_M68K)
static const char *radio_runtime_flag_getvar_with_flags(const char *name, LONG flags)
{
    static char buf[64];
    LONG len;

    if (!name || !*name) return NULL;
    len = GetVar((STRPTR)name, buf, (LONG)(sizeof(buf) - 1), flags);
    if (len < 0) return NULL;
    if (len >= (LONG)sizeof(buf)) len = (LONG)sizeof(buf) - 1;
    buf[len] = '\0';
    return buf;
}
#endif

const char *radio_runtime_flag_raw_getvar(const char *name)
{
#if defined(AMIGA_M68K)
    const char *value;

    value = radio_runtime_flag_getvar_with_flags(name, 0);
    if (value) return value;
#ifdef GVF_GLOBAL_ONLY
    value = radio_runtime_flag_getvar_with_flags(name, GVF_GLOBAL_ONLY);
    if (value) return value;
#endif
#ifdef GVF_LOCAL_ONLY
    value = radio_runtime_flag_getvar_with_flags(name, GVF_LOCAL_ONLY);
    if (value) return value;
#endif
#else
    (void)name;
#endif
    return NULL;
}

int radio_runtime_flag_enabled(const char *name)
{
    const char *value;

    value = radio_runtime_flag_raw_getenv(name);
    if (value) return radio_runtime_flag_value_enabled(value);
    value = radio_runtime_flag_raw_getvar(name);
    if (value) return radio_runtime_flag_value_enabled(value);
    return 0;
}

static const char *radio_runtime_flag_printable(const char *value)
{
    return (value && *value) ? value : "<unset>";
}

static const char *radio_runtime_effective_probe_text(void)
{
    if (radio_runtime_flag_enabled("MP3_NO_STREAM_PROBE")) return "hard-disabled";
    if (!radio_runtime_flag_enabled("MP3_TEST_ENABLE_STREAM_PROBE")) return "staged-off";
    return "enabled";
}

static const char *radio_runtime_effective_artwork_text(void)
{
    if (radio_runtime_flag_enabled("MP3_NO_ARTWORK")) return "hard-disabled";
    if (!radio_runtime_flag_enabled("MP3_TEST_ENABLE_ARTWORK")) return "staged-off";
    return "enabled";
}

void Radio_LogTestModeSummary(void)
{
    int probeTest = radio_runtime_flag_enabled("MP3_TEST_ENABLE_STREAM_PROBE");
    int artworkTest = radio_runtime_flag_enabled("MP3_TEST_ENABLE_ARTWORK");
    int skipAbortSslFree = radio_runtime_flag_enabled("MP3_SKIP_ABORT_SSL_FREE");

    printf("radio-test-mode: probeTest=%d artworkTest=%d skipAbortSslFree=%d effectiveProbe=%s effectiveArtwork=%s\n",
        probeTest,
        artworkTest,
        skipAbortSslFree,
        radio_runtime_effective_probe_text(),
        radio_runtime_effective_artwork_text());
}

void Radio_LogRuntimeFlagsOnce(void)
{
    static int logged = 0;
    const char *names[5];
    int i;

    if (logged) return;
    logged = 1;
    names[0] = "MP3_NO_STREAM_PROBE";
    names[1] = "MP3_NO_ARTWORK";
    names[2] = "MP3_SKIP_ABORT_SSL_FREE";
    names[3] = "MP3_TEST_ENABLE_STREAM_PROBE";
    names[4] = "MP3_TEST_ENABLE_ARTWORK";
    for (i = 0; i < 5; i++) {
        const char *env_value = radio_runtime_flag_raw_getenv(names[i]);
        const char *getvar_value = radio_runtime_flag_raw_getvar(names[i]);
        int enabled = radio_runtime_flag_enabled(names[i]);
        printf("radio-env: %s getenv=\"%s\" getvar=\"%s\" enabled=%d\n",
            names[i],
            radio_runtime_flag_printable(env_value),
            radio_runtime_flag_printable(getvar_value),
            enabled);
    }
    Radio_LogTestModeSummary();
}

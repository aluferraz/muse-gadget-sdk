# SPDX-License-Identifier: Apache-2.0
"""Run the actual English phonemizer and neural engine on the host."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TTS = ROOT / 'components/muse/local_tts'
VENDOR = TTS / 'vendor'
HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_local_tts.h"
#include "nano_lex_g2p.h"
#include "sanotts_model_data.h"
#include "snt_nano.h"
#include "snt_port.h"
static int count, nonzero, abort_now;
static FILE *raw;
void snt_par_run(snt_par_fn f, int n, void *ctx) { f(0,n,ctx); }
int snt_scratch_id(void) { return 0; }
int64_t snt_now_us(void) { return 0; }
static int sink(const float *p, int n, void *u) {
    (void)u;
    for (int i=0;i<n;i++) {
        assert(isfinite(p[i]));
        if (fabsf(p[i]) > .001f) nonzero++;
        if (raw) { float v=fmaxf(-1,fminf(1,p[i])); short s=(short)(v*28000); fwrite(&s,2,1,raw); }
    }
    count += n;
    return abort_now;
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1],"split")) {
        const char *original="Hello there. A very long sentence with many words and café café café!";
        const char *cursor=original; char phrase[17]; size_t loops=0;
        while (*cursor) {
            const char *before=cursor;
            size_t n=muse_tts_phrase(&cursor,phrase,sizeof(phrase));
            assert(n && n<sizeof(phrase) && cursor>before);
            assert(!((unsigned char)*cursor >= 0x80 && (unsigned char)*cursor < 0xc0));
            assert(++loops<20);
        }
        cursor="**hello**\nworld";
        assert(muse_tts_phrase(&cursor,phrase,sizeof(phrase)));
        assert(!strchr(phrase,'*') && !strchr(phrase,'\n'));
        return 0;
    }
    int32_t ids[NANO_LEX_MAX_TOKENS];
    int n=nano_lex_g2p_text_to_ids("Hello. My voice is generated on this device.",ids,NANO_LEX_MAX_TOKENS);
    assert(n>2 && ids[0]==1 && ids[n-1]==2);
    void *arena=malloc(96*1024); assert(arena);
    snt_nano_config cfg={.front_blob=SANOTTS_FRONT_BLOB,.dec_blob=SANOTTS_DEC_BLOB,
        .arena=arena,.arena_size=96*1024,.noise_seed=42};
    snt_nano_stats stats={0};
    if (argc>1) raw=fopen(argv[1],"wb");
    assert(snt_nano_synthesize(&cfg,ids,n,sink,NULL,&stats)==0);
    if(raw) fclose(raw); raw=NULL;
    assert(count>24000 && nonzero>1000 && stats.samples==count && stats.arena_peak<=96*1024);
    abort_now=1; count=0;
    assert(snt_nano_synthesize(&cfg,ids,n,sink,NULL,&stats)!=0);
    assert(count>0 && count<24000);
    free(arena);
    return 0;
}
'''

class LocalTts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path=Path(cls.tmp.name)
        source=path/'test.c'; source.write_text(HARNESS)
        cls.binary=path/'test'
        cmd=[*shlex.split(os.environ.get('CC','cc')), '-std=c11','-O1',
             '-fsanitize=address,undefined','-DNANO_LEX_WITH_SILVER=0','-DSNT_NANO_FAST_MATH',
             '-I',str(TTS),'-I',str(VENDOR),str(source),str(TTS/'muse_tts_text.c')]
        cmd += [str(VENDOR/name) for name in ['nano_lex_g2p.c','nano_lex_tables.c',
                 'sanotts_model_data.c','snt_nano.c','snt_kernels_ref.c']]
        subprocess.run([*cmd,'-lm','-o',str(cls.binary)],check=True,capture_output=True)

    def test_neural_speech_and_abort(self):
        subprocess.run([str(self.binary)],check=True,capture_output=True,timeout=60)

    def test_phrase_boundaries(self):
        subprocess.run([str(self.binary),'split'],check=True,capture_output=True,timeout=10)

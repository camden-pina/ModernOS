#!/usr/bin/env python3
"""Exercise the production GUI snapshot validator under ASan and UBSan."""
import subprocess,tempfile,os
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=r'''
#include "codeoss_view.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned rnd=0x735abb13;
static unsigned next(void){rnd^=rnd<<13;rnd^=rnd>>17;rnd^=rnd<<5;return rnd;}
static struct code_response base(void){return (struct code_response){.magic=CODE_MAGIC,.version=CODE_VERSION,.sequence=3,.action=CODE_SELECT,.document=9,.path_bytes=2,.revision=1,.lines=1,.eol=1,.tab=4};}
int main(void){
 struct code_response r=base();char empty[]="/x";assert(code_header_valid(&r,3,CODE_SELECT,9));assert(code_payload_valid(&r,empty));
 assert(!code_header_valid(&r,3,CODE_SELECT,8));
 const char *unicode="/x" "a\t\xf0\x9f\x98\x80\r\nz";r.text_bytes=9;r.lines=2;r.cursor=6;r.anchor=6;r.line=1;r.column=0;assert(code_header_valid(&r,3,CODE_SELECT,9));assert(code_payload_valid(&r,unicode));
 r.cursor=3;assert(!code_payload_valid(&r,unicode));r.cursor=5;assert(!code_payload_valid(&r,unicode));r.cursor=6;r.column=1;assert(!code_payload_valid(&r,unicode));r.column=0;r.lines=3;assert(!code_payload_valid(&r,unicode));
 const unsigned char *bad[]={(const unsigned char*)"\xc0\xaf",(const unsigned char*)"\xed\xa0\x80",(const unsigned char*)"\xf4\x90\x80\x80",(const unsigned char*)"\xe2\x82",(const unsigned char*)"\x80"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){assert(!code_utf8((const char*)bad[i],strlen((const char*)bad[i])));}
 assert(!code_utf8("a\0b",3));
 r=base();r.text_bytes=CODE_TEXT_LIMIT;r.flags=CODE_BOM;assert(!code_header_valid(&r,3,CODE_SELECT,9));r.text_bytes=UINT32_MAX-2;assert(!code_header_valid(&r,3,CODE_SELECT,9));r=base();r.path_bytes=0;assert(!code_header_valid(&r,3,CODE_SELECT,9));r=base();r.selection_bytes=CODE_CLIP_LIMIT+1;assert(!code_header_valid(&r,3,CODE_SELECT,9));r=base();r.status=2;assert(!code_header_valid(&r,3,CODE_SELECT,9));
 for(unsigned i=0;i<100000;i++){
  r=base();unsigned *fields=(unsigned*)&r;fields[next()%(sizeof(r)/4)]=next();(void)code_header_valid(&r,3,CODE_SELECT,9);
  r=base();r.text_bytes=next()%1000;r.selection_bytes=next()%300;r.message_bytes=next()%40;r.cursor=next()%1100;r.anchor=next()%1100;r.lines=1+next()%30;r.line=next()%r.lines;r.column=next()%1200;
  size_t n=r.path_bytes+r.text_bytes+r.selection_bytes+r.message_bytes;char *p=malloc(n);assert(p);for(size_t j=0;j<n;j++)p[j]=(char)next();p[0]='/';p[1]='x';assert(code_header_valid(&r,3,CODE_SELECT,9));(void)code_payload_valid(&r,p);free(p);
 }
 puts("PASS production snapshot validator: exact-size payloads, Unicode/CRLF boundaries, limits and 100000 malformed frame pairs (ASan/UBSan)");return 0;
}'''
with tempfile.TemporaryDirectory(prefix='coast-codeoss-protocol-') as temp:
 p=Path(temp);(p/'check.c').write_text(source)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-I'+str(root/'userland/desktop'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

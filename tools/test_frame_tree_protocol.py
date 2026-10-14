#!/usr/bin/env python3
"""Exercise bounded frame-tree parsing with malformed input under ASan/UBSan."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "userland/include/web_frame_tree.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, mutations, accepted;
static unsigned char bytes[WEB_FRAME_TREE_MAX_BYTES+1], saved[4096];
static size_t size, positions[WEB_FRAME_TREE_MAX_NODES];
static uint32_t nodes;
#define REQUIRE(value) do { if(!(value)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#value); exit(1); } } while(0)
static uint32_t random_state=0x50cf49abU;
static uint32_t random_value(void) {
    random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;
    return random_state;
}
static void begin(uint64_t root) {
    struct web_frame_tree_header header={WEB_FRAME_TREE_MAGIC,WEB_FRAME_TREE_VERSION,0,0,root};
    memcpy(bytes,&header,sizeof(header));size=sizeof(header);nodes=0;
}
static void add(uint64_t id,uint64_t parent,uint32_t flags,const void *name,uint32_t name_size,
                const void *origin,uint32_t origin_size) {
    REQUIRE(nodes<WEB_FRAME_TREE_MAX_NODES);
    REQUIRE(size+sizeof(struct web_frame_tree_node)+name_size+origin_size<=sizeof(bytes));
    positions[nodes++]=size;
    struct web_frame_tree_node node={id,parent,flags,name_size,origin_size,0};
    memcpy(bytes+size,&node,sizeof(node));size+=sizeof(node);
    if(name_size)memcpy(bytes+size,name,name_size);
    size+=name_size;
    if(origin_size)memcpy(bytes+size,origin,origin_size);
    size+=origin_size;
    memcpy(bytes+offsetof(struct web_frame_tree_header,count),&nodes,sizeof(nodes));
}
static void expect(int valid,const char *why) {
    struct web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES];uint32_t count=0;uint64_t root=0;
    int result=web_frame_tree_decode(bytes,size,spans,&count,&root);checks++;
    if(result!=valid) {fprintf(stderr,"FAIL %s: accepted=%d expected=%d bytes=%zu\n",why,result,valid,size);exit(1);}
    if(result) {
        REQUIRE(count && count<=WEB_FRAME_TREE_MAX_NODES && root);
        REQUIRE(spans[0].node.frame==root && !spans[0].node.parent);
        for(unsigned i=0;i<count;i++) {
            REQUIRE(spans[i].name>=bytes && spans[i].origin>=bytes);
            REQUIRE(spans[i].name+spans[i].node.name_bytes<=bytes+size);
            REQUIRE(spans[i].origin+spans[i].node.origin_bytes<=bytes+size);
            REQUIRE(spans[i].depth<=WEB_FRAME_TREE_MAX_DEPTH);
        }
    }
}
static void patch_header(size_t at,uint32_t value) {memcpy(bytes+at,&value,sizeof(value));}
static void patch_node(unsigned index,size_t at,uint32_t value) {patch_header(positions[index]+at,value);}
static void simple(void) {
    static const unsigned char unicode_name[]={'a',0,0xf0,0x9f,0x90,0x88};
    begin(1);add(1,0,WEB_FRAME_TREE_LOCAL,"root",4,"https://example.test",20);
    add(2,1,0,unicode_name,sizeof(unicode_name),"null",4);
    add(3,2,0,"leaf",4,"https://foreign.test",20);
}
static void reject_name(const unsigned char *name,uint32_t count,const char *why) {
    begin(1);add(1,0,WEB_FRAME_TREE_LOCAL,name,count,"null",4);expect(0,why);
}
int main(void) {
    struct web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES];uint32_t count;uint64_t root;
    REQUIRE(!web_frame_tree_decode(NULL,0,spans,&count,&root));checks++;
    simple();expect(1,"valid nested tree and embedded NUL/UTF8 name");
    size_t original_size=size;memcpy(saved,bytes,size);
    for(size_t prefix=0;prefix<original_size;prefix++) {size=prefix;expect(0,"truncated tree");}
    size=original_size;bytes[size++]=0;expect(0,"trailing byte");
#define HEADER_BAD(field,value,why) do {simple();patch_header(offsetof(struct web_frame_tree_header,field),value);expect(0,why);} while(0)
    HEADER_BAD(magic,0,"unknown magic");HEADER_BAD(version,2,"unknown version");
    HEADER_BAD(reserved,1,"reserved header bits");HEADER_BAD(count,0,"empty tree");
    HEADER_BAD(count,WEB_FRAME_TREE_MAX_NODES+1,"node quota");HEADER_BAD(count,UINT32_MAX,"count overflow");
    simple();uint64_t zero=0;memcpy(bytes+offsetof(struct web_frame_tree_header,root),&zero,sizeof(zero));expect(0,"zero root");
#define NODE_BAD(field,value,why) do {simple();patch_node(0,offsetof(struct web_frame_tree_node,field),value);expect(0,why);} while(0)
    NODE_BAD(reserved,1,"reserved node bits");NODE_BAD(flags,2,"unknown node flags");
    NODE_BAD(name_bytes,WEB_FRAME_TREE_MAX_NAME+1,"name quota");NODE_BAD(name_bytes,UINT32_MAX,"name length overflow");
    NODE_BAD(origin_bytes,0,"missing origin");NODE_BAD(origin_bytes,WEB_FRAME_TREE_MAX_ORIGIN+1,"origin quota");
    NODE_BAD(origin_bytes,UINT32_MAX,"origin length overflow");
    simple();memcpy(bytes+positions[0]+offsetof(struct web_frame_tree_node,frame),&zero,sizeof(zero));expect(0,"zero ID");
    simple();uint64_t wrong=7;memcpy(bytes+positions[0]+offsetof(struct web_frame_tree_node,frame),&wrong,sizeof(wrong));expect(0,"root mismatch");
    simple();memcpy(bytes+positions[0]+offsetof(struct web_frame_tree_node,parent),&wrong,sizeof(wrong));expect(0,"root parent");
    begin(1);add(1,0,1,"",0,"null",4);add(1,1,1,"",0,"null",4);expect(0,"duplicate ID");
    begin(1);add(1,0,1,"",0,"null",4);add(2,7,1,"",0,"null",4);expect(0,"dangling parent");
    begin(1);add(1,0,1,"",0,"null",4);add(2,2,1,"",0,"null",4);expect(0,"self parent");
    begin(1);add(1,0,1,"",0,"null",4);add(2,3,1,"",0,"null",4);add(3,2,1,"",0,"null",4);expect(0,"cycle");
    begin(1);add(1,0,1,"",0,"null",4);add(2,0,1,"",0,"null",4);expect(0,"second root");
    begin(1);add(1,0,1,"",0,"null",4);add(2,1,1,"",0,"null",4);add(3,1,1,"",0,"null",4);add(4,2,1,"",0,"null",4);expect(0,"reopened sibling subtree");
    begin(1);for(unsigned i=0;i<=WEB_FRAME_TREE_MAX_DEPTH;i++)add(i+1,i,1,"",0,"null",4);expect(1,"maximum depth");
    add(WEB_FRAME_TREE_MAX_DEPTH+2,WEB_FRAME_TREE_MAX_DEPTH+1,1,"",0,"null",4);expect(0,"depth exceeded");
    begin(1);add(1,0,1,"",0,"null",4);for(unsigned i=1;i<WEB_FRAME_TREE_MAX_NODES;i++)add(i+1,1,0,"",0,"null",4);expect(1,"maximum width/count");
    size=WEB_FRAME_TREE_MAX_BYTES+1;expect(0,"body quota");
    const unsigned char overlong[]={0xc0,0xaf},surrogate[]={0xed,0xa0,0x80},too_large[]={0xf4,0x90,0x80,0x80};
    const unsigned char incomplete[]={0xe2,0x82},lone[]={0x80},bad_continuation[]={0xe2,0x41,0x82};
    reject_name(overlong,sizeof(overlong),"overlong UTF8");reject_name(surrogate,sizeof(surrogate),"UTF8 surrogate");
    reject_name(too_large,sizeof(too_large),"UTF8 above Unicode maximum");reject_name(incomplete,sizeof(incomplete),"truncated UTF8");
    reject_name(lone,sizeof(lone),"lone continuation");reject_name(bad_continuation,sizeof(bad_continuation),"bad continuation");
    const unsigned char limit[]={0xf4,0x8f,0xbf,0xbf};
    begin(1);add(1,0,1,limit,sizeof(limit),"null",4);expect(1,"maximum Unicode scalar");
    for(unsigned control=0;control<=0x20;control++) {
        unsigned char origin[]={'n','u','l','l'};origin[2]=control;
        begin(1);add(1,0,1,"",0,origin,sizeof(origin));expect(0,"origin control/NUL/space");
    }
    for(unsigned invalid=0x7f;invalid<=0xff;invalid++) {
        unsigned char origin[]={'n','u','l','l'};origin[2]=invalid;
        begin(1);add(1,0,1,"",0,origin,sizeof(origin));expect(0,"non-ASCII origin");
    }
    simple();original_size=size;memcpy(saved,bytes,size);
    for(unsigned iteration=0;iteration<20000;iteration++) {
        memcpy(bytes,saved,original_size);size=original_size;
        unsigned changes=1+random_value()%16;
        for(unsigned change=0;change<changes;change++)bytes[random_value()%original_size]=(unsigned char)random_value();
        if(iteration%3==0)size=random_value()%(original_size+16);
        if(size>original_size)for(size_t i=original_size;i<size;i++)bytes[i]=(unsigned char)random_value();
        uint32_t output_count=0;uint64_t output_root=0;
        int result=web_frame_tree_decode(bytes,size,spans,&output_count,&output_root);mutations++;
        if(result) {
            accepted++;REQUIRE(output_count && output_count<=WEB_FRAME_TREE_MAX_NODES && output_root);
            for(unsigned i=0;i<output_count;i++) {
                REQUIRE(spans[i].name>=bytes && spans[i].origin>=bytes);
                REQUIRE(spans[i].origin+spans[i].node.origin_bytes<=bytes+size);
                REQUIRE(spans[i].depth<=WEB_FRAME_TREE_MAX_DEPTH);
            }
        }
    }
    printf("PASS frame tree parser: %u boundary/malformed checks and %u seeded mutations (%u valid), ASan/UBSan\n",checks,mutations,accepted);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()

    work = args.output_dir or ROOT / 'build' / ('frame-tree-protocol-' + str(time.time_ns()))
    work.mkdir(parents=True, exist_ok=False)
    source = work / 'frame-tree-parser-check.c'
    source.write_text(HARNESS)
    binary = work / 'frame-tree-parser-check'
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I' + str(ROOT), str(source), '-o', str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (work / 'compile.log').write_text(compiled.stdout + compiled.stderr)

    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    environment = os.environ | {'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
    (work / 'result.log').write_text(result.stdout + result.stderr)
    (work / 'result.json').write_text(json.dumps({'compile_command': command,
                                                  'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                                                 'passed': result.returncode == 0}, indent=2) + '\n')

    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    print(result.stdout.rstrip())

    print('Evidence:', work)


if __name__ == '__main__':
    main()

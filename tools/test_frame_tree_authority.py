#!/usr/bin/env python3
"""Host-check the real broker tree policy with minimal transport/process stubs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#define _GNU_SOURCE
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "userland/include/web_frame_protocol.h"
typedef struct NativeBrowser NativeBrowser;
typedef struct {char *origin;int channel,broken;} Broker;
typedef struct {uint64_t frame,parent;int local;GBytes *name;char *origin;} NativeFrameNode;
struct NativeBrowser {
    Broker broker;int pid;NativeBrowser *parent,*children[WEB_FRAME_MAX_CHILDREN];
    uint64_t frame_id,root_frame;unsigned sandbox;
    NativeFrameNode *tree;unsigned tree_count;uint64_t tree_revision,tree_export_revision;
    GBytes *tree_export;GHashTable *tree_names;char *committed_origin,*tree_url_origin,*pending;
};
static unsigned transport_calls;
static NativeBrowser *native_child(NativeBrowser *view,uint64_t id) {
    for(unsigned i=0;i<WEB_FRAME_MAX_CHILDREN;i++)if(view->children[i] && view->children[i]->frame_id==id)return view->children[i];
    return NULL;
}
static int web_resource_body(const void *bytes,size_t size) {(void)bytes;(void)size;transport_calls++;return -1;}
static int native_frame_forward(NativeBrowser *view,struct web_frame_packet packet,const char *metadata,int body) {
    (void)view;(void)packet;(void)metadata;(void)body;transport_calls++;return 0;
}
'''
SUFFIX = r'''
#include "userland/modern-browser/native-frame-tree.inc"
#define REQUIRE(value) do { if(!(value)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#value);exit(1);} } while(0)
static const uint64_t root_id=(uint64_t)11<<32|1,local_id=(uint64_t)11<<32|3,remote_id=(uint64_t)11<<32|2;
static NativeBrowser view,child;
static GByteArray *snapshot;
static unsigned checks;
static void reset(const char *origin) {
    native_tree_clear(&child);native_tree_clear(&view);
    memset(&view,0,sizeof(view));memset(&child,0,sizeof(child));
    view.pid=11;view.root_frame=root_id;view.broker.channel=-1;view.broker.origin=(char*)origin;
    child.pid=22;child.frame_id=remote_id;child.root_frame=remote_id;child.broker.channel=-1;
    child.broker.origin="https://foreign.test:443";child.parent=&view;view.children[0]=&child;
}
static void begin(uint64_t root) {
    if(snapshot)g_byte_array_unref(snapshot);
    snapshot=g_byte_array_new();
    struct web_frame_tree_header header={WEB_FRAME_TREE_MAGIC,WEB_FRAME_TREE_VERSION,0,0,root};
    g_byte_array_append(snapshot,(const void*)&header,sizeof(header));
}
static void add(uint64_t id,uint64_t parent,int local,const void *name,size_t name_size,const char *origin) {
    struct web_frame_tree_node node={.frame=id,.parent=parent,.flags=local?WEB_FRAME_TREE_LOCAL:0,
        .name_bytes=name_size,.origin_bytes=strlen(origin)};
    g_byte_array_append(snapshot,(const void*)&node,sizeof(node));
    if(name_size)g_byte_array_append(snapshot,name,name_size);
    g_byte_array_append(snapshot,(const void*)origin,strlen(origin));
    uint32_t count;memcpy(&count,snapshot->data+offsetof(struct web_frame_tree_header,count),sizeof(count));count++;
    memcpy(snapshot->data+offsetof(struct web_frame_tree_header,count),&count,sizeof(count));
}
static void check(NativeBrowser *owner,int expected,const char *why) {
    int result=native_tree_accept(owner,snapshot->data,snapshot->len,owner->tree_revision+1);checks++;
    if(result!=expected) {fprintf(stderr,"FAIL %s: accepted=%d expected=%d\n",why,result,expected);exit(1);}
    REQUIRE(!transport_calls);
}
static void root_record(const char *origin) {begin(root_id);add(root_id,0,1,"root",4,origin);}
static struct web_frame_tree_span *export_tree(NativeBrowser *owner,GBytes **bytes,uint32_t *count) {
    static struct web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES];uint64_t root;
    *bytes=native_tree_export(owner);REQUIRE(*bytes);
    gsize size;const void *data=g_bytes_get_data(*bytes,&size);
    REQUIRE(web_frame_tree_decode(data,size,spans,count,&root) && root==root_id);return spans;
}
static void expect_origin(const struct web_frame_tree_span *span,const char *origin) {
    REQUIRE(span->node.origin_bytes==strlen(origin) && !memcmp(span->origin,origin,strlen(origin)));
}
int main(void) {
    reset("https://example.test:443");root_record("https://example.test");
    add(local_id,root_id,1,"local",5,"https://example.test");
    add(remote_id,local_id,0,"child",5,"https://attacker.test");check(&view,1,"default HTTPS and genuine local/remote IDs");
    GBytes *wire;uint32_t count;struct web_frame_tree_span *spans=export_tree(&child,&wire,&count);
    REQUIRE(count==3 && !(spans[0].node.flags&WEB_FRAME_TREE_LOCAL) && (spans[2].node.flags&WEB_FRAME_TREE_LOCAL));
    expect_origin(&spans[0],"https://example.test");expect_origin(&spans[2],"https://foreign.test");g_bytes_unref(wire);checks++;
    NativeFrameNode *found=NULL;
    REQUIRE(native_tree_owner(&view,local_id,&found)==&view && found && found->local);
    REQUIRE(native_tree_owner(&view,remote_id,&found)==&child);checks++;
    reset("http://example.test:80");root_record("http://example.test");check(&view,1,"default HTTP serialization");
    reset("https://example.test:8443");root_record("https://example.test:8443");check(&view,1,"nondefault port preserved");
    reset("https://example.test:443");root_record("https://foreign.test");check(&view,0,"foreign local origin");
    root_record("http://example.test");check(&view,0,"scheme forgery");
    root_record("https://example.test:8443");check(&view,0,"port forgery");
    root_record("https://example.test:443");check(&view,0,"noncanonical explicit default port");
    root_record("HTTPS://EXAMPLE.TEST");check(&view,0,"noncanonical origin case");
    root_record("https://example.test/path");check(&view,0,"origin path masquerade");
    root_record("https://user@example.test");check(&view,0,"userinfo masquerade");
    root_record("null");check(&view,1,"opaque local origin allowed");
    root_record("https://example.test");add((uint64_t)22<<32|7,root_id,1,"",0,"https://example.test");check(&view,0,"another renderer local ID");
    root_record("https://example.test");add(remote_id,root_id,1,"",0,"https://example.test");
    /* IDs owned by this PID cannot change an existing foreign leaf to local. */
    check(&view,0,"existing remote process root claimed local");
    begin(root_id+9);add(root_id+9,0,1,"",0,"https://example.test");check(&view,0,"different bound root");
    begin(root_id);add(root_id,0,0,"",0,"https://example.test");check(&view,0,"root must be local");
    root_record("https://example.test");add((uint64_t)11<<32|99,root_id,0,"",0,"https://foreign.test");check(&view,0,"unknown foreign leaf");
    root_record("https://example.test");add(remote_id,root_id,0,"",0,"https://foreign.test");
    add(local_id,remote_id,1,"",0,"https://example.test");check(&view,0,"remote descendants claimed");
    reset("https://example.test:443");root_record("https://example.test");
    const unsigned char embedded_name[]={'n',0,'m'};
    add(remote_id,root_id,0,embedded_name,sizeof(embedded_name),"https://foreign.test");check(&view,1,"embedded NUL name");
    child.root_frame=remote_id;
    begin(remote_id);add(remote_id,0,1,"stale",5,"https://foreign.test");check(&child,1,"inherited root ID exception");
    spans=export_tree(&child,&wire,&count);REQUIRE(count==2);
    REQUIRE(spans[1].node.name_bytes==sizeof(embedded_name) && !memcmp(spans[1].name,embedded_name,sizeof(embedded_name)));g_bytes_unref(wire);checks++;
    view.children[0]=NULL;
    root_record("https://example.test");add(remote_id,root_id,0,embedded_name,sizeof(embedded_name),"https://foreign.test");check(&view,1,"previously owned removed leaf tombstone");
    spans=export_tree(&view,&wire,&count);REQUIRE(count==1);g_bytes_unref(wire);checks++;
    reset("https://example.test:443");child.sandbox=4;
    root_record("https://example.test");add(remote_id,root_id,0,"",0,"https://foreign.test");check(&view,1,"sandboxed bootstrap leaf");
    spans=export_tree(&child,&wire,&count);REQUIRE(count==2);expect_origin(&spans[1],"null");g_bytes_unref(wire);checks++;
    native_tree_clear(&child);native_tree_clear(&view);g_byte_array_unref(snapshot);
    printf("PASS broker frame tree authority: %u ownership/origin/leaf/name/export checks, ASan/UBSan\n",checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()

    work = args.output_dir or ROOT / 'build' / ('frame-tree-authority-' + str(time.time_ns()))
    work.mkdir(parents=True, exist_ok=False)

    broker = (ROOT / 'userland/modern-browser/web-broker.c').read_text()
    origin = broker[broker.index('static char *origin_for('):broker.index('static void init(')]
    source = work / 'frame-tree-authority-check.c'
    source.write_text(PREFIX + origin + SUFFIX)

    binary = work / 'frame-tree-authority-check'
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
        '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-save-temps=obj',
        '-I' + str(ROOT), str(source), '-o', str(binary)] + flags

    paths = ['userland/modern-browser/native-frame-tree.inc', 'userland/modern-browser/web-broker.c',
             'userland/include/web_frame_tree.h']
    before = {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in paths}

    for path in paths:
        saved = work / 'source' / path
        saved.parent.mkdir(parents=True, exist_ok=True)
        saved.write_bytes((ROOT / path).read_bytes())

    compiled = subprocess.run(command, capture_output=True, text=True)
    (work / 'compile.log').write_text(compiled.stdout + compiled.stderr)

    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)

    env = os.environ | {'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                        'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
    (work / 'result.log').write_text(result.stdout + result.stderr)

    after = {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in paths}
    report = {'compile_command': command, 'exit_code': result.returncode, 'stdout': result.stdout,
              'stderr': result.stderr, 'source_hashes': before, 'sources_unchanged_during_test': before == after,
              'passed': result.returncode == 0 and before == after}
    (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    if not report['passed']:
        raise RuntimeError(result.stdout + result.stderr + str(report))
    print(result.stdout.rstrip());

    print('Evidence:', work)


if __name__ == '__main__':
    main()

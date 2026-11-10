"""Read-only GDB inspection of a paused, owned QA guest.

Use with the exact guest kernel's debug ELF. COAST_TASK_DUMP selects the
JSON output. Optional COAST_UV_LAYOUT supplies offsets measured against a
specific runtime; its host binary checksum is verified before reading libuv.
No inferior functions are called and no guest memory is written.
"""
import gdb,hashlib,json,os,struct

uv_layout=None
if os.environ.get('COAST_UV_LAYOUT'):
    with open(os.environ['COAST_UV_LAYOUT']) as stream:uv_layout=json.load(stream)
    with open(uv_layout['binary'],'rb') as stream:
        assert hashlib.file_digest(stream,'sha256').hexdigest()==uv_layout['binary_sha256'],'Runtime layout checksum mismatch'

ipcz_layout=None
if os.environ.get('COAST_IPCZ_LAYOUT'):
    with open(os.environ['COAST_IPCZ_LAYOUT']) as stream:ipcz_layout=json.load(stream)
    with open(ipcz_layout['binary'],'rb') as stream:
        assert hashlib.file_digest(stream,'sha256').hexdigest()==ipcz_layout['binary_sha256'],'ipcz layout checksum mismatch'

inferior=gdb.selected_inferior()
assert inferior.pid,'No live owned guest is attached'
hhdm=int(gdb.parse_and_eval('boot_info.hhdm_offset'))
assert hhdm,'Missing live kernel direct map'
mask=0x000ffffffffff000
def read(address,length):return bytes(inferior.read_memory(address,length))
def word(address):return struct.unpack('<Q',read(address,8))[0]
def array(value):
    first,last=value.type.range()
    return [value[i] for i in range(first,last+1)]
def physical(root,address):
    table=root&mask
    for shift in [39,30,21,12]:
        entry=word(hhdm+table+((address>>shift)&511)*8)
        if not entry&1:raise ValueError('unmapped')
        if shift in [30,21] and entry&128:
            return (entry&mask&~((1<<shift)-1))+(address&((1<<shift)-1))
        table=entry&mask
    return table+(address&4095)
def user_word(root,address):return word(hhdm+physical(root,address))
def user_bytes(root,address,size):
    result=bytearray()
    while size:
        count=min(size,4096-(address&4095))
        result.extend(read(hhdm+physical(root,address),count));address+=count;size-=count
    return bytes(result)

def uv_state(root):
    """Inspect bounded queues using externally measured, checksum-pinned offsets."""
    layout=uv_layout;offsets=layout['offsets']
    loop=user_word(root,layout['image_bias']+layout['default_loop_pointer_rva'])
    if not loop:return {'initialized':False}
    data=user_bytes(root,loop,offsets['uv_loop_t.size'])
    def field(name,fmt):return struct.unpack_from(fmt,data,offsets['uv_loop_t.'+name])[0]
    def queue(head,limit=256):
        result=[];node=user_word(root,head)
        while node!=head:
            if not node or node in result or len(result)>=limit:raise ValueError('Invalid or oversized libuv queue')
            result.append(node);node=user_word(root,node)
        return result
    def scalar(address,fmt):return struct.unpack(fmt,user_bytes(root,address,struct.calcsize(fmt)))[0]
    result={'initialized':True,'address':hex(loop),'backend_fd':field('backend_fd','<i'),
            'active_handles':field('active_handles','<I'),'active_requests':field('active_reqs','<I'),
            'stop_flag':field('stop_flag','<I'),'time_ms':field('time','<Q'),
            'timer_count':struct.unpack_from('<I',data,offsets['uv_loop_t.timer_heap']+8)[0],
            'pending_callbacks':len(queue(loop+offsets['uv_loop_t.pending_queue'])),
            'pending_watchers':len(queue(loop+offsets['uv_loop_t.watcher_queue'])),
            'nfds':field('nfds','<I'),'watchers':[],'async_handles':[],'handles':[]}
    count=field('nwatchers','<I');watchers=field('watchers','<Q')
    if count>4096:raise ValueError('Unexpected libuv watcher array size')
    if count:
        pointers=struct.unpack('<'+'Q'*count,user_bytes(root,watchers,count*8))
        for fd,pointer in enumerate(pointers):
            if pointer:
                io=user_bytes(root,pointer,offsets['uv__io_t.size'])
                result['watchers'].append({'fd':fd,'address':hex(pointer),
                    **{name:struct.unpack_from(fmt,io,offsets['uv__io_t.'+name])[0]
                       for name,fmt in [('bits','<Q'),('events','<I'),('pevents','<I')]}})
    for node in queue(loop+offsets['uv_loop_t.async_handles']):
        handle=node-offsets['uv_async_t.queue']
        result['async_handles'].append({'address':hex(handle),
            'pending':scalar(handle+offsets['uv_async_t.pending'],'<i'),
            'callback':hex(user_word(root,handle+offsets['uv_async_t.async_cb']))})
    for node in queue(loop+offsets['uv_loop_t.handle_queue']):
        handle=node-offsets['uv_handle_t.handle_queue']
        result['handles'].append({'address':hex(handle),
            'type':scalar(handle+offsets['uv_async_t.type'],'<I'),
            'flags':scalar(handle+offsets['uv_handle_t.flags'],'<I')})
    timer=struct.unpack_from('<Q',data,offsets['uv_loop_t.timer_heap'])[0]
    if timer:
        handle=timer-offsets['uv_timer_t.node']
        result['next_timer']={'address':hex(handle),
            'timeout_ms':scalar(handle+offsets['uv_timer_t.timeout'],'<Q'),
            'repeat_ms':scalar(handle+offsets['uv_timer_t.repeat'],'<Q')}
    return result

def ipcz_state(root):
    """Decode bounded ipcz route queues using exact compiler-measured layouts."""
    l=ipcz_layout;bias=l['image_bias'];node=user_word(root,bias+l['g_node_rva'])
    if not node:return {'initialized':False}
    def scalar(address,fmt):return struct.unpack(fmt,user_bytes(root,address,struct.calcsize(fmt)))[0]
    def u8(address):return scalar(address,'<B')
    def u32(address):return scalar(address,'<I')
    def ptr(address):return user_word(root,address)
    def name(address):return [hex(ptr(address)),hex(ptr(address+8))]
    def table(address,stride):
        cap=ptr(address);size=ptr(address+8)>>l['map']['size_shift']
        if cap>8191 or size>cap:raise ValueError('Invalid or oversized ipcz map')
        if not size:return {'size':0,'capacity':cap,'entries':[]}
        control=ptr(address+16);slots=ptr(address+24)
        marks=user_bytes(root,control,cap);indices=[i for i,b in enumerate(marks) if b<128]
        if len(indices)!=size:raise ValueError('ipcz map control/size mismatch')
        return {'size':size,'capacity':cap,'entries':[slots+i*stride for i in indices]}
    def queue(address):
        start=ptr(address);end=ptr(address+8);front=ptr(address+24)
        if end<start or (end-start)%48 or (end-start)//48>16384:raise ValueError('Invalid or oversized ipcz parcel queue')
        count=(end-start)//48
        if front>count:raise ValueError('Invalid ipcz queue front')
        samples=[]
        for i in range(front,min(count,front+128)):
            at=start+i*48;engaged=u8(at+40)
            if engaged:samples.append({'index':i,'parcel':hex(ptr(at)),'span_count':ptr(at+8),'span_bytes':ptr(at+16),'span_start':ptr(at+24),'span_end':ptr(at+32)})
        return {'slots':count,'front':front,'base_sequence':ptr(address+40),'final_known':bool(u8(address+32)),
            'front_engaged':bool(u8(start+front*48+40)) if front<count else False,'sampled_entries':samples,'sampled_slots':min(count-front,128)}
    def channel_state(link):
        driver=ptr(link+88);transport=ptr(driver+24);channel=ptr(transport+128)
        if not channel:return {'channel':None,'driver':hex(driver),'transport':hex(transport)}
        def deque_size(address):
            capacity=ptr(address+8);begin=ptr(address+16);end=ptr(address+24)
            if not capacity:return 0
            if begin>=capacity or end>=capacity:raise ValueError('Invalid channel deque')
            return (end-begin)%capacity
        delayed=table(channel+16,64);keys=[u32(at) for at in delayed['entries']]
        return {'address':hex(channel),'driver':hex(driver),'transport':hex(transport),
            'dispatched':u32(channel+12),'last_sent':u32(channel+48),
            'delayed_count':len(keys),'delayed_first':sorted(keys)[:64],'delayed_last':max(keys) if keys else None,
            'socket_fd':scalar(channel+128,'<i'),'read_watch':hex(ptr(channel+136)),'write_watch':hex(ptr(channel+144)),
            'incoming_fds':deque_size(channel+152),'outgoing_messages':deque_size(channel+232),
            'pending_write':bool(u8(channel+224)),'reject_writes':bool(u8(channel+225)),
            'shared_writer':bool(u8(channel+312)),'write_notifier':hex(ptr(channel+320)),'read_notifier':hex(ptr(channel+344))}
    def router(address):
        if u32(address+12)!=1:raise ValueError('Wrong ipcz portal object type')
        begin=ptr(address+32);end=ptr(address+40)
        if end<begin or (end-begin)%48 or (end-begin)//48>128:raise ValueError('Invalid ipcz trap vector')
        traps=[{'flags':u32(at+8),'minimum_parcels':ptr(at+16),'minimum_bytes':ptr(at+24),'handler':hex(ptr(at+32)),'context':hex(ptr(at+40))} for at in range(begin,end,48)]
        for trap in traps:
            handler=int(trap['handler'],16)
            if handler in [bias+l['mojo_trap']['event_handler_rva'],bias+l['mojo_trap']['removal_handler_rva']]:
                context=int(trap['context'],16);owner=ptr(context+8)
                if ptr(context+16)!=address:raise ValueError('ipcz trigger portal mismatch')
                begin=ptr(owner+152);end=ptr(owner+160)
                trap['mojo']={'trigger':hex(context),'owner':hex(owner),'signals':u32(context+32),'context':hex(ptr(context+40)),
                    'trigger_armed':bool(u8(context+80)),'trigger_removed':bool(u8(context+81)),
                    'handler':hex(ptr(owner+16)),'waiters':u32(owner+128),'dispatch_thread':hex(ptr(owner+136)),
                    'dispatch_engaged':bool(u8(owner+144)),'trigger_count':(end-begin)//16,
                    'pending_events':ptr(owner+184)>>1,'armed':bool(u8(owner+352))}
        r={'address':hex(address),'peer_closed':bool(u8(address+24)),'disconnected':bool(u8(address+25)),
            'status':u32(address+28),'traps':traps,'outward_link':hex(ptr(address+56)),'decaying_link':hex(ptr(address+64)),
            'inward_edge':hex(ptr(address+72)),'bridge':hex(ptr(address+80)),
            'inbound':queue(address+88),'outbound':queue(address+136)}
        return r
    if u32(node+12)!=0:raise ValueError('Wrong ipcz node object type')
    result={'initialized':True,'address':hex(node),'name':name(node+96),'broker':hex(ptr(node+112)),
        'allocation_delegate':hex(ptr(node+120)),'connections':[]}
    for slot in table(node+128,32)['entries']:
        link=ptr(slot+16)
        row={'node_link':hex(link),'key':name(slot),'local':name(link+32),'remote':name(link+48),
            'activation':u32(link+112),'next_outgoing_sequence':ptr(link+120),
            'pending_memory':table(link+232,24)['size'],'partial_parcels':table(link+264,24)['size'],
            'early_parcels':table(link+328,32)['size'],'sublinks':[]}
        try:row['channel']=channel_state(link)
        except Exception as error:row['channel_error']=str(error)
        for entry in table(link+128,24)['entries']:
            sublink=ptr(entry);remote_link=ptr(entry+8);receiver=ptr(entry+16)
            sub={'id':sublink,'link':hex(remote_link),'receiver':hex(receiver)}
            if remote_link and ptr(remote_link)==bias+l['remote_link_vtable_rva']:
                state=ptr(remote_link+72)
                sub['remote_state']={'type':u8(remote_link+32),'side':u8(remote_link+36),'stable':bool(u8(remote_link+37)),
                    'state':hex(state),'status':u32(state+4) if state else None}
            try:sub['router']=router(receiver)
            except Exception as e:sub['router_error']=str(e)
            row['sublinks'].append(sub)
        result['connections'].append(row)
    return result

def unwind(root,frame):
    result=[int(frame['rip'])];pointer=int(frame['rbp'])
    for unused in range(16):
        if not 0x10000<=pointer<0x800000000000 or pointer&7:break
        try:following=user_word(root,pointer);ret=user_word(root,pointer+8)
        except Exception:break
        result.append(ret)
        if following<=pointer or following-pointer>8*1024*1024:break
        pointer=following
    return [hex(value) for value in result]

tasks=gdb.parse_and_eval('tasks');rows=[]
frame_type=gdb.lookup_type('register_t').pointer()
try:
    private_streams={int(s['pair']):s for s in array(gdb.parse_and_eval('private_streams')) if int(s['pair'])}
except gdb.error:
    private_streams={}  # Older kernels may not contain private stream support.
for index in range(tasks.type.range()[1]+1):
    task=tasks[index]
    if not int(task['state']):continue
    row={name:int(task[name]) for name in ['pid','tgid','ppid','state','cpu','runtime_ticks','futex_waiting','futex_address','wake_at','syscall_locked','signal_pending','signal_mask','kill_signal']}
    row['name']=task['name'].string();root=int(task['space']['root'])
    if int(task['user']) and row['pid']==row['tgid']:
        try:
            count=gdb.lookup_type('struct task_context')['fds'].type.range()[1]+1
            data=read(int(task['fds']),count*8)
            row['open_fds']=[index for index,value in enumerate(struct.unpack('<'+'Q'*count,data)) if value]
            row['descriptors']=[]
            for fd in row['open_fds']:
                file=task['fds'][fd].dereference()
                item={'fd':fd,'file':hex(int(task['fds'][fd])),'path':file['path'].string(),
                      'flags':int(file['flags']),'references':int(file['references'])}
                try:
                    if item['path'].startswith('/home/port-test/runtime/coast-rpc-') and item['path'].endswith('.jsonl'):
                        node=gdb.Value(int(file['node']['id'])).cast(gdb.lookup_type('struct ram_node').pointer())
                        size=int(node['size']);assert size<=8*1024*1024
                        if int(node['boot_data']):body=read(int(node['boot_data']),size)
                        else:
                            body=b''.join(read(hhdm+int(node['pages'][i]),min(4096,size-i*4096)) for i in range((size+4095)//4096))
                        item['diagnostic_trace']=body.decode('utf8')
                    if int(file['event']):item['event_value']=int(file['event']['value'])
                    if int(file['pipe']):item['pipe']={key:int(file['pipe'][key]) for key in ['count','capacity','readers','writers']}
                    if int(file['epoll']):
                        item['watches']=[{'fd':int(w['fd']),'file':hex(int(w['file'])),
                            'events':int(w['event']['events']),'disabled':bool(w['disabled'])}
                            for w in array(file['epoll']['watches']) if int(w['file'])]
                    if int(file['unix_socket']):
                        sock=file['unix_socket'];pair=sock['pair']
                        item['unix']={'pair':hex(int(pair)),'side':int(sock['side']),
                            'queues':[{key:int(q[key]) for key in ['bytes','count']} for q in array(pair['queues'])],
                            **{key:[bool(v) for v in array(pair[key])] for key in ['open','read_shutdown','write_shutdown']}}
                        private=private_streams.get(int(pair))
                        if private is not None:
                            item['unix']['private']={key:int(private[key]) for key in ['state','family','pending_count','backlog']}
                            item['unix']['private']['names']=[name.string() for name in array(private['names'])]
                except Exception as error:item['detail_error']=str(error)
                row['descriptors'].append(item)
            for i in range(int(task['environment_count'])):
                value=task['environment'][i].string()
                if value.startswith('VSCODE_ESM_ENTRYPOINT='):row['entrypoint']=value.split('=',1)[1]
        except Exception as error:row['fd_error']=str(error)
        if ipcz_layout and (row['name'].startswith('/opt/electron-') or row['name']=='/proc/self/exe'):
            try:row['ipcz']=ipcz_state(root)
            except Exception as error:row['ipcz_error']=str(error)
        if uv_layout and (row['name'].startswith('/opt/electron-') or row['name']=='/proc/self/exe'):
            try:row['libuv']=uv_state(root)
            except Exception as error:row['libuv_error']=str(error)
    if int(task['user']) and root and int(task['frame']):
        for name,pointer in [('scheduled',task['frame']),('syscall',(int(task['stack_top'])-gdb.lookup_type('register_t').sizeof))]:
            try:
                frame=gdb.Value(int(pointer)).cast(frame_type).dereference()
                row[name]={field:hex(int(frame[field])) for field in ['rip','rbp','user_rsp','cs','rax','rdi','rsi','rdx','r10','r8','r9']}
                if int(frame['cs'])&3:row[name]['user_stack']=unwind(root,frame)
                if name=='syscall' and row['pid']==row['tgid'] and int(frame['cs'])&3:
                    # glibc may omit RBP. Preserve possible return addresses
                    # from the saved user stack separately from a real unwind.
                    try:
                        start=int(frame['user_rsp']);data=user_bytes(root,start,3072)
                        row[name]['stack_code_candidates']=[{'offset':i*8,'pc':hex(value)}
                            for i,value in enumerate(struct.unpack('<'+'Q'*(len(data)//8),data))
                            if 0x400000000<=value<0x418000000]
                    except Exception as error:row[name]['stack_scan_error']=str(error)
            except Exception as error:row[name]={'error':str(error)}
    rows.append(row)
assert rows,'No live guest tasks were read'
with open(os.environ['COAST_TASK_DUMP'],'w') as stream:json.dump(rows,stream,indent=2)
print('Read-only owned guest task snapshot:',len(rows),'tasks')

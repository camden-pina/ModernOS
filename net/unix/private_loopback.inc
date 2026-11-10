/* The admitted app tree's loopback-only IPv4 stream namespace. These local
 * endpoints reuse the SAME32 charged private-stream slots, bounded queues,
 * backlog4 and refcount/half-close implementation. No packet, NIC, routing or
 * global lwIP socket is created. Root and other app budgets see no listener.
 * Callers hold syscall_lock and vfs_mutex, as for pathname private streams. */
struct private_inet_address { uint16_t family,port;uint32_t ip;uint8_t padding[8]; };
_Static_assert(sizeof(struct private_inet_address)==16,"IPv4 sockaddr ABI");
static bool private_port_busy(const struct private_stream *self,unsigned port) {
    for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++) {
        const struct private_stream *s=&private_streams[i];
        if(s!=self && s->pair && s->family==2 && s->budget==self->budget && s->state && s->ports[0]==port)return true;
    }
    return false;
}
static unsigned private_auto_port(const struct private_stream *s) {
    /* At most31 other slots can hold a port;32 candidates suffice. */
    for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++)if(!private_port_busy(s,49152u+i))return 49152u+i;
    return 0;
}
static int private_inet_address(struct task *t,uint64_t pointer,uint64_t length,unsigned *port) {
    if(length<sizeof(struct private_inet_address) || length>128)return -E_INVAL;
    struct private_inet_address a;
    if(!user_copy_in(&t->space,&a,pointer,sizeof(a)))return -E_FAULT;
    if(a.family!=2)return -E_AFNOSUPPORT;
    if(a.ip!=0x0100007fu)return -E_ACCES; /* Exactly127.0.0.1; no ANY/WAN. */
    *port=(uint16_t)((a.port>>8)|(a.port<<8));return 0;
}
static long private_inet_name(struct task *t,register_t *r,unsigned port) {
    uint32_t length;if(!user_copy_in(&t->space,&length,r->rdx,4))return -E_FAULT;
    struct private_inet_address a={.family=2,.port=(uint16_t)((port>>8)|(port<<8)),.ip=0x0100007fu};
    uint32_t actual=sizeof(a),copy=min(length,actual);
    if(!user_access(&t->space,r->rsi,copy,true) || !user_access(&t->space,r->rdx,4,true))return -E_FAULT;
    return user_copy_out(&t->space,r->rsi,&a,copy) && user_copy_out(&t->space,r->rdx,&actual,4)?0:-E_FAULT;
}
static long private_inet_bind(struct task *t,register_t *r,struct private_stream *s) {
    if(s->state)return -E_INVAL;
    unsigned port;int error=private_inet_address(t,r->rsi,r->rdx,&port);if(error)return error;
    if(!port)port=private_auto_port(s);
    if(!port || private_port_busy(s,port))return -E_ADDRINUSE;
    s->ports[0]=port;s->state=1;return 0;
}
static long private_inet_connect(struct task *t,register_t *r,struct private_stream *s) {
    if(s->state>=2)return s->state==3?-E_ISCONN:-E_INVAL;
    unsigned port;int error=private_inet_address(t,r->rsi,r->rdx,&port);if(error)return error;
    if(!port)return -E_CONNREFUSED;
    struct private_stream *listener=NULL;
    for(unsigned i=0;i<PRIVATE_STREAM_LIMIT;i++) {
        struct private_stream *candidate=&private_streams[i];
        if(candidate->pair && candidate->family==2 && candidate->budget==s->budget &&
           candidate->state==2 && candidate->ports[0]==port){listener=candidate;break;}
    }
    if(!listener)return -E_CONNREFUSED;
    if(listener->pending_count>=listener->backlog)return -E_AGAIN;
    unsigned local=s->ports[0]?s->ports[0]:private_auto_port(s);
    if(!local)return -E_ADDRINUSE;
    struct unix_pair *p=s->pair;struct file *accepted=NULL;
    error=vfs_unix_socket(&p->sockets[1],0,&accepted);if(error)return error;
    p->open[1]=true;s->state=3;s->ports[0]=local;s->ports[1]=port;
    s->peers[0]=listener->peers[0];s->peers[1]=(struct unix_peer_credentials){(int32_t)t->tgid,1000,1000};
    s->options[1]=listener->options[0];
    memcpy(s->keepalive[1],listener->keepalive[0],sizeof(s->keepalive[1]));
    s->tos[1]=listener->tos[0];
    listener->pending[listener->pending_count++]=accepted;return 0;
}
static long private_inet_option(struct task *t,register_t *r,struct unix_socket *socket,struct private_stream *s) {
    unsigned bit=r->rsi==1 && r->rdx==2?1u:r->rsi==6 && r->rdx==1?2u:r->rsi==1 && r->rdx==9?4u:0u;
    unsigned parameter=r->rsi==6 && r->rdx>=4 && r->rdx<=6?r->rdx-4:3;
    bool tos=r->rsi==0 && r->rdx==1;
    if(r->rax==SYS_setsockopt) {
        if(!bit && parameter==3 && !tos)return -E_NOPROTOOPT;
        if(r->r8<4 || r->r8>INT32_MAX)return -E_INVAL;
        int value;if(!user_copy_in(&t->space,&value,r->r10,4))return -E_FAULT;
        if(bit && value!=0 && value!=1)return -E_INVAL;
        if(parameter<3 && (value<1 || value>(parameter==2?127:32767)))return -E_INVAL;
        if(tos && (value<0 || value>255))return -E_INVAL;
        /* Local queues deliver immediately, have no TIME_WAIT and know peer
           lifetime directly. Keepalive values are retained without network
           probes/timers; they never enlarge queues or alter owner checks. */
        mutex_lock(&vfs_mutex);
        if(tos)s->tos[socket->side]=value;
        else if(parameter<3)s->keepalive[socket->side][parameter]=value;
        else if(value)s->options[socket->side]|=bit;else s->options[socket->side]&=~bit;
        mutex_unlock(&vfs_mutex);return 0;
    }
    int value;
    mutex_lock(&vfs_mutex);
    if(tos)value=s->tos[socket->side];
    else if(parameter<3) {
        static const uint16_t defaults[3]={7200,75,9};
        value=s->keepalive[socket->side][parameter]?s->keepalive[socket->side][parameter]:defaults[parameter];
    } else if(bit)value=!!(s->options[socket->side]&bit);
    else if(r->rsi!=1){mutex_unlock(&vfs_mutex);return -E_NOPROTOOPT;}
    else if(r->rdx==3)value=1;
    else if(r->rdx==4)value=0;
    else if(r->rdx==7 || r->rdx==8)value=BYTE_LIMIT;
    else if(r->rdx==30)value=s->state==2;
    else if(r->rdx==38)value=6;
    else if(r->rdx==39)value=2;
    else {mutex_unlock(&vfs_mutex);return -E_NOPROTOOPT;}
    mutex_unlock(&vfs_mutex);
    uint32_t length,actual=4;
    if(!user_copy_in(&t->space,&length,r->r8,4))return -E_FAULT;
    if(length>INT32_MAX)return -E_INVAL;
    if(!user_access(&t->space,r->r10,min(length,actual),true) || !user_access(&t->space,r->r8,4,true))return -E_FAULT;
    return user_copy_out(&t->space,r->r10,&value,min(length,actual)) && user_copy_out(&t->space,r->r8,&actual,4)?0:-E_FAULT;
}

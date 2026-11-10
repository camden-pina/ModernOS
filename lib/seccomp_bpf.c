#include <seccomp.h>
#include <seccomp_bpf.h>
#include <string.h>

bool seccomp_bpf_action(uint32_t v) {
    switch(v&0xffff0000u) {
    case SC_ALLOW:case SC_ERRNO:case SC_TRAP:case SC_KILL_THREAD:case SC_KILL_PROCESS:return true;
    default:return false;
    }
}
unsigned seccomp_bpf_rank(uint32_t v) {
    switch(v&0xffff0000u) {
    case SC_KILL_PROCESS:return 0;case SC_KILL_THREAD:return 1;
    case SC_TRAP:return 2;case SC_ERRNO:return 3;case SC_ALLOW:return 4;
    default:return 0;
    }
}
static void seccomp_bpf_merge(uint32_t *states,unsigned to,uint32_t state) {
    if(states[to]==UINT32_MAX)states[to]=state;else states[to]&=state;
}
/* All edges go forward. One ascending pass computes the intersection of
 * initialized scratch words on every path, with fixed 16KiB scratch. */
bool seccomp_bpf_verify(const struct seccomp_insn *p,unsigned n,uint32_t *states,bool validate_actions) {
    for(unsigned i=0;i<n;i++)states[i]=UINT32_MAX;
    states[0]=0;
    for(unsigned i=0;i<n;i++) {
        struct seccomp_insn q=p[i];unsigned c=q.code,cl=c&7;uint32_t s=states[i];
        bool reachable=s!=UINT32_MAX,fall=true;
        if(c>0xff)return false;
        switch(cl) {
        case 0:case 1:
            if(c!=cl && c!=(0x60|cl) && c!=(0x80|cl) && !(cl==0 && c==0x20))return false;
            if(c==0x20 && ((q.k&3) || q.k>sizeof(struct seccomp_data)-4))return false;
            if(c==(0x60|cl) && (q.k>=16 || (reachable && !(s&(1u<<q.k)))))return false;
            break;
        case 2:case 3:
            if(c!=cl || q.k>=16)return false;
            if(reachable)s|=1u<<q.k;
            break;
        case 4: {
            unsigned op=c&0xf0;
            if(op>0xa0 || (op==0x80 && c!=0x84))return false;
            if(!(c&8) && ((op==0x30 || op==0x90) && !q.k))return false;
            if(!(c&8) && (op==0x60 || op==0x70) && q.k>=32)return false;
            break;
        }
        case 5: {
            unsigned op=c&0xf0;
            if(c==5) {
                if(q.jt || q.jf || q.k>=n-i-1)return false;
                if(reachable)seccomp_bpf_merge(states,i+1+q.k,s);
            } else {
                if(op!=0x10 && op!=0x20 && op!=0x30 && op!=0x40)return false;
                if(q.jt>=n-i-1 || q.jf>=n-i-1)return false;
                if(reachable){seccomp_bpf_merge(states,i+1+q.jt,s);seccomp_bpf_merge(states,i+1+q.jf,s);}
            }
            fall=false;break;
        }
        case 6:
            if(c!=6 && c!=0x16)return false;
            if(c==6 && validate_actions && !seccomp_bpf_action(q.k))return false;
            fall=false;break;
        case 7:
            if(c!=7 && c!=0x87)return false;
            break;
        }
        if(fall) {
            if(i+1==n)return false;
            if(reachable)seccomp_bpf_merge(states,i+1,s);
        }
    }
    return true;
}
uint32_t seccomp_bpf_run(const struct seccomp_insn *p,unsigned n,const struct seccomp_data *d) {
    uint32_t a=0,x=0,m[16]={0};
    for(unsigned i=0;i<n;i++) {
        struct seccomp_insn q=p[i];unsigned c=q.code;uint32_t v=(c&8)?x:q.k;
        switch(c&7) {
        case 0:
            if(c==0)a=q.k;
            else if(c==0x20)memcpy(&a,(const uint8_t *)d+q.k,4);
            else if(c==0x60)a=m[q.k];else a=sizeof(*d);
            break;
        case 1:x=c==1?q.k:c==0x61?m[q.k]:sizeof(*d);break;
        case 2:m[q.k]=a;break;case 3:m[q.k]=x;break;
        case 4:
            switch(c&0xf0) {
            case 0:a+=v;break;case 0x10:a-=v;break;case 0x20:a*=v;break;
            case 0x30:if(!v)return SC_KILL_THREAD;a/=v;break;
            case 0x40:a|=v;break;case 0x50:a&=v;break;
            case 0x60:a<<=v&31;break;case 0x70:a>>=v&31;break;
            case 0x80:a=0u-a;break;case 0x90:if(!v)return SC_KILL_THREAD;a%=v;break;
            case 0xa0:a^=v;break;
            }
            break;
        case 5:
            if(c==5)i+=q.k;
            else {bool yes=(c&0xf0)==0x10?a==v:(c&0xf0)==0x20?a>v:(c&0xf0)==0x30?a>=v:(a&v)!=0;i+=yes?q.jt:q.jf;}
            break;
        case 6:return c==6?q.k:a;
        case 7:if(c==7)x=a;else a=x;break;
        }
    }
    return SC_KILL_PROCESS;
}

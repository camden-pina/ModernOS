#!/usr/bin/env python3
"""Compare the production password KDF with hashlib's PBKDF2 implementation."""
import hashlib,json,re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'kernel/sched/accounts.c').read_text()
erase=source[source.index('static void erase('):source.index('static bool valid_uid(')]
derive=source[source.index('static void derive('):source.index('static int authenticate(')]
rounds=int(re.search(r'#define ROUNDS (\d+)u',source)[1])
passwords=['','Coast guest password','a'*100,'spaces and ! symbols:123']
salt=bytes(range(16))
code='#include <crypto/sha256.h>\n#include <stdio.h>\n#include <string.h>\n#define ROUNDS '+str(rounds)+'u\n'+erase+derive
code+='\nint main(void){unsigned char salt[16],d[32];for(unsigned i=0;i<16;i++)salt[i]=i;\n'
for pw in passwords:
 code+='derive('+json.dumps(pw)+',salt,d);for(unsigned i=0;i<32;i++)printf("%02x",d[i]);puts("");\n'
code+='return 0;}\n'
with tempfile.TemporaryDirectory(prefix='coast-account-kdf-') as temp:
 p=Path(temp);(p/'check.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-idirafter',str(root/'include'),str(p/'check.c'),str(root/'lib/sha256.c'),'-o',str(p/'check')],check=True)
 actual=subprocess.check_output([str(p/'check')],text=True).splitlines()
 expected=[hashlib.pbkdf2_hmac('sha256',p.encode(),salt,rounds).hex() for p in passwords]
 assert actual==expected,(actual,expected)
 print('PASS production PBKDF2-HMAC-SHA256: empty, ordinary, long HMAC key, spaces/symbols; '+str(rounds)+' iterations')

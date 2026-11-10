// Run only inside the actual native JSC host, using its checked file service.
(function(){
 const s=codeOSSService,h=Coast,root=h.args[0];let checks=0;
 function ok(v,m){if(!v)throw Error('SHARED_CHECK '+m);checks++;}
 function q(id,action,text='',a=0,b=0,flags=0,d=0){return s.dispatch({c:id,action,text,a,b,flags,d});}
 function reject(fn,m){let failed=false;try{fn();}catch(_){failed=true;}ok(failed,m);}
 for(let i=1;i<=8;i++){q(i,0);q(i,1,'document-'+i+' café 😀\r\n');}
 for(let i=1;i<=8;i++)ok(q(i,10).content==='document-'+i+' café 😀\r\n','separate model '+i);
 q(2,4);ok(q(2,10).content==='','independent undo');ok(q(3,10).content.startsWith('document-3'),'other history intact');
 reject(()=>q(3,0),'duplicate registration refused');ok(q(3,10).content.startsWith('document-3'),'duplicate cannot clear dirty text');
 reject(()=>q(99,1,'wrong document'),'unknown ID rejected');reject(()=>q(0,0),'zero ID rejected');reject(()=>q(-1,0),'negative ID rejected');reject(()=>q(2147483648,0),'overflow ID rejected');
 q(2,15);reject(()=>q(2,1,'late edit'),'released ID rejected');q(2,0);ok(q(2,10).content==='','new registration has no former text/history');
 for(let i=1;i<=8;i++)q(i,15);ok(s.size===0,'all documents released');
 for(let i=1;i<=32;i++)q(i,0);reject(()=>q(33,0),'bounded document registry');ok(s.size===32,'failed registration leaves registry intact');for(let i=1;i<=32;i++)q(i,15);
 const path=root+'/shared.txt',original='\uFEFFfirst 😀\r\n';h.saveText(path,original,null);q(101,0,path);q(102,0,path);q(101,1,'one ');let r=q(101,6);ok(!r.error&&h.readText(path)==='\uFEFFone first 😀\r\n','first shared-file model save');q(102,1,'two ');r=q(102,6);ok(r.error===1&&(r.flags&1)&&h.readText(path)==='\uFEFFone first 😀\r\n','second stale model conflicts');r=q(102,7,root+'/shared-copy.txt');ok(!r.error&&h.readText(root+'/shared-copy.txt')==='\uFEFFtwo first 😀\r\n','conflicting model Save As exact');
 q(101,15);q(102,15);q(200,0);q(200,1,'unsaved only');q(200,15);ok(s.size===0,'release drops models and bounded history');ok(h.readText(path)==='\uFEFFone first 😀\r\n','release never writes other model data');
 print('CODEOSS_SHARED_CHECKS='+checks+' genuine separate models/history, ID bounds/release and concurrent checked-file conflicts');
})();

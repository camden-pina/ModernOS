// Executes only in the actual native JSC worker, using its genuine file API.
(function(){
 const E=codeOSSGraphical.constructor,h=Coast,root=h.args[0];if(!root||root[0]!=='/')throw Error('Pass an empty test directory');
 let checks=0;function ok(v,m){if(!v)throw Error('GRAPHICAL_CHECK '+m);checks++;}
 function q(e,action,text='',a=0,b=0,flags=0,d=0){return e.dispatch({action,text,a,b,c:0,d,flags});}
 let e=new E(),r=q(e,0);ok(!r.error&&!r.content&&r.lines===1,'new upstream document');
 r=q(e,1,'first café 😀\r\nsecond\nthird\rfourth');ok(!r.error&&r.lines===4,'real upstream mixed EOL count');let expected=r.content;
 r=q(e,4);ok(!r.content&&(r.flags&8),'undo');r=q(e,5);ok(r.content===expected,'redo preserves scalar/EOL bytes');
 r=q(e,8,'',12,12);ok(r.error===1&&r.content===expected,'surrogate split rejected');
 r=q(e,8,'',14,14);ok(r.error===1,'CRLF split rejected');
 r=q(e,8,'',0,0);q(e,2,'',3,0,2);r=q(e,2,'',0);ok(r.cursor===expected.length-1,'scalar left from document end');
 q(e,8,'',0,0);r=q(e,2,'',5);ok(r.line===1&&r.column===0,'vertical movement uses upstream line mapping');
 q(e,11,'café');r=q(e,10);ok(!r.error&&r.selection==='café','find/select/copy UTF-8');
 const rev=r.revision;q(e,1,'X');r=q(e,8,'',0,0,0,rev);ok(r.error===1,'stale pointer selection rejected');
 r=q(e,0);q(e,1,'  one');r=q(e,1,'',0,0,4);ok(r.content==='  one\n  ','newline and indentation');
 r=q(e,1,'',0,0,8);ok(r.content==='  one\n    ','tab spaces to next stop');q(e,12,'',8,0);r=q(e,1,'',0,0,8);ok(r.content.endsWith('\t'),'literal tab mode');
 r=q(e,13,'cached 😀\r\n',2,0,16);ok(!r.error&&(r.flags&16)&&!(r.flags&2)&&(r.flags&1),'new unsaved recovery with BOM');
 r=q(e,7,root+'/recovered.txt');ok(!r.error&&h.readText(root+'/recovered.txt')==='\uFEFFcached 😀\r\n','BOM recovery Save As exact');
 const original='\uFEFFalpha 😀\r\nbeta\n';h.saveText(root+'/original.txt',original,null);
 e=new E();r=q(e,0,root+'/original.txt');ok(!r.error&&(r.flags&16)&&r.content===original.slice(1),'real file open/BOM hidden');
 q(e,8,'',0,0);q(e,1,'edited ');r=q(e,6);ok(!r.error&&h.readText(root+'/original.txt')==='\uFEFFedited alpha 😀\r\nbeta\n','checked save exact mixed bytes');ok(r.message.includes('retained'),'original retained notice');
 const opened=h.readText(root+'/original.txt');q(e,1,'local ');h.saveText(root+'/original.txt','external\n',opened);const before=e.doc.content;r=q(e,6);ok(r.error===1&&r.content===before&&(r.flags&1)&&h.readText(root+'/original.txt')==='external\n','conflict retains model and disk');
 r=q(e,7,root+'/recovered.txt');ok(r.error===1&&r.content===before,'Save As no-clobber');r=q(e,7,root+'/copy.txt');ok(!r.error&&h.readText(root+'/copy.txt')===e.doc.snapshot(),'Save As recovers edits');
 e=new E();for(let i=0;i<100;i++)q(e,1,'x');ok(e.history.length===64,'bounded history');for(let i=0;i<64;i++)q(e,4);r=q(e,10);ok(e.doc.content.length===36,'history eviction correct');q(e,1,'new');ok(e.historyAt===e.history.length,'redo discarded on edit');
 e=new E();q(e,1,'x'.repeat(256*1024));r=q(e,1,'y');ok(r.error===1&&r.content.length===256*1024,'preview byte limit rejects edit intact');q(e,9);r=q(e,10);ok(r.error===1,'clipboard byte limit');
 r=q(e,0,root+'/missing.txt');ok(r.error===1&&r.content.length===256*1024,'failed open preserves current model');
 print('CODEOSS_GRAPHICAL_CHECKS='+checks+' genuine upstream model/edit/history/Unicode/EOL/cursor/copy/recovery/limits and checked native file/conflict saves');
})();

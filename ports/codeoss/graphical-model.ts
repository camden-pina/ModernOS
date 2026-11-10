/* Original Coast graphical adapter. Actual text storage/edits are upstream
   PieceTreeBase via NativeDocument. There is no Node/DOM/workbench emulation. */
import { host, NativeDocument } from './native-model';
const LIMIT=256*1024,CLIP=65536,HISTORY=512*1024;
interface Request { action:number;flags:number;a:number;b:number;c:number;d:number;text:string; }
interface Edit {at:number;removed:string;inserted:string;cursor:number;anchor:number;after:number;}
function size(s:string):number {let n=0;for(const ch of s){const c=ch.codePointAt(0)!;n+=c<128?1:c<2048?2:c<65536?3:4;}return n;}
function valid(s:string):void {if(s.includes('\0') || /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(s))throw Error('Only valid Unicode text without NUL is supported.');}
function bounded(n:number,max:number):number {if(!Number.isSafeInteger(n)||n<0||n>max)throw RangeError('Position is outside the document.');return n;}
function prev(s:string,n:number):number {if(!n)return 0;if(n>=2&&s.slice(n-2,n)==='\r\n')return n-2;n--;if(n&&/[\uDC00-\uDFFF]/.test(s[n]))n--;return n;}
function next(s:string,n:number):number {if(n===s.length)return n;if(s.slice(n,n+2)==='\r\n')return n+2;return n+(s.codePointAt(n)!>65535?2:1);}
export class GraphicalEditor {
 doc=NativeDocument.create('/home/user/Untitled.txt');cursor=0;anchor=0;revision=1;tab=4;spaces=true;preferred=-1;
 history:Edit[]=[];historyAt=0;historyUnits=0;message='';selection='';
 private boundary(n:number):void {const s=this.doc.content;bounded(n,s.length);if(n&&s[n-1]==='\r'&&s[n]==='\n')throw RangeError('Position splits a CRLF line ending.');if(n&&n<s.length&&/[\uD800-\uDBFF]/.test(s[n-1])&&/[\uDC00-\uDFFF]/.test(s[n]))throw RangeError('Position splits a Unicode scalar.');}
 private reset(doc:NativeDocument):void {if(size(doc.snapshot())>LIMIT)throw Error('Code OSS Core Preview supports files up to 256 KiB. The file is unchanged.');this.doc=doc;this.cursor=this.anchor=0;this.history=[];this.historyAt=this.historyUnits=0;this.revision++;this.preferred=-1;}
 private edit(value:string):void {
  valid(value);const text=this.doc.content,a=Math.min(this.cursor,this.anchor),b=Math.max(this.cursor,this.anchor);this.boundary(a);this.boundary(b);
  const result=text.slice(0,a)+value+text.slice(b);if(size(result)+(this.doc.hasBOM?3:0)>LIMIT)throw Error('The 256 KiB graphical preview limit would be exceeded. Existing text is preserved.');
  if(a===b&&!value)return;
  const record:Edit={at:a,removed:text.slice(a,b),inserted:value,cursor:this.cursor,anchor:this.anchor,after:a+value.length};
  while(this.history.length>this.historyAt){const r=this.history.pop()!;this.historyUnits-=r.removed.length+r.inserted.length;}
  const units=record.removed.length+record.inserted.length;
  while(this.history.length&&(this.history.length>=64||this.historyUnits+units>HISTORY)){const r=this.history.shift()!;this.historyUnits-=r.removed.length+r.inserted.length;this.historyAt--;}
  // Inputs are fully validated before either genuine PieceTree mutation.
  if(b>a)this.doc.delete(a,b-a);if(value)this.doc.insert(a,value);
  this.history.push(record);this.historyUnits+=units;this.historyAt=this.history.length;this.cursor=this.anchor=record.after;this.revision++;this.preferred=-1;
 }
 private undo(redo:boolean):void {
  if(redo?this.historyAt===this.history.length:!this.historyAt)return;
  const r=this.history[redo?this.historyAt:this.historyAt-1],remove=redo?r.removed:r.inserted,insert=redo?r.inserted:r.removed;
  if(remove)this.doc.delete(r.at,remove.length);if(insert)this.doc.insert(r.at,insert);
  if(redo){this.historyAt++;this.cursor=this.anchor=r.after;}else{this.historyAt--;this.cursor=r.cursor;this.anchor=r.anchor;}
  this.revision++;this.preferred=-1;
 }
 private visual(line:string,at:number):number {let col=0;for(let i=0;i<at;){const n=next(line,i);if(line[i]==='\t')col+=this.tab-col%this.tab;else if(line[i]!=='\r')col++;i=n;}return col;}
 private column(line:string,wanted:number):number {let col=0,i=0;while(i<line.length){const n=next(line,i),width=line[i]==='\t'?this.tab-col%this.tab:line[i]==='\r'?0:1;if(width&&wanted*2<(col*2+width))break;col+=width;i=n;}return i;}
 private move(r:Request):void {
  const text=this.doc.content,p=this.doc.tree.getPositionAt(this.cursor);let n=this.cursor;
  if(r.a===0||r.a===1){n=r.a===0?prev(text,n):next(text,n);if(r.flags&2){while(n>0&&n<text.length&&/\s/.test(text[r.a===0?n:prev(text,n)]))n=r.a===0?prev(text,n):next(text,n);while(n>0&&n<text.length&&!/\s/.test(text[r.a===0?prev(text,n):n]))n=r.a===0?prev(text,n):next(text,n);}}
  else if(r.a===2)n=r.flags&2?0:this.doc.tree.getOffsetAt(p.lineNumber,1);
  else if(r.a===3)n=r.flags&2?text.length:this.doc.tree.getOffsetAt(p.lineNumber,this.doc.tree.getLineContent(p.lineNumber).length+1);
  else if(r.a>=4&&r.a<=7){const down=r.a===5||r.a===7,amount=r.a>=6?Math.max(1,Math.min(100,r.b)):1;if(this.preferred<0)this.preferred=this.visual(this.doc.tree.getLineContent(p.lineNumber),p.column-1);const line=Math.max(1,Math.min(this.doc.tree.getLineCount(),p.lineNumber+(down?amount:-amount)));n=this.doc.tree.getOffsetAt(line,1)+this.column(this.doc.tree.getLineContent(line),this.preferred);}
  else throw Error('Unsupported cursor movement.');
  if(r.a<4)this.preferred=-1;this.boundary(n);this.cursor=n;if(!(r.flags&1))this.anchor=n;
 }
 dispatch(r:Request):any {
  this.message='';this.selection='';let error=0;
  try {
   if(r.action===0){this.reset(r.text?NativeDocument.open(r.text):NativeDocument.create('/home/user/Untitled.txt'));}
   else if(r.action===1){let value=r.text;if(r.flags&4){const a=Math.min(this.cursor,this.anchor),p=this.doc.tree.getPositionAt(a),prefix=this.doc.tree.getLineContent(p.lineNumber).slice(0,p.column-1).match(/^[ \t]*/)![0];value=this.doc.tree.getEOL()+prefix;}else if(r.flags&8){const p=this.doc.tree.getPositionAt(this.cursor),col=this.visual(this.doc.tree.getLineContent(p.lineNumber),p.column-1);value=this.spaces?' '.repeat(this.tab-col%this.tab):'\t';}this.edit(value);}
   else if(r.action===2)this.move(r);
   else if(r.action===3){if(r.d&&r.d!==this.revision)throw Error('The document changed before selection. Select it again.');if(this.cursor===this.anchor){const text=this.doc.content;let n=r.a?next(text,this.cursor):prev(text,this.cursor);if(r.flags&2){while(n>0&&n<text.length&&/\s/.test(text[r.a?prev(text,n):n]))n=r.a?next(text,n):prev(text,n);while(n>0&&n<text.length&&!/\s/.test(text[r.a?n:prev(text,n)]))n=r.a?next(text,n):prev(text,n);}this.anchor=n;}this.edit('');}
   else if(r.action===4||r.action===5)this.undo(r.action===5);
   else if(r.action===6||r.action===7){const result=r.action===6?this.doc.save():this.doc.saveAs(r.text);this.message=result.backupPath?'Saved. Previous version retained: '+result.backupPath:'Saved.';}
   else if(r.action===8){if(r.d&&r.d!==this.revision)throw Error('The document changed before the click. Click again.');this.boundary(r.a);this.boundary(r.b);this.cursor=r.a;this.anchor=r.b;this.preferred=-1;}
   else if(r.action===9){this.anchor=0;this.cursor=this.doc.content.length;this.preferred=-1;}
   else if(r.action===10){const selected=this.doc.content.slice(Math.min(this.cursor,this.anchor),Math.max(this.cursor,this.anchor));if(size(selected)>CLIP)throw Error('Clipboard limit is 64 KiB. Select a smaller range.');this.selection=selected;}
   else if(r.action===11){valid(r.text);if(r.text){const text=this.doc.content;let at=text.indexOf(r.text,this.cursor);if(at<0)at=text.indexOf(r.text);if(at<0)this.message='No match. Search is literal and case-sensitive.';else{this.boundary(at);this.boundary(at+r.text.length);this.anchor=at;this.cursor=at+r.text.length;this.preferred=-1;}}}
   else if(r.action===12){if(![2,4,8].includes(r.a))throw Error('Tab width must be 2, 4 or 8.');this.tab=r.a;this.spaces=!!r.b;}
   else if(r.action===13){valid(r.text);this.reset(NativeDocument.createWithText('/home/user/Recovered.txt',(r.flags&16?'\uFEFF':'')+r.text));this.cursor=this.anchor=Math.min(r.a,this.doc.content.length);this.boundary(this.cursor);this.message='Recovered the last acknowledged text. Use Save As to create a new copy.';}
   else throw Error('Unsupported graphical action.');
  } catch(e){error=1;const v=e as Error&{code?:string};this.message=Array.from((v.code?v.code+': ':'')+(v.message||String(e))).slice(0,240).join('');}
  return this.snapshot(error);
 }
 snapshot(error=0):any {
  const p=this.doc.tree.getPositionAt(this.cursor),content=this.doc.content;
  return {error,path:this.doc.path,content,selection:this.selection,message:this.message,cursor:this.cursor,anchor:this.anchor,lines:this.doc.tree.getLineCount(),line:p.lineNumber-1,column:this.visual(this.doc.tree.getLineContent(p.lineNumber),p.column-1),revision:this.revision,eol:this.doc.tree.getEOL()==='\r\n'?2:1,tab:this.tab,flags:((this.doc.dirty||(!this.doc.exists&&this.doc.hasBOM))?1:0)|(this.doc.exists?2:0)|(this.historyAt?4:0)|(this.historyAt<this.history.length?8:0)|(this.doc.hasBOM?16:0)|(this.spaces?32:0)};
 }
}
(globalThis as any).codeOSSGraphical=new GraphicalEditor();

/* One JSC runtime, distinct genuine models and undo/file state per document.
   IDs are explicit in protocol v2; unknown, duplicate and overflow registrations
   fail closed instead of dispatching an edit into another document. */
export class GraphicalService {
 private documents=new Map<number,GraphicalEditor>();
 get size():number{return this.documents.size;}
 dispatch(r:Request):any {
  if(!Number.isSafeInteger(r.c)||r.c<1||r.c>0x7fffffff)throw Error('Invalid document identity.');
  let editor=this.documents.get(r.c);
  if(r.action===0||r.action===13){
   if(editor)throw Error('Duplicate document registration.');
   if(this.documents.size>=32)throw Error('The graphical service document limit is 32.');
   editor=new GraphicalEditor();this.documents.set(r.c,editor);
  }
  if(!editor)throw Error('Unknown document identity.');
  if(r.action===15){const result=editor.snapshot();this.documents.delete(r.c);return result;}
  return editor.dispatch(r);
 }
}
(globalThis as any).codeOSSService=new GraphicalService();

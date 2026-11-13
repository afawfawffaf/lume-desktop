const exact=(value,keys)=>value && typeof value==='object' && !Array.isArray(value) &&
  Object.keys(value).length===keys.length && keys.every(key=>Object.hasOwn(value,key));
const integer=(value,min,max)=>Number.isSafeInteger(value)&&value>=min&&value<=max;
const text=(value,max)=>typeof value==='string'&&value.length<=max&&!/[\u0000-\u001f\u007f]/.test(value);
const displayText=value=>typeof value==='string'&&value.length<=512&&new TextEncoder().encode(value).length<=512&&
  !/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(value);
const id=value=>text(value,96)&&/^local-\d+$/.test(value);
const artwork=value=>{
  if(value==='')return true;
  const match=typeof value==='string'&&value.match(/^http:\/\/127\.0\.0\.1:([1-9]\d{0,4})\/[a-f0-9]{64}\/media\.png$/);
  return !!match&&Number(match[1])<=65535;
};

export function validMusicLibraryCommand(action,fields) {
  if(action==='snapshot')return exact(fields,[]);
  if(action==='import-folder')return exact(fields,[]);
  if(action==='sync-lx')return exact(fields,[]);
  if(action==='page')return (exact(fields,['offset','limit'])||exact(fields,['offset','limit','query']))&&
    integer(fields.offset,0,4096)&&integer(fields.limit,1,100)&&
    (fields.query===undefined||text(fields.query,128));
  if(action==='visibility')return exact(fields,['visible'])&&typeof fields.visible==='boolean';
  return ['play','enqueue','insertNext','remove'].includes(action)&&exact(fields,['id'])&&id(fields.id);
}
const libraryKeys=['version','type','revision','loaded','error','total','offset','limit','tracks'];
// The optional sync block reports the LX playlist pull that runs beside the
// library: the page shows it as a notice, so it must never gate the list.
const syncProgress=value=>exact(value,['active','stage','done','total'])&&typeof value.active==='boolean'&&
  ['download','convert'].includes(value.stage)&&integer(value.done,0,1e6)&&integer(value.total,0,1e6);
export function validMusicLibraryMessage(message) {
  return (exact(message,libraryKeys)||(exact(message,[...libraryKeys,'sync'])&&syncProgress(message.sync))) &&
    message.version===1&&message.type==='music-library-state'&&integer(message.revision,0,Number.MAX_SAFE_INTEGER)&&
    typeof message.loaded==='boolean'&&text(message.error,128)&&integer(message.total,0,4096)&&
    integer(message.offset,0,4096)&&integer(message.limit,1,100)&&Array.isArray(message.tracks)&&
    message.tracks.length<=message.limit&&new Set(message.tracks.map(track=>track.id)).size===message.tracks.length&&
    message.tracks.every(track=>exact(track,['id','title','artist','album','source','artwork','duration','available','error'])&&
      id(track.id)&&['title','artist','album'].every(key=>displayText(track[key]))&&track.source===''&&artwork(track.artwork)&&
      Number.isFinite(track.duration)&&track.duration>=0&&track.duration<=1e9&&
      typeof track.available==='boolean'&&text(track.error,128));
}
export function createMusicLibrary({send,onChange,onError}={}) {
  let state={tracks:[],total:0,offset:0,limit:100,loaded:false,error:'',pending:false,sync:null};
  let revision=-1,visible=false,disposed=false;
  const emit=()=>onChange?.({...state,tracks:[...state.tracks]});
  function post(action,fields={}) {
    if(disposed||!validMusicLibraryCommand(action,fields))return false;
    if(action!=='visibility'&&action!=='sync-lx'){state={...state,pending:true,error:''};emit();}
    try {if(send?.(action,fields)===true)return true;}catch{}
    state={...state,pending:false,error:'unavailable'};emit();onError?.(state.error);return false;
  }
  return {
    get state(){return {...state,tracks:[...state.tracks]};},
    load:()=>post('snapshot'),
    importFolder:()=>post('import-folder'),
    syncLx:()=>post('sync-lx'),
    // The native owner matches the query against the whole library and returns
    // the page window from those matches, so a search is not limited to the
    // tracks that happen to sit on the page being displayed.
    page:(offset,query='')=>post('page',query?{offset,limit:100,query}:{offset,limit:100}),
    play:id=>post('play',{id}),
    enqueue:(id,next=false)=>post(next?'insertNext':'enqueue',{id}),
    remove:id=>post('remove',{id}),
    setVisible(value) {
      if(disposed||visible===(value===true))return;
      visible=value===true;
      post('visibility',{visible});
      if(!visible){state={...state,pending:false,tracks:state.tracks.map(track=>({...track,artwork:''}))};emit();}
    },
    releasePageData() {
      if(disposed)return;
      if(visible)this.setVisible(false);
      if(!state.loaded&&!state.tracks.length&&!state.total&&!state.error&&!state.pending)return;
      state={...state,tracks:[],total:0,offset:0,loaded:false,error:'',pending:false};
      emit();
    },
    receive(message) {
      if(disposed||!validMusicLibraryMessage(message)||message.revision<=revision)return false;
      revision=message.revision;
      const {tracks,total,offset,limit,loaded,error,sync}=message;
      state={tracks,total,offset,limit,loaded,error,pending:false,sync:sync||null};emit();
      if(error)onError?.(error);
      return true;
    },
    dispose(){if(disposed)return;this.setVisible(false);disposed=true;state={...state,tracks:[]};},
  };
}

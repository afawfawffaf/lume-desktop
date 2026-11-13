import {mountMusicView} from './music-view.js';
import {createMusicPreferences} from './music-preferences.js';
import {createMusicBackground,createMusicBackgroundCatalogQuery} from './music-background.js';
import {createMusicProviderAccount} from './music-provider-account.js';
import {createMusicOnline,musicOnlineError} from './music-online.js';
import {createMusicLibrary} from './music-library.js';
import {createMusicDesktop} from './music-desktop.js';
import {createMusicPage} from './music-page-settings.js';
import {createMusicFontQuery} from './music-font-query.js';
import {createMusicFontAssetQuery} from './music-font-asset-query.js';
import {createMusicFontAssets} from './music-font-assets.js';
import {musicCommand} from './music-runtime-state.js';

export function createMusicRuntime({
  bridge, content, showNotice, translate, getDefaultBackground, syncDesktopControls,
  initialDesktopState, initialPageState, viewState, onExitFullscreen, onImmersiveChange,
} = {}) {
  let musicViewState=viewState||null, musicViewOpen=false, musicView=null, musicNativeVisible=null;
  // The view validates commands against the newest host snapshot. Each new
  // queue bumps the session, so a mount-time snapshot would reject every
  // later toggle/seek/next as "unavailable" while playback continues.
  let latestMusicState=null;
  let musicLyricsRequestKey='';
  const queryMusicFonts=createMusicFontQuery(bridge);
  const queryMusicBackgrounds=createMusicBackgroundCatalogQuery(bridge);
  const musicFontAssets=createMusicFontAssets();
  const musicFontAssetQuery=createMusicFontAssetQuery(bridge,{onAssets:assets=>{
    musicFontAssets.setAssets(assets);
    musicView?.refreshFontLabels();
  }});
  const queryAvailableMusicFonts=async options=>{
    const [system,imported]=await Promise.allSettled([queryMusicFonts(options),musicFontAssetQuery.load()]);
    if(options?.signal?.aborted)throw new DOMException('Font query cancelled','AbortError');
    const fonts=[...musicFontAssets.listAssets(),...(system.status==='fulfilled'?system.value:[])];
    if(!fonts.length && (system.status==='rejected'||imported.status==='rejected'))
      throw system.status==='rejected'?system.reason:imported.reason;
    return fonts;
  };
  const ensureMusicFont=(document,family,options)=>musicFontAssetQuery.loaded?
    musicFontAssets.ensureFont(document,family,options):
    musicFontAssetQuery.load().then(()=>musicFontAssets.ensureFont(document,family,options));
  const uploadMusicFont=async({signal}={})=>{
    const candidate=await musicFontAssetQuery.choose({signal});
    if(!candidate)return null;
    try {
      await musicFontAssets.ensureFont(content?.ownerDocument || globalThis.document,candidate.family,{signal});
      return candidate.family;
    } catch(error) {
      void musicFontAssetQuery.drop(candidate.id).catch(()=>{});
      throw error;
    }
  };
  const musicProviderAccount=createMusicProviderAccount({
    send:(action,fields)=>bridge.musicProviderAccount(action,fields),
  });
  const musicOnline=createMusicOnline({
    send:(action,fields)=>bridge.musicOnline(action,fields),
    cache:true,
    onChange:()=>refreshMusicLibrary(),
    onError:error=>showNotice?.(musicOnlineError(error)),
  });
  const musicLibrary=createMusicLibrary({
    send:(action,fields)=>bridge.musicLibrary(action,fields),
    onChange:()=>{announceSync();refreshMusicLibrary();},
    onError:()=>showNotice?.('本地曲库操作失败，请检查文件是否可用后重试'),
  });
  // The sync runs for minutes behind a hidden child process, so its progress is
  // the only feedback the page gets: one notice line, replaced in place.
  let syncNotice='',syncWasActive=false;
  function announceSync() {
    const sync=musicLibrary.state.sync;
    if(sync&&sync.active){
      syncWasActive=true;
      const text=sync.stage==='convert'
        ? `正在整理音质 ${sync.done}/${sync.total}`
        : `正在同步 LX 歌单 ${sync.done}/${sync.total}`;
      if(text===syncNotice)return;
      syncNotice=text;
      showNotice?.(text,{top:true});
      return;
    }
    // The finishing state arrives several times; only the first one after a run
    // may speak, or the trailing snapshots would clear the message they follow.
    if(!syncWasActive)return;
    syncWasActive=false;
    syncNotice='';
    showNotice?.('LX 歌单同步完成',{autoDismiss:true,top:true});
  }
  const musicDesktop=createMusicDesktop({
    send:(action,fields)=>bridge.musicDesktop(action,fields),
    onChange:value=>{syncDesktopControls?.(value);musicView?.updateDesktopControls?.();},
    onError:error=>showNotice?.(error==='smtc'
      ? '系统媒体控制暂不可用，应用内播放不受影响' : '桌面音乐组件操作失败，请重试'),
  });
  if(initialDesktopState?.type==='music-desktop-state')musicDesktop.receive(initialDesktopState);
  const musicPage=createMusicPage({
    send:(action,fields)=>bridge.musicPage(action,fields),
    onChange:()=>musicView?.updateDesktopControls?.(),
    onError:()=>showNotice?.('桌面音乐卡片设置保存失败，请重试'),
  });
  if(initialPageState?.type==='music-page-state')musicPage.receive(initialPageState);
  const musicPreferences=createMusicPreferences({
    send:(action,fields)=>bridge.musicPreferences(action,fields),
    onChange:value=>musicView?.updatePreferences(value),
    onError:error=>showNotice?.(error==='load'
      ? translate?.('音乐设置读取失败','Could not load music settings')
      : translate?.('音乐设置保存失败，已恢复此前设置','Could not save music settings. Previous settings restored.')),
  });
  const musicBackground=createMusicBackground({
    send:(action,fields)=>bridge.musicBackground(action,fields),
    onChange:value=>musicView?.updateBackground(value),
    onError:error=>showNotice?.(error==='unsupported'
      ? translate?.('不支持此背景文件','Unsupported background file')
      : error==='too-large' ? translate?.('背景文件过大','Background file is too large')
      : error==='load' ? translate?.('音乐背景读取失败','Could not load music background')
      : error==='conflict' ? translate?.('音乐背景已更改，请重试','Music background changed. Please try again.')
      : translate?.('音乐背景保存失败，已保留此前背景','Could not save music background. Previous background retained.')),
  });
  musicProviderAccount.subscribe(value=>musicOnline.accounts(value));

  function musicLibraryView() {
    const local=musicLibrary.state;
    return {...musicOnline.snapshot(),localTracks:local.tracks,localTotal:local.total,
      localOffset:local.offset,localLimit:local.limit,
      localMore:local.offset+local.tracks.length<local.total,
      localError:local.error,localLoading:local.pending,localFolderImport:true,
      localSyncLx:true,localSyncing:!!(local.sync&&local.sync.active)};
  }
  function refreshMusicLibrary(){musicView?.updateLibrary(musicLibraryView());}
  function sendMusic(action,fields={},musicState) {
    const command=musicCommand(action,fields,musicState);
    if(!command)return false;
    const {version:_version,scope:_scope,action:wireAction,...payload}=command;
    return bridge.music(wireAction,payload);
  }
  function open(musicState) {
    if(musicViewOpen)return;
    musicViewOpen=true;
    sendMusic('open',{},musicState);
    sendMusic('load-home',{},musicState);
    musicPreferences.load();
    musicBackground.load();
    musicProviderAccount.load();
    musicOnline.setVisible(true);
    musicLibrary.setVisible(true);
    musicLibrary.load();
    if(musicDesktop.state.pending===false)musicDesktop.load();
    if(musicPage.state.pending===false)musicPage.load();
  }
  function close(musicState) {
    const captured=musicView?.captureView();
    if(captured)musicViewState=captured;
    musicView?.dispose();
    musicView=null;
    musicOnline.setVisible(false);
    musicLibrary.setVisible(false);
    musicLibrary.releasePageData();
    if(!musicViewOpen)return;
    sendMusic('close',{},musicState);
    musicViewOpen=false;
    musicLyricsRequestKey='';
    musicNativeVisible=null;
    return musicViewState;
  }
  function render(musicState) {
    latestMusicState=musicState;
    open(musicState);
    if(musicView)musicView.update(musicState,{syncClock:false});
    else musicView=mountMusicView(content,{
      snapshot:musicState,providerAccount:musicProviderAccount,library:musicLibraryView(),
      online:musicOnline,localLibrary:musicLibrary,desktopControls:musicDesktop,desktopCard:musicPage,
      onDesktopBack:()=>closeAndNavigate?.(),onExitFullscreen,onImmersiveChange,
      queryFonts:queryAvailableMusicFonts,getFontLabel:family=>musicFontAssets.labelFor(family),
      ensureFont:ensureMusicFont,onFontUpload:uploadMusicFont,queryBackgrounds:queryMusicBackgrounds,
      onBackgroundSelect:id=>musicBackground.select(id),viewState:musicViewState,
      send:(action,fields)=>sendMusic(action,fields,latestMusicState||musicState),
      onImport:files=>bridge.musicDropFiles(files),onNotice:text=>showNotice?.(text),
      preferences:musicPreferences.snapshot(),onPreferencePreview:patch=>musicPreferences.preview(patch),
      onPreferenceCommit:patch=>musicPreferences.commit(patch),background:musicBackground.snapshot(),
      defaultBackground:getDefaultBackground?.(),
      onBackgroundAction:action=>action==='choose'?musicBackground.choose():
        action==='clear'&&musicBackground.clear(),
    });
  }
  let closeAndNavigate=null;
  return {
    setCloseAndNavigate(callback){closeAndNavigate=callback;},
    render,
    close,
    setVisible(visible,musicState) {
      musicView?.setVisible(visible);
      musicOnline.setVisible(musicViewOpen&&visible);
      musicLibrary.setVisible(musicViewOpen&&visible);
      if(musicViewOpen&&musicNativeVisible!==visible) {
        musicNativeVisible=visible;
        if(!visible)musicLyricsRequestKey='';
        sendMusic('visibility',{visible},musicState);
      }
    },
    receive(message) {
      if(message.type==='music-preferences-state')return musicPreferences.receive(message);
      if(message.type==='music-background-state')return musicBackground.receive(message);
      if(message.type==='music-provider-account-state')return musicProviderAccount.receive(message);
      if(message.type==='music-online-state')return musicOnline.receive(message);
      if(message.type==='music-library-state')return musicLibrary.receive(message);
      if(message.type==='music-desktop-state')return musicDesktop.receive(message);
      if(message.type==='music-page-state')return musicPage.receive(message);
      if(['music-fonts-state','music-font-assets-state','music-background-catalog'].includes(message.type))return true;
      return false;
    },
    updateMusicState(next){latestMusicState=next;musicView?.update(next);},
    updateDefaultBackground(media){musicView?.updateDefaultBackground?.(media);},
    desktopState(){return musicDesktop.state;},
    setDesktopEnabled(value){return musicDesktop.setEnabled(value);},
    pageState(){return musicPage.state;},
    setPageSize(width,height){return musicPage.setSize(width,height);},
    resetPage(){return musicPage.reset();},
    dispose() {
      close();
      musicProviderAccount.dispose();
      musicOnline.dispose();
      musicLibrary.dispose();
      musicDesktop.dispose();
      musicPage.dispose();
      musicBackground.dispose();
      musicFontAssetQuery.dispose();
      musicFontAssets.dispose();
    },
  };
}

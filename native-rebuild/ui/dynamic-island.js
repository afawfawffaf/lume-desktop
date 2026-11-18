import {toggle} from './dom.js';
import {settingsFrame,settingsSelect,syncSettingsSelect,syncSettingsControlBusy} from './settings-frame.js';
import {t} from './localization.js';

export function validIslandState(m) {
  const keys=['version','type','enabled','running','locked','expanded','focusing','finished','remaining',
    'hideFullscreen','topmost','hover','autoHide','size','offset','material','loadStatus','error','result','saved'];
  return !!m && typeof m==='object' && !Array.isArray(m) && Object.keys(m).length===keys.length &&
    keys.every(key=>Object.hasOwn(m,key)) && m.version===1 && m.type==='dynamicIsland-state' &&
    ['enabled','running','locked','expanded','focusing','finished','saved','hideFullscreen','topmost','hover','autoHide'].every(key=>typeof m[key]==='boolean') &&
    islandSizes.includes(m.size) && islandMaterials.includes(m.material) &&
    typeof m.offset==='number' && Number.isFinite(m.offset) &&
    Number.isInteger(m.remaining) && m.remaining>=0 && m.remaining<=1500 &&
    ['fresh','loaded','recovered','failed'].includes(m.loadStatus) && ['','runtime','save','rollback'].includes(m.error) &&
    ['snapshot','applied'].includes(m.result) && (!m.running || m.enabled) &&
    (!m.saved || (m.result==='applied' && !m.error && m.loadStatus==='loaded'));
}
export const islandSizes=['small','standard','large'];
// One WorkBuddy account. The reply never carries a token: only whether the
// account is connected and the last reading the host fetched. `id` is the uid
// the API assigned and is the only handle used to remove an account.
const WORKBUDDY_SLOT_KEYS=['build','id','connected','label','status','remaining','plan'];
export function validWorkBuddyState(m) {
  const keys=['version','type','slots','desktopAvailable','aiAvailable'];
  if(!m||typeof m!=='object'||Array.isArray(m)||Object.keys(m).length!==keys.length||
    !keys.every(key=>Object.hasOwn(m,key))||m.version!==1||m.type!=='workbuddy-state'||
    typeof m.desktopAvailable!=='boolean'||typeof m.aiAvailable!=='boolean'||
    !Array.isArray(m.slots))return false;
  return m.slots.every(s=>{
    if(!s||typeof s!=='object'||Object.keys(s).length!==WORKBUDDY_SLOT_KEYS.length||
      !WORKBUDDY_SLOT_KEYS.every(key=>Object.hasOwn(s,key)))return false;
    return ['desktop','ai'].includes(s.build)&&
      typeof s.id==='string'&&s.id.length<=128&&
      typeof s.connected==='boolean'&&typeof s.label==='string'&&s.label.length<=64&&
      typeof s.status==='string'&&s.status.length<=16&&
      typeof s.remaining==='number'&&Number.isFinite(s.remaining)&&
      typeof s.plan==='string'&&s.plan.length<=64;
  });
}
// The quota page's reply. It never carries the key itself, only whether one is
// stored and the last reading the host fetched.
export function validQuotaState(m) {
  const keys=['version','type','hasKey','running','ok','available','error','amount','currency'];
  return !!m && typeof m==='object' && !Array.isArray(m) && Object.keys(m).length===keys.length &&
    keys.every(key=>Object.hasOwn(m,key)) && m.version===1 && m.type==='quota-state' &&
    ['hasKey','running','ok','available'].every(key=>typeof m[key]==='boolean') &&
    typeof m.error==='string' && m.error.length<=16 && typeof m.amount==='string' && m.amount.length<=32 &&
    typeof m.currency==='string' && m.currency.length<=8 && (!m.running || m.hasKey);
}
export const islandMaterials=['none','liquid-glass','frosted-glass'];
const materialOptions=()=>[['none',t('纯黑','Black')],['liquid-glass',t('液态玻璃','Liquid glass')],['frosted-glass',t('磨砂玻璃','Frosted glass')]];
const sizeOptions=()=>[['small',t('小','Small')],['standard',t('标准','Standard')],['large',t('大','Large')]];
// Display switches: [action, state key, label, hint].
const displayToggles=()=>[
  ['autoHide','autoHide',t('无活动时自动隐藏','Auto-hide when idle'),t('闲置几秒后收到屏幕最上方，只露出一条边；鼠标移上去即滑出，切歌或暂停时也会短暂出现','After a few idle seconds it slides up against the top of the screen, leaving a sliver; hover to bring it back. It also peeks out on track changes')],
  ['hideFullscreen','hideFullscreen',t('全屏和游戏时隐藏','Hide for fullscreen apps and games'),t('全屏程序、游戏或演示在前台时立即让开，不遮挡画面','Steps aside at once while a fullscreen app, game or presentation is in front')],
  ['topmost','topmost',t('显示在窗口上方','Show above windows'),t('关闭后只在桌面上可见','When off, it is only visible on the desktop')],
  ['hover','hover',t('悬停自动展开','Expand on hover'),t('鼠标停留片刻即展开，移开后收起','Expands after a short rest and collapses when the pointer leaves')],
];
export function islandStatus(state) {
  const status=state.dynamicIslandStatus;
  if(status==='loading')return t('正在读取设置…','Loading settings…');
  if(status==='saving')return t('正在应用…','Applying…');
  if(status==='unavailable')return t('桌面宿主未连接，设置未保存','Desktop host disconnected. Settings were not saved.');
  if(status==='error')return state.dynamicIslandError==='runtime'
    ? t('灵动岛启动失败，请重试','Could not start the island. Please try again.')
    : t('设置读取或保存失败，原有配置已保留','Could not load or save settings. Your previous settings were preserved.');
  if(status==='recovered')return t('已从备份读取配置，下一次修改后保存','Settings restored from backup; changes will save them.');
  return '';
}
const busy=state=>['loading','saving','unavailable'].includes(state.dynamicIslandStatus);
// The quota page's only configuration: a key, saved encrypted by the host.
// Nothing here ever shows the stored key back to the page.
function quotaSection(quota) {
  const q=quota||{hasKey:false,running:false,ok:false,available:false,error:'',amount:'',currency:''};
  const status=q.running?t('正在查询余额…','Fetching balance…'):
    !q.hasKey?t('未设置密钥，灵动岛不会显示该页','No key saved; the page stays hidden'):
    q.ok?`${q.currency==='USD'?'$':'¥'}${q.amount} · ${q.available?t('可用','Available'):t('余额不足','Insufficient')}`:
    q.error==='auth'?t('密钥无效','Key rejected'):
    q.error==='parse'?t('响应无法识别','Unrecognised response'):
    q.error==='network'?t('网络请求失败','Network request failed'):
    q.error==='save'?t('密钥保存失败','Could not save the key'):
    t('尚未查询','Not fetched yet');
  return `<div class="setting-row island-option"><label>${t('DeepSeek 余额','DeepSeek balance')}<small>${t('保存 API Key 后，灵动岛会多一个余额页签，鼠标滚轮可切换。密钥用 Windows 凭据加密保存，只用于向 DeepSeek 查询余额。','After saving an API key the island gains a balance page you reach with the wheel. The key is encrypted with Windows credentials and only ever sent to DeepSeek.')}</small></label><span class="island-quota-status">${status}</span></div>`+
    `<div class="setting-row island-quota-input"><label for="island-quota-key">${t('API Key','API Key')}</label><input id="island-quota-key" type="password" autocomplete="off" spellcheck="false" maxlength="512" placeholder="${q.hasKey?t('已保存，输入可替换','Saved; type to replace'):'sk-...'}" ${q.running?'disabled':''}><button type="button" class="reset" data-action="island-quota-save" ${q.running?'disabled':''}>${t('保存','Save')}</button><button type="button" class="reset" data-action="island-quota-clear" ${q.hasKey?'':'disabled'}>${t('清除','Clear')}</button></div>`;
}

// WorkBuddy and WorkBuddy AI are two separate logins, so the settings page has
// one block each: its own account list and its own connect button. How many
// accounts a build has is not fixed — every connected account is listed, and
// connecting adds another one to that same build.
function workbuddySection(workbuddy) {
  const state=workbuddy||{slots:[],desktopAvailable:false,aiAvailable:false};
  const statusOf=slot=>{
    if(slot.status==='waiting')return t('等待浏览器授权…','Waiting for the browser…');
    if(slot.status==='fetching')return t('正在查询…','Fetching…');
    if(slot.status==='ready')return t('已连接','Connected');
    if(slot.status==='expired')return t('授权已过期，请重新连接','Expired; connect again');
    if(slot.status==='auth')return t('授权无效','Rejected');
    if(slot.status==='network')return t('网络失败','Network failed');
    if(slot.status==='parse')return t('响应无法识别','Unrecognised response');
    return t('未连接','Not connected');
  };
  const row=slot=>{
    const busy=slot.status==='waiting'||slot.status==='fetching';
    const name=slot.connected?(slot.label||t('已连接账号','Connected account')):t('未连接','Not connected');
    const figure=slot.status==='ready'&&slot.connected?` · ${slot.remaining.toFixed(2)} ${t('积分','credits')}`:'';
    // A row that is only a pending connection has no uid yet, so it offers no
    // disconnect button: there is nothing stored to remove.
    const control=slot.connected
      ? `<button type="button" class="reset" data-action="island-workbuddy-disconnect" data-wb-build="${slot.build}" data-wb-id="${slot.id}" ${busy?'disabled':''}>${t('断开','Disconnect')}</button>`
      : '';
    return `<div class="setting-row island-workbuddy-row"><label>${name}
      <small>${statusOf(slot)}${figure}</small></label>${control}</div>`;
  };
  const build=kind=>{
    const accounts=state.slots.filter(s=>s.build===kind);
    const title=kind==='ai'?t('WorkBuddy AI 积分','WorkBuddy AI credits'):t('WorkBuddy 积分','WorkBuddy credits');
    const hint=kind==='ai'
      ? t('连接的是 WorkBuddy AI 版本。需要几个账号就点几次「连接」，每个账号单独授权一次。','This signs in to the WorkBuddy AI build. Connect as many accounts as you need; each is authorised once.')
      : t('连接的是 WorkBuddy 桌面版。需要几个账号就点几次「连接」，每个账号单独授权一次。','This signs in to the WorkBuddy desktop build. Connect as many accounts as you need; each is authorised once.');
    return `<div class="setting-row island-option"><label>${title}<small>${hint}${t('点「连接」后会在浏览器打开官网登录页，登录完成即自动读取积分。令牌用 Windows 凭据加密保存在本机，只用于查询积分。','Connect opens the sign-in page in your browser; credits are read once you finish. The token is encrypted with Windows credentials and only ever sent to WorkBuddy.')}</small></label></div>`+
      (accounts.length?accounts.map(row).join(''):`<div class="setting-row island-workbuddy-row"><label>${t('还没有连接账号','No account connected')}<small>${statusOf({status:'none'})}</small></label></div>`)+
      `<div class="setting-row island-workbuddy-row"><label>${t('添加账号','Add an account')}</label><button type="button" class="reset" data-action="island-workbuddy-connect" data-wb-build="${kind}">${t('连接','Connect')}</button></div>`;
  };
  return build('desktop')+build('ai');
}

export function islandPage(state,tab='settings') {
  const p=state.dynamicIsland,disabled=busy(state),off=disabled||!p.enabled;
  const quota=state.quota;
  const workbuddy=state.workbuddy;
  const button=(action,label,inactive=false)=>`<button type="button" class="reset" data-action="dynamicIsland-${action}" ${inactive?'disabled':''}>${label}</button>`;
  const body=tab==='settings' ?
    `<div class="island-preview" aria-hidden="true"><span class="island-cover"></span><span class="island-title">${t('正在播放','Now playing')}</span><span class="island-bars"><i></i><i></i><i></i><i></i></span></div>`+
    `<div class="setting-row"><label>${t('启用灵动岛','Enable Dynamic Island')}</label>${toggle(p.enabled,t('灵动岛','Dynamic Island'),'dynamicIsland-toggle',disabled)}</div>`+
    `<div class="setting-row"><label for="dynamicIsland-material">${t('材质','Material')}</label>${settingsSelect('dynamicIsland-material',t('材质','Material'),p.material,materialOptions(),disabled)}</div>`+
    `<div class="setting-row"><label for="dynamicIsland-size">${t('大小','Size')}</label>${settingsSelect('dynamicIsland-size',t('大小','Size'),p.size,sizeOptions(),disabled)}</div>`+
    quotaSection(quota)+
    workbuddySection(workbuddy)+
    displayToggles().map(([action,key,label,hint])=>`<div class="setting-row island-option"><label>${label}<small>${hint}</small></label>${toggle(p[key],label,`dynamicIsland-${action}`,disabled)}</div>`).join('')+
    `<div class="setting-row"><label>${t('展开面板','Expanded panel')}</label>${button(p.expanded?'collapse':'expand',p.expanded?t('收起','Collapse'):t('展开','Expand'),off)}</div>`+
    `<div class="reset-row">${button('reset',t('恢复默认设置','Restore defaults'),disabled)}</div>` :
    `<div class="settings-details"><h3>${t('功能说明','About the island')}</h3><p>${t('胶囊固定在顶部中央，点击展开或收起；展开后鼠标离开几秒会自动收起。','The capsule stays centered at the top and expands or collapses when clicked; it collapses a few seconds after the pointer leaves.')}</p><p>${t('默认在全屏程序和游戏前台时完全隐藏，不抢焦点。','By default it hides completely in front of fullscreen apps and games and never takes focus.')}</p><p>${t('展开后点击其他任意位置即收起；右键胶囊可切换材质。','When expanded, clicking anywhere else collapses it; right-click the capsule to change the material.')}</p><p>${t('播放音乐时显示封面、歌曲、歌手、进度和节奏频谱，并提供切歌与播放控制。','When music is playing, it shows the cover, track, artist, progress and beat spectrum with playback controls.')}</p></div>`;
  return settingsFrame({title:t('灵动岛','Dynamic Island'),description:t('音乐播放信息与控制','Music playback information and controls'),
    tab,body,status:islandStatus(state),error:state.dynamicIslandStatus==='error',scope:'dynamicIsland',backLabel:t('返回桌面美化','Back to desktop styles')});
}
export function syncIslandControls(root,state) {
  const p=state.dynamicIsland,disabled=busy(state);
  for(const [action,value] of [['toggle',p.enabled],...displayToggles().map(([a,key])=>[a,p[key]])]) {
    root.querySelectorAll(`[data-action="dynamicIsland-${action}"]`).forEach(button=>{
      button.classList.toggle('checked',value);button.setAttribute('aria-checked',String(value));
      syncSettingsControlBusy(button,disabled);
    });
  }
  root.querySelectorAll('[data-action^="dynamicIsland-"]').forEach(button=>{
    const action=button.dataset.action.slice('dynamicIsland-'.length);
    if(['toggle','lock','hideFullscreen','topmost','hover','autoHide'].includes(action))return;
    syncSettingsControlBusy(button,disabled||(action!=='reset'&&!p.enabled));
    if(['expand','collapse'].includes(action)) {
      button.dataset.action=`dynamicIsland-${p.expanded?'collapse':'expand'}`;
      button.textContent=p.expanded?t('收起','Collapse'):t('展开','Expand');
    }
  });
  syncSettingsSelect(root,'dynamicIsland-size',p.size,sizeOptions(),disabled,true);
  syncSettingsSelect(root,'dynamicIsland-material',p.material,materialOptions(),disabled,true);
  const status=root.querySelector('[data-settings-scope="dynamicIsland"] .status');
  if(status){status.textContent=islandStatus(state);status.hidden=!status.textContent;status.setAttribute('role',state.dynamicIslandStatus==='error'?'alert':'status');}
}

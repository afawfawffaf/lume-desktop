// WorkBuddy and WorkBuddy AI are two separate logins with no fixed number of
// accounts per build. These checks hold that shape in place: what the page may
// send, what the host may reply, and that the settings page shows one block per
// build rather than a fixed set of numbered slots.
import test from 'node:test';
import assert from 'node:assert/strict';
import {createBridge, validHostMessage} from '../bridge.js';
import {validWorkBuddyState, islandPage} from '../dynamic-island.js';
import {initialState} from '../state.js';

const account = fields => ({version: 1, type: 'workbuddy-state',
  slots: [{build: 'desktop', id: 'uid-1', connected: true, label: '枪意',
    status: 'ready', remaining: 2494.82, plan: '体验版'}],
  desktopAvailable: true, aiAvailable: false, ...fields});

test('workbuddy state accepts the documented shape and rejects malformed ones', () => {
  assert.ok(validWorkBuddyState(account()));
  assert.ok(validHostMessage(account()));
  // A token or any credential must never appear in the reply.
  assert.equal(validHostMessage(account({slots: [{...account().slots[0], token: 't'}]})), false);
  // The removed slot number is not part of the shape any more.
  assert.equal(validHostMessage(account({slots: [{...account().slots[0], slot: 0}]})), false);
  assert.equal(validHostMessage(account({slots: [{...account().slots[0], build: 'mac'}]})), false);
  assert.equal(validHostMessage(account({slots: [{...account().slots[0], id: 7}]})), false);
  assert.equal(validHostMessage(account({slots: [{...account().slots[0], remaining: '2494'}]})), false);
  assert.equal(validHostMessage(account({slots: 'none'})), false);
});

test('the account count is not fixed', () => {
  const many = Array.from({length: 9}, (_, i) => ({build: i % 2 ? 'ai' : 'desktop',
    id: `uid-${i}`, connected: true, label: `acct-${i}`, status: 'ready',
    remaining: 10 + i, plan: ''}));
  assert.ok(validWorkBuddyState(account({slots: many, aiAvailable: true})));
  // No account at all is a valid reply too.
  assert.ok(validWorkBuddyState(account({slots: [], desktopAvailable: false, aiAvailable: false})));
});

test('connect names only a build, disconnect names the account by its uid', () => {
  const sent = [];
  const bridge = createBridge({postMessage: value => sent.push(value)});
  // Both builds are separate logins: each connect is its own browser handshake.
  assert.ok(bridge.workbuddy('connect', 'desktop'));
  assert.deepEqual(sent.at(-1), {version: 1, scope: 'workbuddy', action: 'connect', build: 'desktop'});
  assert.ok(bridge.workbuddy('connect', 'ai'));
  assert.deepEqual(sent.at(-1), {version: 1, scope: 'workbuddy', action: 'connect', build: 'ai'});
  assert.ok(bridge.workbuddy('disconnect', 'desktop', 'uid-42'));
  assert.deepEqual(sent.at(-1), {version: 1, scope: 'workbuddy',
    action: 'disconnect', build: 'desktop', id: 'uid-42'});
  assert.ok(bridge.workbuddy('refresh'));
  assert.deepEqual(sent.at(-1), {version: 1, scope: 'workbuddy', action: 'refresh'});
  assert.ok(bridge.workbuddy('snapshot'));
  // A slot number is gone, and nothing that is not a short printable uid passes.
  for (const [action, build, id] of [['connect', 'mac', ''], ['connect', '', ''],
    ['disconnect', 'desktop', ''], ['disconnect', 'desktop', 'has space'],
    ['disconnect', 'desktop', 'uid\nnewline'], ['disconnect', 'desktop', 'x'.repeat(129)],
    ['disconnect', 'desktop', 0], ['disconnect', 'mac', 'uid-1'], ['nope', 'desktop', '']])
    assert.equal(bridge.workbuddy(action, build, id), false, `${action} ${build} ${id}`);
});

test('each build gets its own block and its own connect button', () => {
  const state = {...initialState(), workbuddy: account({slots: [
    {build: 'desktop', id: 'd-1', connected: true, label: '账号甲',
      status: 'ready', remaining: 2494.82, plan: '体验版'},
    {build: 'ai', id: 'a-1', connected: true, label: '账号乙',
      status: 'ready', remaining: 12, plan: ''},
  ], aiAvailable: true})};
  const html = islandPage(state, 'settings');
  // Two headings, not one list of numbered slots.
  assert.match(html, /WorkBuddy 积分|WorkBuddy credits/);
  assert.match(html, /WorkBuddy AI 积分|WorkBuddy AI credits/);
  assert.doesNotMatch(html, /账号 1|Account 1/);
  // One connect button per build, carrying the build and no slot.
  assert.match(html, /data-action="island-workbuddy-connect" data-wb-build="desktop"/);
  assert.match(html, /data-action="island-workbuddy-connect" data-wb-build="ai"/);
  assert.doesNotMatch(html, /data-wb-slot/);
  // Each connected account is addressable by uid.
  assert.match(html, /data-action="island-workbuddy-disconnect" data-wb-build="desktop" data-wb-id="d-1"/);
  assert.match(html, /data-action="island-workbuddy-disconnect" data-wb-build="ai" data-wb-id="a-1"/);
  assert.match(html, /2494\.82/);
});

test('a build with no account still offers connect', () => {
  const state = {...initialState(),
    workbuddy: account({slots: [], desktopAvailable: false, aiAvailable: false})};
  const html = islandPage(state, 'settings');
  assert.match(html, /还没有连接账号|No account connected/);
  assert.match(html, /data-action="island-workbuddy-connect" data-wb-build="desktop"/);
  assert.match(html, /data-action="island-workbuddy-connect" data-wb-build="ai"/);
});

test('a pending connection lists its state without a disconnect button', () => {
  // A handshake that has not produced a uid yet cannot be removed: there is
  // nothing stored to remove.
  const state = {...initialState(), workbuddy: account({slots: [
    {build: 'ai', id: '', connected: false, label: '', status: 'waiting',
      remaining: 0, plan: ''},
  ]})};
  const html = islandPage(state, 'settings');
  assert.match(html, /等待浏览器授权|Waiting for the browser/);
  assert.doesNotMatch(html, /island-workbuddy-disconnect/);
});

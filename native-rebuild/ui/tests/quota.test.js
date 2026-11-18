// The quota page's wire format and its only free-text command, checked without
// a network call: what the page may send and what the host will accept has to
// agree, and a key must never reach the page.
import test from 'node:test';
import assert from 'node:assert/strict';
import {createBridge, validHostMessage} from '../bridge.js';
import {validQuotaState, islandPage} from '../dynamic-island.js';
import {initialState} from '../state.js';

const quota = fields => ({version: 1, type: 'quota-state', hasKey: true, running: false,
  ok: true, available: true, error: '', amount: '110.00', currency: 'CNY', ...fields});

test('quota state accepts the documented shape and rejects malformed ones', () => {
  assert.ok(validQuotaState(quota()));
  assert.ok(validHostMessage(quota()));
  // No key field exists in the reply at all: the page can never receive one.
  assert.equal(validHostMessage(quota({key: 'sk-secret'})), false);
  assert.equal(validHostMessage(quota({amount: 110})), false);
  assert.equal(validHostMessage(quota({hasKey: 'yes'})), false);
  assert.equal(validHostMessage(quota({error: 'x'.repeat(17)})), false);
  // A fetch cannot be running without a stored key.
  assert.equal(validHostMessage(quota({hasKey: false, running: true})), false);
});

test('the page sends only the four quota actions, and only a printable key', () => {
  const sent = [];
  const bridge = createBridge({postMessage: value => sent.push(value)});
  assert.ok(bridge.quota('set', 'sk-0123456789abcdef'));
  assert.deepEqual(sent.at(-1), {version: 1, scope: 'quota', action: 'set', key: 'sk-0123456789abcdef'});
  assert.ok(bridge.quota('clear'));
  assert.deepEqual(sent.at(-1), {version: 1, scope: 'quota', action: 'clear'});
  assert.ok(bridge.quota('refresh'));
  assert.ok(bridge.quota('snapshot'));
  // A key is trimmed on the way out; nothing else is accepted.
  assert.ok(bridge.quota('set', '  sk-trimmed  '));
  assert.equal(sent.at(-1).key, 'sk-trimmed');
  for (const [action, key] of [['save', ''], ['set', ''], ['set', '   '], ['set', 'has space'],
    ['set', 'sk-\n-newline'], ['set', 'sk-\u4e2d\u6587'], ['set', 'x'.repeat(513)], ['nope', '']])
    assert.equal(bridge.quota(action, key), false, `${action} ${key}`);
});

test('the settings page shows a key field and never a stored key', () => {
  const state = {...initialState(), quota: quota({hasKey: true, ok: false, error: 'auth'})};
  const html = islandPage(state, 'settings');
  assert.match(html, /type="password"/);
  assert.match(html, /id="island-quota-key"/);
  assert.match(html, /data-action="island-quota-save"/);
  assert.match(html, /data-action="island-quota-clear"/);
  // The placeholder says a key is saved; no actual key value is rendered.
  assert.match(html, /已保存，输入可替换|Saved; type to replace/);
  assert.doesNotMatch(html, /value="sk-/);
  assert.match(html, /密钥无效|Key rejected/);
});

test('the page reports each failure state in words, never as a zero balance', () => {
  const text = quota2 => islandPage({...initialState(), quota: quota2}, 'settings');
  assert.match(text(quota({hasKey: false, ok: false, amount: '', currency: ''})), /未设置密钥|No key saved/);
  assert.match(text(quota({ok: false, error: 'network', amount: '', currency: ''})), /网络请求失败|Network request failed/);
  assert.match(text(quota({ok: false, error: 'parse', amount: '', currency: ''})), /无法识别|Unrecognised/);
  assert.match(text(quota({ok: false, error: 'save'})), /保存失败|Could not save/);
  // A reading that succeeded shows the amount and the availability line.
  assert.match(text(quota({available: false})), /110\.00/);
  assert.match(text(quota({available: false})), /余额不足|Insufficient/);
});

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
  '../vava-station/openwrt/luci-app-vava-agent/htdocs/luci-static/resources/view/vava-agent/settings.js'), 'utf8');

async function render(readonly = false) {
  let tick, now = 0, modal = null, pairing = { id: 'old', phase: 'completed', active: false, remaining_ms: 0 };
  const commands = [], notifications = [];
  let devices = [
    { slot: 0, serial: 'CAMERA-A', paired: true, state: 'sleep_observed', profile: { name: '门口' }, recording: {} },
    { slot: 1, serial: 'CAMERA-B', paired: true, state: 'offline', profile: {}, recording: {} }
  ];
  let removal = null;
  let commandError, statusError, heldStatus;
  function E(tag, attrs = {}, children = []) {
    return { tag, attrs, value: attrs.value || '', children: Array.isArray(children) ? children : [children],
      replaceChildren(...nodes) { this.children = nodes; },
      set textContent(value) { this.children = [value]; } };
  }
  class Map {
    constructor() { this.tabs = []; this.options = []; }
    section() {
      return {
        tab: (name, title) => this.tabs.push({ name, title }),
        taboption: (tab, type, name) => {
          const option = { tab, type, name, depends() {} };
          this.options.push(option);
          return option;
        }
      };
    }
    async render() {
      // LuCI resolves the UCI write ACL while loading the form map.
      this.readonly = readonly;
      return E('form', {}, this.tabs.map(tab => E('div', { 'data-tab': tab.name, 'data-tab-title': tab.title },
        this.options.filter(option => option.tab === tab.name && option.renderWidget).map(option => option.renderWidget()))));
    }
  }
  function status() {
    return { code: 0, stdout: JSON.stringify({ ok: true,
      devices, storage: { records: 2, available: true }, clock_valid: true, pairing, removal }) };
  }
  const view = vm.runInNewContext(`(function() { ${source}\n })()`, {
    E, URL, performance: { now: () => now },
    form: { Map, NamedSection: class {}, Flag: class {}, Value: class {}, DummyValue: class {} },
    view: { extend: definition => definition },
    uci: { load: async () => {}, get: () => undefined },
    rpc: { declare: () => async () => ({ 'vava-agent': { instances: { main: { running: true } } } }) },
    fs: { exec: async (file, args) => {
      if (file.endsWith('-status')) {
        if (statusError) throw new Error('unreachable');
        if (heldStatus) return heldStatus.promise;
        return status();
      }
      if (file.endsWith('-name')) {
        commands.push(['name', ...args]);
        const camera=devices.find(device=>String(device.slot)===args[0]&&device.serial===args[1]);
        const error=commandError||(!camera?'camera_identity_changed':
          (String(camera.profile?.revision||0)!==args[2]?'configuration_conflict':null));
        if(error)return {code:1,stdout:JSON.stringify({ok:false,error})};
        camera.profile={name:args[3],revision:(camera.profile?.revision||0)+(args[3]===(camera.profile?.name||'')?0:1)};
        return {code:0,stdout:JSON.stringify({ok:true,profile:camera.profile})};
      }
      if (file.endsWith('-unpair') || file.endsWith('-reset')) {
        const action = file.endsWith('-reset') ? 'reset' : 'unpair';
        commands.push([action, ...args]);
        const camera = devices.find(device => String(device.slot) === args[0] && device.serial === args[1]);
        const error = commandError || (!camera ? 'camera_identity_changed' :
          (camera.recording.active ? 'recording_must_finish_before_remove' : null));
        if (error) return { code: 1, stdout: JSON.stringify({ ok: false, error }) };
        if(args.length===4){
          if(removal?.id!==args[3]||removal.phase!=='result_unknown')return {code:1,stdout:'{"ok":false,"error":"removal_confirmation_conflict"}'};
          removal={...removal,phase:'completed',active:false,operator_confirmed:true};devices=devices.filter(device=>device!==camera);
        }else removal={id:'remove-1',action:action==='reset'?'reset':'remove',slot:camera.slot,serial:camera.serial,active:true,phase:'waiting_camera'};
        return { code: 0, stdout: JSON.stringify({ok:true,operation_id:removal.id,removal}) };
      }
      commands.push([...args]);
      if (commandError) return { code: 1, stdout: JSON.stringify({ ok: false, error: commandError }) };
      if (args[0] === 'open') {
        pairing = { id: 'new', phase: 'opening_ap', active: true, remaining_ms: 150000 };
        return { code: 0, stdout: JSON.stringify({ ok: true, operation_id: 'new', window_seconds: 120 }) };
      }
      pairing = { ...pairing, phase: 'cancelled', active: false, remaining_ms: 0 };
      return { code: 0, stdout: '{"ok":true}' };
    } },
    poll: { add: callback => { tick = callback; } },
    ui: { createHandlerFn: (_, callback) => callback,
      showModal: (title, children) => { modal = E('dialog', {}, [E('h4', {}, title), ...children]); },
      hideModal: () => { modal = null; }, addNotification: (...args) => notifications.push(args) },
    // Some LuCI versions do not provide a satisfied nodespec for this view.
    L: { hasViewPermission: () => null }
  });
  await view.load();
  const root = await view.render();
  const settle = () => new Promise(resolve => setImmediate(resolve));
  await settle();
  return { root, commands, notifications, get modal() { return modal; },
    setPairing(value) { pairing = value; },
    setDevices(value) { devices = value; },
    setRemoval(value) {
      removal=value;
      if(value?.phase==='completed'&&value.action!=='reset')devices=devices.filter(device=>device.serial!==value.serial||device.slot!==value.slot);
    },
    failCommand(value) { commandError = value; },
    failStatus(value) { statusError = value; },
    holdStatus() {
      const snapshot = status();
      let release;
      const promise = new Promise(resolve => { release = () => resolve(snapshot); });
      heldStatus = { promise };
      return () => { heldStatus = null; release(); };
    },
    async advance(ms) { now += ms; await tick(); await settle(); },
    async start() { await buttons(root)[0].attrs.click(); await settle(); },
    async unpair(index = 0) { await buttons(root).filter(button => text(button) === '删除摄像头')[index].attrs.click(); await settle(); },
    async confirmUnpair() { await buttons(modal).find(button => text(button) === '恢复出厂并删除').attrs.click(); await settle(); },
    async reset(index = 0) { await buttons(root).filter(button => text(button) === '恢复出厂设置')[index].attrs.click(); await settle(); },
    async confirmReset() { await buttons(modal).find(button => text(button) === '确认恢复出厂设置').attrs.click(); await settle(); },
    async rename(index = 0) { await buttons(root).filter(button => text(button) === '重命名')[index].attrs.click(); await settle(); },
    setName(value) { nodes(modal).find(node=>node.tag==='input').value=value; },
    async saveName() { await buttons(modal).find(button=>text(button)==='保存名称').attrs.click(); await settle(); },
    async dismiss() { await buttons(modal)[0].attrs.click(); await settle(); }
  };
}

function nodes(root) {
  if (!root || typeof root !== 'object') return [];
  return [root, ...(root.children || []).flatMap(nodes)];
}
function buttons(root) { return nodes(root).filter(node => node.tag === 'button'); }
function text(root) {
  if (!root) return '';
  if (typeof root === 'string') return root;
  return (root.children || []).map(text).join(' ');
}
function active(remaining = 120000) {
  return { id: 'new', phase: 'waiting_camera', active: true, remaining_ms: remaining };
}

test('LuCI administrator can pair; pairing and status occupy separate native tabs', async () => {
  const { root } = await render();
  assert.equal(buttons(root).length, 7);
  // HTML boolean attributes must be absent for enabled controls; "false" still disables.
  for (const button of buttons(root)) assert.equal(button.attrs.disabled, null);
  assert.equal(text(buttons(root)[0]), '打开配对窗口');
  assert.equal(root.tag, 'form');
  assert.deepEqual(Array.from(root.children, child => child.attrs['data-tab']),
    ['general', 'server', 'pairing', 'camera', 'storage', 'advanced', 'status']);
  const pairingTab = root.children.find(child => child.attrs['data-tab'] === 'pairing');
  const statusTab = root.children.find(child => child.attrs['data-tab'] === 'status');
  assert.equal(buttons(pairingTab).length, 7);
  assert.doesNotMatch(text(pairingTab), /关闭配对窗口/);
  assert.equal(buttons(statusTab).length, 0);
  assert.doesNotMatch(text(pairingTab), /Agent 状态/);
  assert.match(text(statusTab), /Agent 状态/);
  assert.match(text(root), /服务运行中/);
  assert.match(text(root), /本地录像：2 段/);
  assert.match(text(root), /配对阶段：已完成/);
});

test('LuCI read-only UCI permission keeps pairing controls disabled', async () => {
  for (const button of buttons((await render(true)).root)) assert.equal(button.attrs.disabled, true);
});

test('pairing uses a modal, waits for AP readiness and counts down between backend polls', async () => {
  const ui = await render();
  await ui.start();
  assert.deepEqual(ui.commands, [['open']]);
  assert.match(text(ui.modal), /已请求 120 秒配对窗口/);
  assert.match(text(ui.modal), /热点就绪后开始倒计时/);
  assert.doesNotMatch(text(ui.modal), /剩余 150 秒/);
  assert.equal(ui.notifications.length, 0);
  ui.setPairing(active());
  await ui.advance(2000);
  assert.match(text(ui.modal), /剩余 120 秒/);
  await ui.advance(1000);
  assert.match(text(ui.modal), /剩余 119 秒/);
  ui.setPairing(active(0));
  await ui.advance(119000);
  assert.match(text(ui.modal), /正在确认配对结果/);
  assert.doesNotMatch(text(ui.modal), /已超时/);
  ui.setPairing({ id: 'new', phase: 'expired', active: false, remaining_ms: 0 });
  await ui.advance(2000);
  assert.match(text(ui.modal), /配对窗口已超时/);
  await ui.advance(3000);
  assert.doesNotMatch(text(ui.modal), /剩余/);
  await ui.dismiss();
  assert.equal(ui.modal, null);
});

test('success is based on camera confirmation, and closing result does not cancel pairing', async () => {
  const ui = await render();
  await ui.start();
  ui.setPairing({ id: 'new', phase: 'completed', active: false, serial: 'camera-1', remaining_ms: 0 });
  await ui.advance(2000);
  assert.match(text(ui.modal), /摄像头配对成功/);
  assert.match(text(ui.modal), /camera-1/);
  await ui.dismiss();
  assert.deepEqual(ui.commands, [['open']]);
});

test('active dialog cancels on the backend; a failed cancellation stays visible and can retry', async () => {
  const ui = await render();
  await ui.start();
  ui.failCommand('backend unavailable');
  await ui.dismiss();
  assert.match(text(ui.modal), /关闭失败/);
  assert.equal(text(buttons(ui.modal)[0]), '取消配对');
  assert.equal(buttons(ui.modal)[0].disabled, false);
  ui.failCommand(null);
  await ui.dismiss();
  assert.equal(ui.modal, null);
  assert.deepEqual(ui.commands, [['open'], ['close'], ['close']]);
  await ui.advance(2000);
  assert.equal(ui.modal, null);
});

test('failed open and unavailable status are shown inside the dialog', async () => {
  const ui = await render();
  ui.failCommand('camera_slots_full');
  await ui.start();
  assert.match(text(ui.modal), /已登记 4 台摄像头/);
  await ui.dismiss();
  ui.failCommand(null);
  await ui.start();
  ui.failStatus(true);
  await ui.advance(2000);
  assert.match(text(ui.modal), /暂时无法读取基站状态/);
  assert.doesNotMatch(text(ui.modal), /配对成功|已超时/);
  ui.failStatus(false);
  ui.setPairing(active());
  await ui.advance(2000);
  assert.match(text(ui.modal), /剩余 120 秒/);
  assert.doesNotMatch(text(ui.modal), /暂时无法/);
  assert.equal(ui.notifications.length, 0);
});

test('a previous operation and in-flight status cannot complete a new pairing dialog', async () => {
  const ui = await render();
  const release = ui.holdStatus();
  const pending = ui.advance(5000);
  const starting = ui.start();
  await new Promise(resolve => setImmediate(resolve));
  // start() awaits the held refresh; release it before awaiting either action.
  release();
  await Promise.all([pending, starting]);
  assert.match(text(ui.modal), /正在同步本次配对状态/);
  assert.doesNotMatch(text(ui.modal), /配对成功/);
  ui.setPairing({ id: 'old', phase: 'completed', active: false, remaining_ms: 0 });
  await ui.advance(2000);
  assert.doesNotMatch(text(ui.modal), /配对成功/);
  ui.setPairing(active());
  await ui.advance(2000);
  assert.match(text(ui.modal), /剩余 120 秒/);
});

test('opening progress for an existing active operation does not create another window', async () => {
  const ui = await render();
  ui.setPairing(active(60000));
  await ui.advance(5000);
  await ui.start();
  assert.deepEqual(ui.commands, []);
  assert.match(text(ui.modal), /配对窗口已开启/);
  assert.match(text(ui.modal), /剩余 60 秒/);
});

test('in-flight active status cannot resurrect a cancelled operation', async () => {
  const ui = await render();
  await ui.start();
  const release = ui.holdStatus();
  const pending = ui.advance(2000);
  const closing = ui.dismiss();
  await new Promise(resolve => setImmediate(resolve));
  release();
  await Promise.all([pending, closing]);
  assert.equal(ui.modal, null);
  assert.match(text(ui.root), /配对阶段：已取消/);
  await ui.start();
  assert.deepEqual(ui.commands, [['open'], ['close'], ['open']]);
});

test('unpair shows the selected SN and effects, and backing out never sends a command', async () => {
  const ui = await render();
  await ui.unpair(1);
  assert.match(text(ui.modal), /CAMERA-B/);
  assert.doesNotMatch(text(ui.modal), /CAMERA-A/);
  assert.match(text(ui.modal), /历史录像、索引和解密密钥保留/);
  assert.match(text(ui.modal), /恢复这台摄像头的出厂设置/);
  assert.deepEqual(ui.commands, []);
  assert.equal(text(buttons(ui.modal)[0]), '返回');
  await ui.dismiss();
  assert.equal(ui.modal, null);
  assert.deepEqual(ui.commands, []);
});

test('confirmed unpair is guarded by slot and SN, removes only the selected row and retains clips', async () => {
  const ui = await render();
  await ui.unpair();
  await ui.confirmUnpair();
  assert.deepEqual(ui.commands, [['unpair', '0', 'CAMERA-A']]);
  assert.match(text(ui.modal), /正在唤醒摄像头/);
  assert.match(text(ui.root), /已登记摄像头：2 \/ 4/);
  ui.setRemoval({id:'remove-1',slot:0,serial:'CAMERA-A',active:false,phase:'completed'});
  await ui.advance(2000);
  assert.match(text(ui.modal), /基站登记已移除/);
  assert.equal(buttons(ui.modal)[1].hidden, true);
  assert.equal(buttons(ui.root).filter(button=>text(button)==='删除摄像头').length, 1);
  assert.match(text(ui.root), /CAMERA-B/);
  assert.match(text(ui.root), /已登记摄像头：1 \/ 4 · 本地录像：2 段/);
  await ui.dismiss();
  assert.equal(ui.modal, null);
});

test('a stale unpair confirmation cannot remove a new camera occupying the same slot', async () => {
  const ui = await render();
  await ui.unpair();
  ui.setDevices([{ slot: 0, serial: 'OTHER-CAMERA', paired: true, recording: {} }]);
  await ui.confirmUnpair();
  assert.match(text(ui.modal), /登记已变化/);
  assert.match(text(ui.root), /OTHER-CAMERA/);
  assert.deepEqual(ui.commands, [['unpair', '0', 'CAMERA-A']]);
  assert.equal(buttons(ui.modal)[1].hidden, true);
});

test('unpair controls block recording and active pairing, and unavailable status removes stale actions', async () => {
  const ui = await render();
  ui.setDevices([{ slot: 0, serial: 'CAMERA-A', paired: true, recording: { active: true } }]);
  await ui.advance(5000);
  assert.equal(buttons(ui.root).find(button => text(button) === '删除摄像头').attrs.disabled, true);
  ui.setDevices([{ slot: 0, serial: 'CAMERA-A', paired: true, recording: {} }]);
  ui.setPairing(active());
  await ui.advance(5000);
  assert.equal(buttons(ui.root).find(button => text(button) === '删除摄像头').attrs.disabled, true);
  ui.failStatus(true);
  await ui.advance(5000);
  assert.equal(buttons(ui.root).filter(button => text(button) === '删除摄像头').length, 0);
  assert.match(text(ui.root), /暂时无法读取摄像头列表/);
});

test('unpair waits out pre-removal status so the final list cannot resurrect a removed camera', async () => {
  const ui = await render();
  await ui.unpair();
  const release = ui.holdStatus();
  const pending = ui.advance(5000);
  const removing = ui.confirmUnpair();
  await new Promise(resolve => setImmediate(resolve));
  release();
  await Promise.all([pending, removing]);
  assert.match(text(ui.root), /CAMERA-A/); // request accepted, still waiting for camera reset
  ui.setRemoval({id:'remove-1',slot:0,serial:'CAMERA-A',active:false,phase:'completed'});
  await ui.advance(2000);
  assert.equal(buttons(ui.root).filter(button=>text(button)==='删除摄像头').length, 1);
  assert.match(text(ui.root), /CAMERA-B/);
});


test('unknown reset keeps the camera registered and physical confirmation requires another explicit action', async () => {
  const ui=await render();await ui.unpair();await ui.confirmUnpair();
  ui.setRemoval({id:'remove-1',slot:0,serial:'CAMERA-A',active:false,phase:'result_unknown',reason:'camera_reset_disconnected_unconfirmed'});
  await ui.advance(2000);
  assert.match(text(ui.modal),/基站登记尚未移除/);
  assert.equal(ui.commands.length,1);
  await buttons(ui.modal).find(b=>text(b)==='已确认摄像头恢复出厂，完成删除').attrs.click();
  assert.match(text(ui.modal),/仅凭摄像头离线不能确认/);
  assert.equal(ui.commands.length,1);
  await buttons(ui.modal).find(b=>text(b)==='摄像头已恢复出厂，确认完成删除').attrs.click();
  assert.deepEqual(ui.commands[1],['unpair','0','CAMERA-A','confirm','remove-1']);
  assert.match(text(ui.modal),/根据你对摄像头恢复出厂的确认/);
  assert.match(text(ui.root),/本地录像：2 段/);
});

test('standalone restore has its own confirmation and helper, and keeps cameras and clips', async () => {
  const ui=await render();await ui.reset(1);
  assert.match(text(ui.modal),/出厂参数/);
  assert.match(text(ui.modal),/CAMERA-B/);
  assert.doesNotMatch(text(ui.modal),/CAMERA-A/);
  assert.match(text(ui.modal),/保留配对、摄像头名称和基站布防计划/);
  assert.deepEqual(ui.commands,[]);
  await ui.dismiss();assert.deepEqual(ui.commands,[]);
  await ui.reset(1);await ui.confirmReset();
  assert.deepEqual(ui.commands,[['reset','1','CAMERA-B']]);
  ui.setRemoval({id:'remove-1',action:'reset',slot:1,serial:'CAMERA-B',active:false,phase:'completed'});
  await ui.advance(2000);
  assert.match(text(ui.modal),/已收到摄像头恢复参数的响应/);
  assert.doesNotMatch(text(ui.modal),/基站登记已移除/);
  assert.match(text(ui.root),/已登记摄像头：2 \/ 4 · 本地录像：2 段/);
});

test('standalone restore with missing echo cannot offer manual deletion completion', async () => {
  const ui=await render();await ui.reset();await ui.confirmReset();
  ui.setRemoval({id:'remove-1',action:'reset',slot:0,serial:'CAMERA-A',active:false,phase:'result_unknown'});
  await ui.advance(2000);
  assert.match(text(ui.modal),/恢复结果未确认，配对登记保留/);
  assert.doesNotMatch(text(ui.modal),/完成删除/);
  assert.equal(ui.commands.length,1);
});

test('rename saves a Chinese label for the selected offline camera and can restore its default label', async () => {
  const ui=await render();await ui.rename(1);
  assert.match(text(ui.modal),/CAMERA-B/);assert.doesNotMatch(text(ui.modal),/CAMERA-A/);
  ui.setName('后门 📷');await ui.advance(5000);
  assert.equal(nodes(ui.modal).find(n=>n.tag==='input').value,'后门 📷');
  await ui.saveName();
  assert.deepEqual(ui.commands,[['name','1','CAMERA-B','0','后门 📷']]);
  assert.equal(ui.modal,null);assert.match(text(ui.root),/后门 📷/);
  await ui.rename(1);ui.setName('');await ui.saveName();
  assert.deepEqual(ui.commands[1],['name','1','CAMERA-B','1','']);
  assert.equal(ui.modal,null);assert.doesNotMatch(text(ui.root),/后门 📷/);assert.match(text(ui.root),/摄像头 2/);
});

test('rename cancellation and invalid names never send commands, and valid boundary names are accepted', async () => {
  const ui=await render();await ui.rename();ui.setName('新名称');await ui.dismiss();
  assert.deepEqual(ui.commands,[]);assert.match(text(ui.root),/门口/);
  await ui.rename();
  for(const name of ['门'.repeat(33),'📷'.repeat(25),'line\nbreak','left\u202eright','\ud800']){
    ui.setName(name);await ui.saveName();assert.match(text(ui.modal),/名称最多 32/);
    assert.deepEqual(ui.commands,[]);
  }
  ui.setName('门'.repeat(32));await ui.saveName();assert.equal(ui.commands.length,1);
});

test('stale rename cannot target a new slot occupant or overwrite a concurrently updated label', async () => {
  for(const replacement of [
    {slot:0,serial:'OTHER-CAMERA',paired:true,profile:{name:'保留',revision:0}},
    {slot:0,serial:'CAMERA-A',paired:true,profile:{name:'其他页面的新名称',revision:1}}
  ]){
    const ui=await render();await ui.rename();ui.setName('我的名称');ui.setDevices([replacement]);
    await ui.saveName();
    assert.match(text(ui.modal),/登记已变化|已在其他页面修改/);
    assert.equal(nodes(ui.modal).find(n=>n.tag==='input').value,'我的名称');
    assert.doesNotMatch(text(ui.modal),/名称已保存/);
    await ui.advance(2000);assert.match(text(ui.root),new RegExp(replacement.profile.name));
  }
});

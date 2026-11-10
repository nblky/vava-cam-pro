"use strict";
"require view";
"require form";
"require rpc";
"require fs";
"require uci";
"require ui";
"require poll";

var serviceList = rpc.declare({ object: 'service', method: 'list', params: ['name'], expect: { '': {} } });
function privatePath(value) {
  return /^\/(?!$)(?!.*(?:^|\/)\.\.?(?:\/|$))[^\x00-\x20]*$/.test(value);
}
function managerURL(value) {
  try { var u = new URL(value); return !u.username && !u.password &&
    (u.protocol === 'https:' || (u.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].indexOf(u.hostname) >= 0)); }
  catch (e) { return false; }
}
return view.extend({
  load: function() { return uci.load('vava-agent'); },
  render: function() {
    var m = new form.Map('vava-agent', 'VAVA Agent',
      '基站负责摄像头配对、控制、休眠和事件短片。实时观看、云存储与摄像头设置由独立的 VAVA Server 管理。');
    var s = m.section(form.NamedSection, 'main', 'service'); s.anonymous = true;
    s.tab('general', '运行与服务'); s.tab('server', '服务器连接');
    s.tab('pairing', '摄像头配对'); s.tab('camera', '摄像头网络');
    s.tab('storage', '本地存储'); s.tab('advanced', '高级'); s.tab('status', '运行状态');
    var o = s.taboption('general', form.Flag, 'enabled', '启用 VAVA Agent'); o.rmempty = false;
    o = s.taboption('general', form.Flag, 'local_only', '本地独立模式', '开启后不连接远端服务器，仍可在本机完成配对与事件录像。'); o.rmempty = false;
    o = s.taboption('general', form.Value, 'management_url', '管理平台地址');
    o.placeholder = 'https://vava.example.com';
    o.validate = function(section, value) { return !value || managerURL(value) || '填写 HTTPS 地址；本机调试可使用回环 HTTP。'; };
    o = s.taboption('server', form.Value, 'server_host', '服务器域名 / IP'); o.datatype = 'host'; o.depends('local_only', '0'); o.rmempty = false;
    o = s.taboption('server', form.Value, 'server_port', 'UDP 端口'); o.datatype = 'port'; o.default = '3731'; o.depends('local_only', '0'); o.rmempty = false;
    o = s.taboption('server', form.Value, 'device_id', '基站设备 ID'); o.depends('local_only', '0'); o.rmempty = false;
    o.validate = function(section, value) { return /^[A-Za-z0-9_.:-]{1,31}$/.test(value) || '1–31 位字母、数字或 _ . : -'; };
    function path(tab, key, title, description) {
      var opt = s.taboption(tab, form.Value, key, title, description);
      opt.validate = function(section, value) { return privatePath(value) || '填写不含空白和路径跳转的绝对路径。'; };
      opt.rmempty = false; return opt;
    }
    path('server', 'server_key_file', '设备 PSK 文件', '服务器与本基站独享的 32 字节十六进制密钥文件，权限 0600；页面不读取或显示密钥。');
    o = s.taboption('camera', form.Value, 'camera_bind', '摄像头侧地址'); o.datatype = 'ip4addr'; o.default = '172.26.168.1';
    o = s.taboption('camera', form.Value, 'ssid', '正式 Wi-Fi 名称');
    o.validate = function(section, value) { return /^VAVA-[\x20-\x7e]{0,26}$/.test(value) || '原固件要求以 VAVA- 开头，最多 31 位可打印 ASCII 字符。'; };
    o = s.taboption('camera', form.Value, 'pairing_serial', '配对用原厂基站 SN', '按原厂规则生成 VAPair- 临时热点。');
    o.validate = function(section, value) { return !value || /^[A-Za-z0-9]{25}$/.test(value) || '原厂 SN 需为 25 位字母或数字。'; };
    path('camera', 'password_file', '正式 Wi-Fi 密码文件');
    path('storage', 'storage_mount', 'TF / USB 挂载点', '请先在 OpenWrt 磁盘管理中挂载；Agent 不会格式化磁盘，也不会向未挂载的同名目录写录像。');
    path('storage', 'record_dir', '事件短片目录');
    o = s.taboption('storage', form.Value, 'record_max_mb', '本地录像配额（MiB）'); o.datatype = 'range(1,1048576)'; o.rmempty = false;
    path('storage', 'record_keyring', '录像密钥目录', '首次启用前执行 vava-agent --storage-key-create；已有密钥必须连同状态一起备份。');
    path('storage', 'event_dir', '事件日志目录');
    path('advanced', 'state_dir', '持久状态目录');
    o = s.taboption('advanced', form.Value, 'archive_serial', '归档基站 SN');
    o.validate = function(section, value) { return !value || /^[A-Za-z0-9]{25}$/.test(value) || '填写 25 位 SN 或留空。'; };
    o = s.taboption('advanced', form.Value, 'time_offset_seconds', '摄像头时区偏移（秒）'); o.datatype = 'range(-43200,50400)'; o.default = '28800';
    o = s.taboption('advanced', form.Value, 'server_poll_ms', '控制轮询间隔（毫秒）'); o.datatype = 'range(50,5000)'; o.default = '1000';
    o = s.taboption('advanced', form.Value, 'event_limit', '离线事件队列上限'); o.datatype = 'range(16,4096)'; o.default = '1024';
    var status = E('div', { 'class': 'cbi-section', 'aria-live': 'polite' }, [E('h3', {}, 'Agent 状态'), E('p', {}, '正在读取…')]);
    var cameraList = E('div', { 'aria-live': 'polite' }, [E('h3', {}, '已登记摄像头'), E('p', {}, '正在读取…')]);
    var phases = { idle: '空闲', opening_ap: '正在开启热点', waiting_camera: '等待摄像头',
      credentials_sent: '已发送配网信息', waiting_formal_connection: '等待正式连接',
      waiting_confirmation: '等待摄像头确认', completed: '已完成', cancelled: '已取消',
      expired: '已超时', interrupted: '已中断', failed: '失败' };
    var latestRemoval = null, removalDialog = null, renameDialog = null;
    function renameCamera(device) {
      return ui.createHandlerFn(this, function() {
        if (m.readonly) return;
        var profile = device.profile || {}, d = { revision: profile.revision || 0 };
        var input = E('input', { 'class': 'cbi-input-text', id: 'vava-camera-name', type: 'text',
          value: profile.name || '', placeholder: '摄像头 ' + (device.slot + 1),
          'aria-describedby': 'vava-camera-name-help' });
        var feedback = E('p', { role: 'status', 'aria-live': 'polite' });
        var close = E('button', { 'class': 'cbi-button cbi-button-neutral', click: function() { renameDialog = null; ui.hideModal(); } }, '取消');
        var save = E('button', { 'class': 'cbi-button cbi-button-positive', click: ui.createHandlerFn(this, function() {
          if (d.saving || m.readonly) return;
          var name = input.value, bytes;
          try { bytes = unescape(encodeURIComponent(name)).length; } catch (e) { bytes = Infinity; }
          if (Array.from(name).length > 32 || bytes > 96 || /[\x00-\x1f\x7f-\x9f\u2028-\u202e\u2066-\u2069]/.test(name)) {
            feedback.textContent = '名称最多 32 个字符、96 字节，不能包含换行或控制字符。'; return;
          }
          d.saving = true; save.disabled = true; close.disabled = true; input.disabled = true;
          feedback.textContent = '正在保存…';
          return fs.exec('/usr/libexec/vava-agent-name', [String(device.slot), device.serial, String(d.revision), name])
            .then(function(result) {
              var reply; try { reply = JSON.parse(result.stdout); } catch (e) {}
              if (result.code !== 0 || !reply || !reply.ok || !reply.profile || reply.profile.name !== name) {
                var errors = { camera_identity_changed: '摄像头登记已变化，请取消后重新选择。',
                  configuration_conflict: '名称已在其他页面修改，请取消后重新打开。',
                  invalid_camera_name: '名称格式无效，请检查字符和长度。',
                  camera_removal_in_progress_or_unconfirmed: '摄像头正在恢复或删除，请先核对操作进度。',
                  profile_write_failed: '基站保存失败，请检查存储空间后重试。' };
                throw new Error(errors[reply && reply.error] || '保存结果未确认，请先查看列表中的名称，再重新打开重命名。');
              }
              d.revision = reply.profile.revision;
              if (renameDialog === d) { renameDialog = null; ui.hideModal(); }
              return refresh().then(refresh);
            }).catch(function(err) { feedback.textContent = err.message; })
            .finally(function() {
              d.saving = false; save.disabled = false; close.disabled = false; input.disabled = false;
            });
        }) }, '保存名称');
        renameDialog = d;
        ui.showModal('重命名摄像头', [E('p', {}, '摄像头 SN：' + device.serial),
          E('label', { 'for': 'vava-camera-name' }, '摄像头名称'), input,
          E('p', { id: 'vava-camera-name-help' }, '支持中文，最多 32 个字符（96 字节）。留空恢复默认名称；保存后立即生效。'),
          feedback, E('div', { 'class': 'right' }, [close, ' ', save])]);
      });
    }
    function isReset(operation) { return operation && operation.action === 'reset'; }
    function operationTitle(operation) { return isReset(operation) ? '恢复出厂设置' : '删除摄像头'; }
    function operationPhase(operation) {
      if (isReset(operation)) {
        var resetPhases = { committing: '正在清理摄像头配置缓存', completed: '已恢复摄像头参数', failed: '恢复未完成' };
        if (resetPhases[operation.phase]) return resetPhases[operation.phase];
      }
      return removalPhases[operation.phase] || operation.phase;
    }
    var latestPairing = null, latestAt = 0, dialog = null, refreshing = null, lastRefresh = -Infinity, pairingEpoch = 0;
    function drawCameras(devices, pairingActive) {
      var nodes = [E('h3', {}, '已登记摄像头')];
      if (latestRemoval && latestRemoval.phase !== 'idle') nodes.push(E('p', {}, [
        operationTitle(latestRemoval) + '：' + operationPhase(latestRemoval) + ' ',
        E('button', { 'class': 'cbi-button', click: unpair({ serial: latestRemoval.serial, slot: latestRemoval.slot }, true) }, '查看操作进度')
      ]));
      if (!devices) nodes.push(E('p', {}, '暂时无法读取摄像头列表，正在重试。'));
      else if (!devices.length) nodes.push(E('p', {}, '尚未登记摄像头，请先打开配对窗口添加。'));
      else {
        var states = { pair_offered: '正在配对', pair_pending: '等待确认', waking: '正在唤醒',
          sleep_requested: '正在休眠', receiving_video: '正在传输画面', awake: '已唤醒',
          sleep_observed: '休眠', keepalive_connected: '保活连接中', recovering_keepalive: '正在恢复连接', offline: '离线' };
        nodes.push(E('table', { 'class': 'table cbi-section-table' }, [
          E('thead', {}, E('tr', { 'class': 'tr' }, ['摄像头', 'SN', '状态', '操作'].map(function(title) {
            return E('th', { 'class': 'th', scope: 'col' }, title);
          }))),
          E('tbody', {}, devices.map(function(device) {
            var busy = device.recording && (device.recording.active || device.recording.finishing);
            return E('tr', { 'class': 'tr' }, [
              E('td', { 'class': 'td', 'data-title': '摄像头', style: 'overflow-wrap: anywhere' }, [
                E('span', {}, (device.profile || {}).name || '摄像头 ' + (device.slot + 1)), ' ',
                E('button', { 'class': 'cbi-button',
                  disabled: (m.readonly || !device.paired || (latestRemoval && latestRemoval.slot === device.slot && latestRemoval.serial === device.serial &&
                    (latestRemoval.active || latestRemoval.phase === 'result_unknown'))) || null,
                  click: renameCamera(device) }, '重命名')]),
              E('td', { 'class': 'td', 'data-title': 'SN', style: 'overflow-wrap: anywhere' }, device.serial),
              E('td', { 'class': 'td', 'data-title': '状态' }, busy ? '正在录像' : (states[device.state] || device.state || '未知')),
              E('td', { 'class': 'td', 'data-title': '操作' }, [E('button', {
                'class': 'cbi-button',
                disabled: (m.readonly || !device.paired || busy || pairingActive || (latestRemoval && latestRemoval.active)) || null,
                title: busy ? '请等待当前事件短片写入完成' : (pairingActive ? '请先结束当前配对窗口' : null),
                click: unpair(device, false, true)
              }, '恢复出厂设置'), ' ', E('button', {
                'class': 'cbi-button cbi-button-negative',
                disabled: (m.readonly || !device.paired || busy || pairingActive || (latestRemoval && latestRemoval.active)) || null,
                title: busy ? '请等待当前事件短片写入完成' : (pairingActive ? '请先结束当前配对窗口' : null),
                click: unpair(device)
              }, '删除摄像头')])
            ]);
          }))
        ]));
      }
      cameraList.replaceChildren.apply(cameraList, nodes);
    }
    var removalPhases = { waiting_camera: '正在唤醒摄像头', restoring_defaults: '正在恢复出厂配置',
      clearing_pairing: '正在清除摄像头配对', committing: '正在移除基站登记', completed: '删除完成',
      failed: '删除未完成', result_unknown: '摄像头操作结果未确认' };
    function drawRemoval() {
      if (!removalDialog) return;
      var d = removalDialog, p = d.operation, reset = isReset(d), nodes = [E('p', {}, '摄像头 SN：' + d.serial)];
      if (d.requesting) nodes.push(E('p', {}, reset ? '正在提交恢复请求…' : '正在提交删除请求…'));
      else if (p) {
        nodes.push(E('p', { role: 'status' }, operationPhase(p)));
        if (p.active) nodes.push(E('p', {}, '请保持摄像头开机并靠近基站。可以关闭对话框，操作继续在基站执行。'));
        else if (p.phase === 'completed' && reset) nodes.push(E('p', {}, '已收到摄像头恢复参数的响应，已清理基站保存的摄像头参数覆盖。配对、摄像头名称和基站布防计划保留；实际效果请核对摄像头画面与设置报告。'));
        else if (p.phase === 'completed') nodes.push(E('p', {}, p.operator_confirmed ?
          '根据你对摄像头恢复出厂的确认，已移除基站登记。' : '已收到摄像头恢复配置及清除配对的响应，基站登记已移除。'));
        else nodes.push(E('p', { role: 'alert' }, (reset ? '恢复结果未确认，配对登记保留。请检查摄像头状态后重新操作。原因：' : '基站登记尚未移除。请检查摄像头状态后重新确认操作。原因：') + (p.reason || '未知')));
        if (p.phase === 'result_unknown' && !reset && !m.readonly) nodes.push(E('button', {
          'class': 'cbi-button cbi-button-negative', click: confirmPhysicalReset(p)
        }, '已确认摄像头恢复出厂，完成删除'));
      }
      if (d.error) nodes.push(E('p', { role: 'alert' }, d.error));
      if (d.unavailable) nodes.push(E('p', {}, '暂时无法读取进度，正在重试；不会自动重发删除请求。'));
      nodes.push(E('p', {}, '历史录像、索引和解密密钥保留；录像仍按存储留存策略管理。'));
      d.body.replaceChildren.apply(d.body, nodes);
      d.close.disabled = !!d.requesting;
    }
    function confirmPhysicalReset(operation) {
      return ui.createHandlerFn(this, function() {
        removalDialog = null;
        var body = E('div', {}, [E('p', {}, '摄像头 SN：' + operation.serial),
          E('p', {}, '仅在这台摄像头已语音或指示灯确认恢复出厂，或你已完成其物理重置时使用。仅凭摄像头离线不能确认恢复出厂。'),
          E('p', {}, '确认后只完成基站登记清理，保留历史录像；不会再次向摄像头发送重置命令。')]);
        var close = E('button', { 'class': 'cbi-button', click: function() { ui.hideModal(); } }, '返回');
        var confirm = E('button', { 'class': 'cbi-button cbi-button-negative', click: ui.createHandlerFn(this, function() {
          confirm.disabled = true; close.disabled = true;
          return fs.exec('/usr/libexec/vava-agent-unpair', [String(operation.slot), operation.serial, 'confirm', operation.id])
            .then(function(result) {
              var reply; try { reply = JSON.parse(result.stdout); } catch (e) {}
              if (result.code !== 0 || !reply || !reply.ok) throw new Error((reply && reply.error) || '结果未确认，请重新查询删除进度。');
              return refresh().then(refresh).then(unpair({ serial: operation.serial, slot: operation.slot }, true));
            }).catch(function(err) { body.replaceChildren(E('p', { role: 'alert' }, err.message)); confirm.hidden = true; })
            .finally(function() { close.disabled = false; });
        }) }, '摄像头已恢复出厂，确认完成删除');
        ui.showModal('确认摄像头已恢复出厂', [body, E('div', { 'class': 'right' }, [close, ' ', confirm])]);
      });
    }
    function receiveRemoval(p) {
      latestRemoval = p;
      if (!removalDialog || removalDialog.requesting || !p || !removalDialog.id || removalDialog.id !== p.id) return;
      removalDialog.operation = p; removalDialog.unavailable = false; drawRemoval();
    }
    function unpair(device, progressOnly, defaultsOnly) {
      return ui.createHandlerFn(this, function() {
        var d = { serial: device.serial, action: defaultsOnly ? 'reset' : 'remove', body: E('div') };
        d.close = E('button', { 'class': 'cbi-button cbi-button-neutral', click: function() { removalDialog = null; ui.hideModal(); } }, '返回');
        var confirm = E('button', { 'class': 'cbi-button cbi-button-negative', click: ui.createHandlerFn(this, function() {
          d.requesting = true; confirm.hidden = true; drawRemoval();
          return fs.exec(defaultsOnly ? '/usr/libexec/vava-agent-reset' : '/usr/libexec/vava-agent-unpair', [String(device.slot), device.serial]).then(function(result) {
            var reply; try { reply = JSON.parse(result.stdout); } catch (e) {}
            if (result.code !== 0 || !reply || !reply.ok || !reply.operation_id) {
              var errors = { camera_identity_changed: '摄像头登记已变化，请关闭对话框并重新选择。',
                recording_must_finish_before_remove: '摄像头正在录制事件短片，请等待写入完成后再试。',
                removal_state_unavailable_or_unresolved: '还有未确认的摄像头操作，请先核对其结果，并重试原操作。',
                camera_busy_stop_live_or_settings: '请先停止实时观看，等待配置下发完成。',
                factory_reset_firmware_not_verified: '这台摄像头的固件尚未验证恢复出厂命令。' };
              throw new Error(errors[reply && reply.error] || (reply && reply.error) || result.stderr || '未收到有效结果，请检查基站进度。');
            }
            d.id = reply.operation_id; d.operation = reply.removal;
          }).catch(function(err) {
            d.error = (defaultsOnly ? '恢复请求未确认：' : '删除请求未确认：') + err.message;
          }).then(function() {
            d.requesting = false; d.close.textContent = '关闭'; drawRemoval();
            return refresh().then(refresh);
          });
        }) }, defaultsOnly ? '确认恢复出厂设置' : '恢复出厂并删除');
        removalDialog = d;
        if (latestRemoval && (progressOnly || latestRemoval.active)) {
          d.id = latestRemoval.id; d.serial = latestRemoval.serial; d.action = latestRemoval.action; d.operation = latestRemoval; confirm.hidden = true; drawRemoval();
        } else if (defaultsOnly) {
          d.body.replaceChildren(E('p', {}, '确认恢复这台摄像头的出厂参数？'),
            E('p', {}, '摄像头 SN：' + device.serial),
            E('p', {}, '恢复摄像头参数并清理基站保存的参数覆盖。保留配对、摄像头名称和基站布防计划；布防仍由基站管理。'),
            E('p', {}, '请保持摄像头开机；正在观看时请先停止。历史录像、索引和解密密钥保留。'));
        } else {
          d.body.replaceChildren(E('p', {}, '确认恢复这台摄像头的出厂设置，并从基站删除？'),
            E('p', {}, '摄像头 SN：' + device.serial),
            E('p', {}, '摄像头设置和配对信息将被清除，之后需重新添加。请保持摄像头开机；正在观看时请先停止。'),
            E('p', {}, '历史录像、索引和解密密钥保留；删除摄像头不会删除这些片段，录像仍按存储留存策略管理。'));
        }
        ui.showModal(operationTitle(d), [d.body, E('div', { 'class': 'right' }, [d.close, ' ', confirm])]);
      });
    }
    function drawPairing() {
      if (!dialog) return;
      var d = dialog, p = d.pairing, nodes = [];
      if (d.requesting) nodes.push(E('p', {}, '正在请求配对窗口…'));
      else if (p && !p.active) {
        var messages = { completed: '摄像头配对成功。', cancelled: '配对窗口已关闭。',
          expired: '配对窗口已超时，请重新打开后再试。', interrupted: '配对已中断，请重新打开后再试。',
          failed: '配对失败，请检查摄像头和基站网络后重试。', idle: '当前没有配对任务。' };
        nodes.push(E('p', { role: 'status' }, messages[p.phase] || '配对窗口已结束。'));
        if (p.phase === 'completed' && p.serial) nodes.push(E('p', {}, '摄像头 SN：' + p.serial));
      } else if (d.accepted) {
        nodes.push(E('p', {}, d.resumed ? '配对窗口已开启，请让摄像头进入配对模式。' :
          '已请求 ' + d.seconds + ' 秒配对窗口，请让摄像头进入配对模式。'));
        if (!p) nodes.push(E('p', {}, '正在同步本次配对状态…'));
        else if (p.phase === 'opening_ap') nodes.push(E('p', {}, '正在开启临时热点，热点就绪后开始倒计时。'));
        else {
          var seconds = Math.max(0, Math.ceil((d.deadline - performance.now()) / 1000));
          nodes.push(E('p', { 'class': 'cbi-section', 'aria-live': 'off', style: 'font-size: 1.6em; font-weight: bold' },
            seconds ? '剩余 ' + seconds + ' 秒' : '正在确认配对结果…'));
          nodes.push(E('p', {}, '配对阶段：' + (phases[p.phase] || p.phase)));
        }
      }
      if (d.closing) nodes.push(E('p', {}, '正在关闭配对窗口…'));
      if (d.error) nodes.push(E('p', { role: 'alert' }, d.error));
      if (d.unavailable) nodes.push(E('p', { role: 'status' }, '暂时无法读取基站状态，正在重试。'));
      d.body.replaceChildren.apply(d.body, nodes);
      d.button.textContent = d.accepted && (!p || p.active) ? '取消配对' : '关闭';
      d.button.disabled = !!(d.requesting || d.closing);
    }
    function receivePairing(p, at) {
      latestPairing = p; latestAt = at;
      // A status request started before pair-open may still return the previous operation.
      if (!dialog || dialog.requesting || (dialog.id && dialog.id !== p.id)) return;
      dialog.id = p.id; dialog.pairing = p; dialog.unavailable = false;
      dialog.deadline = at + (Number(p.remaining_ms) || 0);
      drawPairing();
    }
    function refresh() {
      if (refreshing) return refreshing;
      lastRefresh = performance.now();
      var epoch = pairingEpoch;
      refreshing = Promise.all([serviceList('vava-agent'), fs.exec('/usr/libexec/vava-agent-status', [])])
        .then(function(results) {
          var instances = (results[0]['vava-agent'] || {}).instances || {};
          var running = Object.keys(instances).some(function(k) { return instances[k].running; });
          var data = null; try { if (results[1] && results[1].code === 0) data = JSON.parse(results[1].stdout); } catch (e) {}
          var nodes = [E('h3', {}, 'Agent 状态'), E('p', {}, running ? '服务运行中' : '服务未运行')];
          if (data && data.ok) {
            nodes.push(E('p', {}, '已登记摄像头：' + data.devices.length + ' / 4 · 本地录像：' + data.storage.records + ' 段'));
            nodes.push(E('p', {}, '时钟：' + (data.clock_valid ? '已同步' : '等待 NTP') + ' · 存储：' + (data.storage.available ? '可用' : '暂不可用')));
            var pairing = data.pairing || {};
            receiveRemoval(data.removal || null);
            drawCameras(data.devices, pairing.active);
            if (latestRemoval && latestRemoval.phase !== 'idle') nodes.push(E('p', {}, operationTitle(latestRemoval) + '：' +
              operationPhase(latestRemoval) + ' · ' + latestRemoval.serial));
            nodes.push(E('p', {}, '配对阶段：' + (phases[pairing.phase] || pairing.phase || '空闲') +
              (pairing.active && pairing.phase !== 'opening_ap' ? ' · 剩余 ' + Math.ceil((pairing.remaining_ms || 0) / 1000) + ' 秒' : '')));
            if (epoch === pairingEpoch) receivePairing(pairing, performance.now());
          } else {
            drawCameras(null);
            if (dialog) { dialog.unavailable = true; drawPairing(); }
            if (removalDialog) { removalDialog.unavailable = true; drawRemoval(); }
          }
          var url = uci.get('vava-agent', 'main', 'management_url');
          if (url && managerURL(url)) nodes.push(E('a', { href: url, target: '_blank', rel: 'noopener noreferrer', 'class': 'cbi-button cbi-button-action' }, '打开管理平台'));
          status.replaceChildren.apply(status, nodes);
        }).catch(function() {
          status.replaceChildren(E('h3', {}, 'Agent 状态'), E('p', {}, '暂时无法读取基站状态，正在重试。'));
          drawCameras(null);
          if (dialog) { dialog.unavailable = true; drawPairing(); }
            if (removalDialog) { removalDialog.unavailable = true; drawRemoval(); }
        }).finally(function() { refreshing = null; });
      return refreshing;
    }
    function pairCommand(action) {
      return fs.exec('/usr/libexec/vava-agent-pair', [action]).then(function(result) {
        var reply; try { reply = JSON.parse(result.stdout); } catch (e) {}
        if (result.code !== 0 || !reply || !reply.ok) {
          var errors = { camera_slots_full: '已登记 4 台摄像头，请先移除不再使用的摄像头。',
            pair_operation_active: '已有配对任务正在进行，请稍后查看配对进度。',
            pair_ap_busy_or_helper_failed: '临时热点开启失败，请检查基站网络。' };
          throw new Error(errors[reply && reply.error] || (reply && reply.error) || result.stderr || 'Agent 未接受配对请求');
        }
        return reply;
      });
    }
    function closePairing(d) {
      d.closing = true; d.error = null; drawPairing();
      return pairCommand('close').then(function() {
        pairingEpoch++;
        // Keep a confirmed success visible if pairing finished while cancellation was requested.
        if (!d.pairing || d.pairing.active) d.pairing = { id: d.id, phase: 'cancelled', active: false };
        latestPairing = d.pairing; latestAt = performance.now();
        d.accepted = false;
        if (dialog === d && d.pairing.phase === 'cancelled') { dialog = null; ui.hideModal(); }
        // Wait out any pre-cancellation poll, then fetch the current backend state.
        return refresh().then(refresh);
      }).catch(function(err) { d.error = '关闭失败：' + err.message; })
        .finally(function() { d.closing = false; drawPairing(); });
    }
    function showPairing() {
      var d = { seconds: 120, body: E('div'), accepted: false };
      d.button = E('button', { 'class': 'cbi-button cbi-button-neutral', click: ui.createHandlerFn(this, function() {
        if (d.accepted && (!d.pairing || d.pairing.active)) return closePairing(d);
        dialog = null; ui.hideModal();
      }) }, '关闭');
      dialog = d;
      ui.showModal('摄像头配对', [d.body, E('div', { 'class': 'right' }, [d.button])]);
      return d;
    }
    function pairing() {
      return ui.createHandlerFn(this, function() {
        var d = showPairing();
        if (latestPairing && latestPairing.active) {
          d.id = latestPairing.id; d.accepted = true; d.resumed = true;
          receivePairing(latestPairing, latestAt);
          return refresh();
        }
        d.requesting = true; drawPairing();
        return pairCommand('open').then(function(reply) {
          pairingEpoch++;
          d.requesting = false; d.accepted = true;
          d.id = reply.operation_id; d.seconds = reply.window_seconds || 120;
          drawPairing();
          return refresh();
        }).catch(function(err) { d.requesting = false; d.error = '开启失败：' + err.message; drawPairing(); });
      });
    }
    // DummyValue panels participate in LuCI tabs without writing display-only state to UCI.
    o = s.taboption('pairing', form.DummyValue, '_pairing');
    o.renderWidget = function() {
      return E('div', {}, [E('h3', {}, '添加摄像头'),
        E('p', {}, '将摄像头放在基站附近，打开配对窗口后，让摄像头进入配对模式。配对成功后，请到管理平台选择摄像头工作模式。'),
        E('button', { 'class': 'cbi-button cbi-button-action', disabled: m.readonly || null, click: pairing() }, '打开配对窗口'),
        cameraList]);
    };
    o = s.taboption('status', form.DummyValue, '_status');
    o.renderWidget = function() { return status; };
    return m.render().then(function(node) {
      poll.add(function() {
        drawPairing();
        if (performance.now() - lastRefresh >= (dialog || removalDialog || renameDialog || (latestRemoval && latestRemoval.active) ? 2000 : 5000)) return refresh();
      }, 1);
      refresh();
      return node;
    });
  }
});

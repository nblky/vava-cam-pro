#!/usr/bin/env python3
"""Exercise OpenWrt scripts in a temporary command/filesystem mock, never real UCI/network."""
import json
import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
checks = []
with tempfile.TemporaryDirectory(prefix='vava-openwrt-', dir='/tmp') as td:
    base = Path(td)
    bin_dir = base/'bin'; bin_dir.mkdir()
    store = base/'uci.json'; store.write_text(json.dumps({'wireless.radio0': 'wifi-device'}))
    log = base/'commands.jsonl'; log.touch()
    stub = bin_dir/'uci'
    stub.write_text('''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
p=Path(os.environ['VAVA_MOCK_STORE']); data=json.loads(p.read_text()); args=sys.argv[1:]
if args and args[0]=='-q': args=args[1:]
if args[0]=='get':
    if args[1] not in data: sys.exit(1)
    print(data[args[1]])
else:
    with open(os.environ['VAVA_MOCK_LOG'],'a') as f: f.write(json.dumps(args)+'\\n')
    if args[0] in ('set','add_list'):
        k,v=args[1].split('=',1); data[k]=v; p.write_text(json.dumps(data))
'''); stub.chmod(0o700)
    wifi = bin_dir/'wifi'
    wifi.write_text('#!/bin/sh\nprintf \'["wifi","%s","%s"]\\n\' "$1" "$2" >> "$VAVA_MOCK_LOG"\n')
    wifi.chmod(0o700)
    env = {**os.environ, 'VAVA_MOCK_STORE':str(store), 'VAVA_MOCK_LOG':str(log)}
    mock_path = str(bin_dir)+':'+os.environ['PATH']
    paths = {}
    for name in ('vava-network','vava-ap','vava-agent.init','vava-ntp','vava-agent-status','vava-agent-pair','vava-agent-unpair','vava-agent-reset','vava-agent-name'):
        original = (ROOT/'vava-station/openwrt/vava-agent/files'/name).read_text()
        subprocess.run(['sh','-n',str(ROOT/'vava-station/openwrt/vava-agent/files'/name)],check=True)
        # Only the test copy changes. Production scripts keep fixed trusted paths.
        body = original.replace('PATH=/usr/sbin:/usr/bin:/sbin:/bin','PATH='+shlex.quote(mock_path))
        body = body.replace('/sbin/wifi',str(wifi)).replace('/etc/vava',str(base/'etc/vava'))
        body = body.replace('/etc/config',str(base/'etc/config'))
        body = body.replace('/proc/mounts',str(base/'mounts'))
        body = body.replace('/tmp/vava-clock-synced',str(base/'clock-synced'))
        body = body.replace('/usr/bin/vavactl',str(bin_dir/'vavactl'))
        target=base/name; target.write_text(body); paths[name]=target
    (base/'etc/config').mkdir(parents=True); (base/'etc/config/wireless').touch()
    def run(name,*args,data=None,ok=True):
        p=subprocess.run(['sh',str(paths[name]),*args],input=data,text=True,env=env,capture_output=True,timeout=5)
        assert (p.returncode==0)==ok,(name,args,p.returncode,p.stdout,p.stderr)
        return p
    run('vava-network','setup','radio0oops',ok=False)
    assert not log.read_text(); checks.append('invalid radio suffix rejected before writes')
    (base/'etc/vava').mkdir(); (base/'etc/vava/wifi-key').write_text('preserve')
    run('vava-network','setup','radio0',ok=False)
    assert (base/'etc/vava/wifi-key').read_text()=='preserve' and not log.read_text()
    (base/'etc/vava/wifi-key').unlink(); checks.append('existing credentials preserved')
    entropy=bin_dir/'hexdump'; entropy.write_text('#!/bin/sh\nexit 0\n'); entropy.chmod(0o700)
    run('vava-network','setup','radio0',ok=False)
    assert not log.read_text() and not (base/'etc/vava/wifi-key').exists()
    entropy.unlink(); checks.append('empty entropy output rejected before credentials and UCI writes')
    run('vava-network','setup','radio0')
    values=json.loads(store.read_text())
    assert values['network.vava.ipaddr']=='172.26.168.1'
    assert values['network.vava.netmask']=='255.255.0.0'
    assert values['wireless.radio0.disabled']=='0'
    assert values['firewall.vava.forward']=='REJECT' and values['firewall.vava.input']=='REJECT'
    assert values['wireless.vava_pair.disabled']=='1'
    assert values['wireless.vava_camera.ssid'].startswith('VAVA-Local-')
    for name,size in [('wifi-key',24)]:
        f=base/'etc/vava'/name
        assert len(f.read_text())==size and f.stat().st_mode&0o777==0o600
    checks.append('dedicated AP DHCP firewall and private credentials generated')
    before=log.read_text(); run('vava-network','setup','radio0',ok=False)
    assert log.read_text()==before; checks.append('existing sections rejected without changes')
    run('vava-ap','open',data='VAPair-1234abcd\nvacd12ab\n')
    assert json.loads(store.read_text())['wireless.vava_pair.disabled']=='0'
    after=log.read_text()[len(before):]
    assert 'commit' not in after and '["wifi","reload","radio0"]' in after
    run('vava-ap','close')
    assert json.loads(store.read_text())['wireless.vava_pair.disabled']=='1'
    run('vava-ap','open',data='VAPair-AbCdEfGh\nvaGhAbEf\n')
    assert json.loads(store.read_text())['wireless.vava_pair.ssid']=='VAPair-AbCdEfGh'
    run('vava-ap','close')
    before=log.read_text(); run('vava-ap','open',data='VAPair-AbCdEfGh\nvaGhAb00\n',ok=False)
    assert log.read_text()==before
    before=log.read_text(); run('vava-ap','open',data='VAPair-1234abcd\ninvalid!\n',ok=False)
    assert log.read_text()==before; checks.append('pair open close and credential validation without flash commit')
    # Execute the renamed procd service in a sandbox with explicit config and no real services.
    record_mount=base/'record-mount';record_mount.mkdir()
    keyring=base/'etc/vava/record-keys';keyring.mkdir();(keyring/'active').write_text('synthetic')
    procd=base/'procd.log';procd.touch();env['VAVA_MOCK_PROCD']=str(procd)
    env['VAVA_MOCK_DIR']=str(base)
    wrapper=base/'init-test'
    wrapper.write_text('''#!/bin/sh
config_load() { [ "$1" = vava-agent ]; }
config_get_bool() { case "$1" in local_only) eval "$1=0";; *) eval "$1=1";; esac; }
config_get() {
 case "$3" in
 state_dir) eval "$1=$VAVA_MOCK_DIR/state";;
 storage_mount) eval "$1=$VAVA_MOCK_DIR/record-mount";;
 record_dir) eval "$1=$VAVA_MOCK_DIR/record-mount/recordings";;
 event_dir) eval "$1=$VAVA_MOCK_DIR/record-mount/events";;
 pairing_serial|archive_serial) eval "$1=0123456789ABCDEFG12345678";;
 *) eval "$1=\\\"$4\\\"";;
 esac
}
logger() { :; }
procd_open_instance() { :; }
procd_set_param() { printf '%s\\n' "$*" >> "$VAVA_MOCK_PROCD"; }
procd_append_param() { procd_set_param "$@"; }
procd_close_instance() { :; }
. '''+shlex.quote(str(paths['vava-agent.init']))+'''
start_service
''')
    def service(body, ok):
        target=base/'service-test';target.write_text(body)
        result=subprocess.run(['sh',str(target)],env=env,capture_output=True,text=True,timeout=5)
        assert (result.returncode==0)==ok, result.stderr
    remote=wrapper.read_text();service(remote,False)
    assert not procd.read_text()
    local=remote.replace('local_only) eval "$1=0"','local_only) eval "$1=1"')
    service(local,True)
    assert 'command /usr/bin/vava-agent ' in procd.read_text()
    assert '--server-host' not in procd.read_text() and '--http' not in procd.read_text()
    assert '--pairing-serial 0123456789ABCDEFG12345678' in procd.read_text()
    assert not (record_mount/'recordings').exists(), 'startup must not create recording directories on an unmounted path'
    checks.append('renamed procd service rejects absent remote credentials and allows explicit standalone startup without card I/O')
    before=procd.read_text()
    bad=local.replace('record_dir) eval "$1=$VAVA_MOCK_DIR/record-mount/recordings"','record_dir) eval "$1=$VAVA_MOCK_DIR/outside"')
    service(bad,False);assert procd.read_text()==before
    checks.append('procd rejects recording paths outside the configured mount')
    ctl=bin_dir/'vavactl'
    ctl.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$VAVA_MOCK_PROCD\"\nprintf '{\"ok\":true}\\n'\n")
    ctl.chmod(0o700)
    env['PATH']=mock_path
    for action,stratum,expected in [('stratum','0',False),('stratum','16',False),('other','2',False),('stratum','2',True)]:
        env.update(ACTION=action,stratum=stratum);run('vava-ntp')
        assert (base/'clock-synced').exists()==expected
    assert 'clock-valid 1' in procd.read_text()
    checks.append('NTP hotplug accepts only valid synchronization and reads vava-agent configuration')
    run('vava-agent-status');run('vava-agent-status','unexpected',ok=False)
    run('vava-agent-pair','open');run('vava-agent-pair','close');run('vava-agent-pair','forget',ok=False)
    assert 'pair-open 120' in procd.read_text()
    checks.append('LuCI helpers only permit status and bounded pairing operations')
    before=procd.read_text()
    for args in [(),('0',),('4','CAMERA-A'),('-1','CAMERA-A'),('00','CAMERA-A'),
                 ('0',''),('0','a'*32),('0','CAMERA A'),('0','CAMERA\nA'),
                 ('0','$(whoami)'),('0','A','forget'),('0','A','bad','0'*16),('0','A','confirm','bad')]:
        run('vava-agent-unpair',*args,ok=False)
    assert procd.read_text()==before
    run('vava-agent-unpair','2','CAMERA-A_2.test:1')
    assert procd.read_text()[len(before):].strip().endswith('if-serial CAMERA-A_2.test:1 remove 2')
    run('vava-agent-unpair','2','CAMERA-A_2.test:1','confirm','1234567890abcdef')
    assert procd.read_text().strip().endswith('if-serial CAMERA-A_2.test:1 remove-confirm 2 1234567890abcdef')
    acl=json.loads((ROOT/'vava-station/openwrt/luci-app-vava-agent/root/usr/share/rpcd/acl.d/luci-app-vava-agent.json').read_text())['luci-app-vava-agent']
    helper='/usr/libexec/vava-agent-unpair'
    assert helper not in acl['read']['file'] and acl['write']['file'][helper]==['exec']
    checks.append('unpair helper requires exact slot and serial, rejects injected arguments, and is write-ACL only')
    before=procd.read_text()
    for args in [(),('0',),('4','CAMERA-A'),('00','CAMERA-A'),('0',''),('0','a'*32),
                 ('0','CAMERA\nA'),('0','$(whoami)'),('0','A','confirm','0'*16)]:
        run('vava-agent-reset',*args,ok=False)
    assert procd.read_text()==before
    run('vava-agent-reset','2','CAMERA-A_2.test:1')
    assert procd.read_text().strip().endswith('if-serial CAMERA-A_2.test:1 reset 2')
    helper='/usr/libexec/vava-agent-reset'
    assert helper not in acl['read']['file'] and acl['write']['file'][helper]==['exec']
    checks.append('independent reset helper is identity guarded and write-ACL only, without a manual unpair bypass')
    before=procd.read_text()
    for args in [(),('0','CAMERA-A','0'),('4','CAMERA-A','0','门口'),('0','CAMERA\nA','0','门口'),
                 ('0','$(whoami)','0','门口'),('0','CAMERA-A','-1','门口'),('0','CAMERA-A','00','门口'),
                 ('0','CAMERA-A','4294967296','门口'),('0','CAMERA-A','0\n','门口'),('0','CAMERA-A','0','门'*33)]:
        run('vava-agent-name',*args,ok=False)
    assert procd.read_text()==before
    run('vava-agent-name','2','CAMERA-A_2.test:1','12','门口 📷')
    assert procd.read_text().strip().endswith('if-serial CAMERA-A_2.test:1 if-revision 12 name 2 门口 📷')
    run('vava-agent-name','2','CAMERA-A_2.test:1','13','')
    assert procd.read_text().strip().endswith('if-serial CAMERA-A_2.test:1 if-revision 13 name 2')
    helper='/usr/libexec/vava-agent-name'
    assert helper not in acl['read']['file'] and acl['write']['file'][helper]==['exec']
    checks.append('name helper requires exact slot, serial and revision, passes UTF-8/empty names as one argument, and is write-ACL only')
print(json.dumps({'ok':True,'host_mock_only':True,'checks':checks},ensure_ascii=False,indent=2))

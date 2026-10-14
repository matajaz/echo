import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('maintenance', Path(__file__).with_name('maintenance.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class ProtocolTests(unittest.TestCase):
    def image(self):
        data = bytearray(300)
        data[0] = 0xe9
        struct.pack_into('<I', data, 32, 0xabcd5432)
        data[176:208] = bytes(range(32))
        return bytes(data)

    def test_common_copy_matches(self):
        root = Path(__file__).resolve().parents[2]
        other = root / ('Vegena' if Path(__file__).resolve().parents[1].name == 'echo' else 'echo') / 'tools' / 'maintenance.py'
        if other.exists(): self.assertEqual(Path(__file__).with_name('maintenance.py').read_bytes(), other.read_bytes())

    def test_log_current_and_legacy(self):
        self.assertEqual(m.parse_log('boot: 7\nid: 9\nhello\n'), (7, 9, 'hello'))
        self.assertEqual(m.parse_log('id: 9\nhello\n'), (None, 9, 'hello'))
        self.assertEqual(m.parse_log('boot: 7\nid: 9\n'), (7, 9, ''))
        for bad in ('boot: 7\nno cursor', 'id: -1', 'id: 4294967296', 'boot: 0\nid: 1'):
            with self.assertRaises(ValueError): m.parse_log(bad)

    def test_descriptor_identity(self):
        self.assertEqual(m.image_identity(self.image()), bytes(range(32)).hex())
        for bad in (b'', b'junk'*100, self.image()[:200]):
            with self.assertRaises(ValueError): m.image_identity(bad)

    def test_exact_image_and_new_boot_required(self):
        expected = 'a'*64
        info = dict(boot_id=2, app_sha_kind='elf_sha256', app_sha=expected, img_state='valid', ota_in_progress=False)
        self.assertTrue(m.accepted(info, expected, 1))
        for change in (dict(boot_id=1),dict(boot_id=0),dict(boot_id=True),dict(app_sha='b'*64),dict(app_sha=expected[:16]),
                       dict(app_sha_kind='bin_sha256'),dict(img_state='pending_verify'),dict(img_state='unknown'),dict(ota_in_progress=True)):
            self.assertFalse(m.accepted(dict(info, **change), expected, 1))
        self.assertFalse(m.accepted({}, expected, 1))

    def run_deploy(self, reject=False, matching=True, wrong_served=False, unstable=False):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp); image=self.image(); (tmp/'app.bin').write_bytes(image)
            (tmp/'secrets.h').write_text('#define OTA_URL_PREFIX "http://firmware/echo/"\n#define OTA_KEY "test-key"\n')
            args=SimpleNamespace(bin=str(tmp/'app.bin'),project='echo',secrets=str(tmp/'secrets.h'),prefix_define='OTA_URL_PREFIX',
                key_define='OTA_KEY',key_env=None,board='board',host='server',user='user',directory='~/firmware',ca=None,
                timeout=12,interval=1,soak=2,ledger=str(tmp/'ledger.jsonl'))
            expected=m.image_identity(image)
            info=dict(boot_id=11,app_sha_kind='elf_sha256',app_sha=expected if matching else 'f'*64,img_state='valid',ota_in_progress=False)
            replies=[dict(boot_id=10),dict(info,img_state='pending_verify'),info,dict(info,img_state='pending_verify'),info,info]
            clock=[0]
            def now(): clock[0]+=1; return clock[0]
            def fetch(url,**kwargs):
                if '/ota?' in url:
                    if reject: raise urllib.error.HTTPError(url,403,'forbidden',None,None)
                    return b'OTA queued'
                return b'wrong' if wrong_served else image
            next_boot=[20]
            def read_info(board):
                value=replies.pop(0) if replies else info
                if unstable and value.get('boot_id')!=10:
                    next_boot[0]+=1
                    value=dict(value,boot_id=next_boot[0])
                return value
            with patch.object(m,'fetch',side_effect=fetch) as fetching, patch.object(m,'info_at',side_effect=read_info), \
                 patch.object(m.subprocess,'run') as remote, patch.object(m.time,'sleep'),patch.object(m.time,'monotonic',side_effect=now),contextlib.redirect_stdout(io.StringIO()):
                if reject or not matching or wrong_served or unstable:
                    with self.assertRaises((ValueError,urllib.error.HTTPError)): m.deploy(args)
                else: m.deploy(args)
                requests=[c.args[0] for c in fetching.call_args_list]
                self.assertIn(hashlib.sha256(image).hexdigest()+'.bin',requests[0])
                self.assertEqual(remote.call_count,4)
            record=json.loads((tmp/'ledger.jsonl').read_text())
            self.assertNotIn('test-key',json.dumps(record))
            return record

    def test_confirmed_deploy(self): self.assertEqual(self.run_deploy()['result'],'success')
    def test_trigger_rejection(self): self.assertEqual(self.run_deploy(reject=True)['result'],'failed')
    def test_wrong_image(self): self.assertEqual(self.run_deploy(matching=False)['result'],'failed')
    def test_boot_loop_never_passes_soak(self): self.assertEqual(self.run_deploy(unstable=True)['result'],'failed')
    def test_publication_mismatch(self): self.assertEqual(self.run_deploy(wrong_served=True)['result'],'failed')

    def test_reboot_with_higher_log_cursor(self):
        replies=[b'boot: 1\nid: 2\nfirst',b'boot: 2\nid: 10\npartial',b'boot: 2\nid: 10\nstartup']
        with patch.object(m,'fetch',side_effect=replies) as fetching,patch.object(m.time,'sleep',side_effect=[None,KeyboardInterrupt]),contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(KeyboardInterrupt): m.collect(SimpleNamespace(output='-',board='board',info_every=0,interval=1))
        self.assertEqual(out.getvalue(),'first\n--- board reboot / log reset ---\nstartup\n')
        self.assertTrue(fetching.call_args_list[-1].args[0].endswith('since=0'))

if __name__ == '__main__': unittest.main()

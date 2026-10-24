import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


CHILD = r'''
import atexit,io,json,logging,sys,time
from pathlib import Path
from rf3_gui.logging_config import configure_logging
base,index = Path(sys.argv[1]),sys.argv[2]
captured=io.StringIO()
sys.stderr=captured
atexit.register(lambda: sys.__stderr__.write('\n'.join(line for line in captured.getvalue().splitlines() if len(line)<1000)[-4000:]))
path=configure_logging()
assert configure_logging() == path
(base / ('ready'+index)).write_text('ready')
deadline=time.monotonic()+10
while not (base/'go').exists():
    assert time.monotonic()<deadline, 'barrier timed out'
    time.sleep(.01)
logger=logging.getLogger('rotation-test')
for sequence in range(3):
    logger.debug('instance-%s-record-%d %s',index,sequence,'x'*600000)
for handler in logging.getLogger().handlers: handler.flush()
diagnostics=captured.getvalue()
assert 'Logging error' not in diagnostics,diagnostics
assert 'PermissionError' not in diagnostics,diagnostics
print(json.dumps({'path':str(path),'records':3}))
'''


class MultiInstanceLogging(unittest.TestCase):
    def test_two_processes_rotate_without_collisions_or_lost_records(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            env = dict(os.environ, RF3_LOG_DIR=directory)
            children = [subprocess.Popen([sys.executable, '-c', CHILD, directory, str(index)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env) for index in range(2)]
            try:
                deadline = time.monotonic() + 10
                while not all((base / f'ready{i}').exists() for i in range(2)):
                    self.assertLess(time.monotonic(), deadline, 'children did not open their logs')
                    self.assertTrue(all(child.poll() is None for child in children))
                    time.sleep(.01)
                (base/'go').write_text('go')
                paths = []
                for child in children:
                    stdout, stderr = child.communicate(timeout=15)
                    self.assertEqual(child.returncode, 0, stderr)
                    paths.append(json.loads(stdout)['path'])
                self.assertNotEqual(paths[0], paths[1])
                for index, path in enumerate(paths):
                    records = ''.join(p.read_text(encoding='utf-8') for p in base.glob(Path(path).name+'*'))
                    for sequence in range(3):
                        self.assertEqual(records.count(f'instance-{index}-record-{sequence} '), 1)
            finally:
                for child in children:
                    if child.poll() is None:
                        child.kill()
                    child.wait(timeout=5)
                    for pipe in (child.stdout, child.stderr):
                        if pipe is not None:
                            pipe.close()


if __name__ == '__main__':
    unittest.main()

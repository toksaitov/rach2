"""Reproduce a null Marionette script timeout in the installed Firefox."""
import contextlib,json,os,signal,socket,subprocess,sys,tempfile,time
from pathlib import Path
sys.path.insert(0,'/tmp')
from protocol import Marionette
child=client=None
with tempfile.TemporaryDirectory(prefix='null-timeout-',dir='/tmp') as tmp:
 try:
  with socket.socket() as s:
   s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
  Path(tmp,'user.js').write_text('user_pref("marionette.port",%d);\n' % port)
  child=subprocess.Popen(['firefox-esr','--headless','--no-remote','--marionette','--profile',tmp],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
  while client is None:
   assert child.poll() is None
   try: client=Marionette(port)
   except ConnectionRefusedError: time.sleep(.1)
  script={'script':'return new Promise(resolve => setTimeout(() => resolve(123), 10));','args':[],'sandbox':None,'newSandbox':True}
  before=client.call('WebDriver:ExecuteScript',script)
  client.call('WebDriver:SetTimeouts',{'script':None})
  bad=client.call('WebDriver:ExecuteScript',script)
  client.call('WebDriver:SetTimeouts',{'script':30000})
  after=client.call('WebDriver:ExecuteScript',script)
  result={'version':client.capabilities['browserVersion'],'before':before,'null_timeout':bad,'after':after}
  Path('/tmp/null-timeout.json').write_text(json.dumps(result,indent=2)+'\n')
  print(json.dumps(result),flush=True)
  assert before==after=={'value':123} and bad=={'value':None},result
 finally:
  if client: client.close()
  if child:
   with contextlib.suppress(ProcessLookupError): os.killpg(child.pid,signal.SIGTERM)
   try: child.wait(timeout=5)
   except subprocess.TimeoutExpired:
    os.killpg(child.pid,signal.SIGKILL);child.wait()

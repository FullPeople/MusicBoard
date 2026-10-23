"""Real, isolated installer/upgrade/uninstall acceptance without UI automation."""
from pathlib import Path
import argparse, ctypes, hashlib, json, mmap, os, struct, subprocess, time, uuid, winreg, zipfile

ROOT=Path(__file__).resolve().parents[1]
REGBASE='Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\'

def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def registration(identity):
    try:
        k=winreg.OpenKey(winreg.HKEY_CURRENT_USER,REGBASE+identity)
    except FileNotFoundError:return None
    with k:
        result={}
        for name in ['InstallLocation','UninstallString','DisplayVersion','DesktopShortcutEnabled']:
            try:result[name]=winreg.QueryValueEx(k,name)[0]
            except FileNotFoundError:result[name]=None
        return result

def argv_of(command):
    count=ctypes.c_int()
    parse=ctypes.windll.shell32.CommandLineToArgvW
    parse.restype=ctypes.POINTER(ctypes.c_wchar_p)
    args=parse(command,ctypes.byref(count))
    try:return [args[i] for i in range(count.value)]
    finally:ctypes.windll.kernel32.LocalFree(args)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--installer',type=Path,required=True)
    parser.add_argument('--payload',type=Path,required=True)
    parser.add_argument('--base',type=Path,required=True)
    parser.add_argument('--transaction-probe',type=Path,required=True)
    args=parser.parse_args()
    out=ROOT/'artifacts/wizard-acceptance'/uuid.uuid4().hex
    out.mkdir(parents=True)
    temporary=out/'temporary';temporary.mkdir()
    user=out/'profile';user.mkdir()
    library=out/'personal-library/BGM/fixture';library.mkdir(parents=True)
    (library/'preserve.wav').write_bytes(b'owned-test-fixture-not-user-audio')
    settings=user/'MusicBoard/settings.conf';settings.parent.mkdir()
    settings.write_text('language=en\nlibrary='+str(library.parent.parent)+'\nvolume=0.41\n',encoding='utf-8')
    keep={str(p):sha(p) for p in [settings,library/'preserve.wav']}
    env=dict(os.environ,TEMP=str(temporary),TMP=str(temporary),LOCALAPPDATA=str(user))
    checks={};commands=[];identities=[]
    def run(command):
        p=subprocess.run(list(map(str,command)),capture_output=True,env=env,timeout=180,creationflags=subprocess.CREATE_NO_WINDOW)
        commands.append({'executable':Path(command[0]).name,'exitCode':p.returncode,'output':(p.stdout+p.stderr).decode('utf-8',errors='replace')[-5000:]})
        return p
    def context(name):
        identity='MusicBoard.WizardAcceptance.'+uuid.uuid4().hex
        installed=out/name
        start=out/(name+'-start.lnk');desktop=out/(name+'-desktop.lnk')
        common=['--quiet','--no-launch','--install-root',installed,'--registry-subkey',identity,'--shortcut-path',start,'--desktop-shortcut-path',desktop]
        identities.append((identity,installed,start,desktop))
        return identity,installed,start,desktop,common
    def uninstall(identity,installed,start,desktop,common):
        r=registration(identity)
        if r: command=argv_of(r['UninstallString'])+['--quiet']
        else: command=[installed/'current/MusicBoardSetup.exe','--uninstall',*common]
        if Path(command[0]).resolve()!=(installed/'current/MusicBoardSetup.exe').resolve() or '--uninstall' not in command:
            raise RuntimeError('Refuse an unowned uninstall command')
        for flag,value in [('--install-root',installed),('--registry-subkey',identity),('--shortcut-path',start),('--desktop-shortcut-path',desktop)]:
            if command.count(flag)!=1 or command.index(flag)+1>=len(command) or str(command[command.index(flag)+1])!=str(value):
                raise RuntimeError('Refuse an uninstall command without exact isolated identity: '+flag)
        p=run(command)
        deadline=time.monotonic()+45
        while time.monotonic()<deadline and (installed.exists() or registration(identity) is not None):time.sleep(.05)
        return p.returncode==0 and not installed.exists() and not start.exists() and not desktop.exists() and registration(identity) is None
    payload=out/'payload';payload.mkdir()
    with zipfile.ZipFile(args.payload) as archive:archive.extractall(payload)
    identity,installed,start,desktop,common=context('fresh-custom-folder')
    failed=run([args.installer,*common,'--unknown-argument'])
    checks['UnknownWizardArgumentRejectedBeforeInstall']=failed.returncode!=0 and not installed.exists()
    begin=time.monotonic()
    installed_run=run([args.installer,*common,'--lang','en','--desktop-shortcut'])
    elapsed=round((time.monotonic()-begin)*1000)
    checks['FinalSingleExeInstallsAtCustomPath']=installed_run.returncode==0 and (installed/'current/MusicBoard.exe').is_file()
    if not checks['FinalSingleExeInstallsAtCustomPath']:raise RuntimeError('Final installer did not install: '+json.dumps(commands[-1]))
    version=run([installed/'current/MusicBoard.exe','--version'])
    checks['InstalledApplicationRunsAndReports036']=version.returncode==0 and b'0.3.6' in version.stdout
    checks['MainAndMediaPayloadExact']=all(sha(installed/'current'/p.relative_to(payload))==sha(p) for p in payload.rglob('*') if p.is_file())
    r=registration(identity)
    checks['RealPrivateRegistrationAndShortcuts']=r is not None and r['DisplayVersion']=='0.3.6' and Path(r['InstallLocation'])==installed and start.is_file() and desktop.is_file()
    checks['UninstallCommandKeepsCustomPathAndPrivateIdentity']=r is not None and all(str(v) in argv_of(r['UninstallString']) for v in [installed,identity,start,desktop])
    toggled=run([args.installer,*common,'--no-desktop-shortcut'])
    checks['DesktopShortcutCanBeDisabled']=toggled.returncode==0 and not desktop.exists() and registration(identity)['DesktopShortcutEnabled']==0
    repeated=run([args.installer,*common])
    checks['QuietSameVersionKeepsSavedDesktopChoice']=repeated.returncode==0 and not desktop.exists() and registration(identity)['DesktopShortcutEnabled']==0
    checks['InstalledUninstallerHonorsRegisteredCustomPath']=uninstall(identity,installed,start,desktop,common)

    old=out/'previous-public-package';old.mkdir()
    with zipfile.ZipFile(args.base) as archive:archive.extractall(old)
    old=old/'MusicBoard-0.3.5-windows-x64'
    identity,installed,start,desktop,common=context('upgrade-custom-folder')
    baseline=run([args.transaction_probe,old,installed])
    checks['Real035RuntimeTransactionBaseline']=baseline.returncode==0 and sha(installed/'current/MusicBoard.exe')==sha(old/'MusicBoard.exe')
    upgraded=run([args.installer,*common,'--no-desktop-shortcut'])
    checks['Actual035To036Upgrade']=upgraded.returncode==0 and sha(installed/'current/MusicBoard.exe')==sha(payload/'MusicBoard.exe') and registration(identity)['DisplayVersion']=='0.3.6'
    checks['UserLibraryAndSettingsPreservedAfterUpgrade']=all(sha(Path(p))==h for p,h in keep.items())
    checks['UpgradedInstalledUninstallerWorks']=uninstall(identity,installed,start,desktop,common)

    identity,installed,start,desktop,common=context('digest-rejection')
    normalized=str(installed.resolve()).replace('/','\\').rstrip('\\').lower()
    mutex_name='Local\\MusicBoard.Setup.Transaction.'+hashlib.sha256(normalized.encode('utf-16-le')).hexdigest()
    create_mutex=ctypes.windll.kernel32.CreateMutexW
    create_mutex.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_wchar_p]
    create_mutex.restype=ctypes.c_void_p
    handle=create_mutex(None,True,mutex_name)
    if not handle:raise OSError(ctypes.get_last_error(),'Cannot prepare isolated transaction lock')
    release_mutex=ctypes.windll.kernel32.ReleaseMutex;release_mutex.argtypes=[ctypes.c_void_p]
    close_handle=ctypes.windll.kernel32.CloseHandle;close_handle.argtypes=[ctypes.c_void_p]
    try:
        busy=run([payload/'MusicBoardSetup.exe',*common,'--package-root',payload])
        checks['ConcurrentInstallationRejectedBeforeChanges']=busy.returncode==170 and not installed.exists() and registration(identity) is None
    finally:release_mutex(handle);close_handle(handle)
    rejected=run([payload/'MusicBoardSetup.exe',*common,'--package-root',payload,'--manifest-sha256','0'*64])
    checks['WrongAuthenticatedManifestRejected']=rejected.returncode!=0 and not installed.exists() and registration(identity) is None
    unknown=run([payload/'MusicBoardSetup.exe',*common,'--package-root',payload,'--not-supported'])
    checks['UnknownBackendArgumentRejected']=unknown.returncode!=0 and not installed.exists()

    identity,installed,start,desktop,common=context('live-backend-progress')
    channel='Local\\MusicBoard.Setup.Progress.'+uuid.uuid4().hex
    shared=mmap.mmap(-1,32,tagname=channel)
    shared[:]=struct.pack('<iiiiqq',0x46545032,0,0,0,0,1)
    command=[payload/'MusicBoardSetup.exe',*common,'--package-root',payload,'--manifest-sha256',sha(payload/'SHA256SUMS.txt'),'--progress-channel',channel,'--no-desktop-shortcut']
    p=subprocess.Popen(list(map(str,command)),stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,creationflags=subprocess.CREATE_NO_WINDOW)
    samples=[];deadline=time.monotonic()+180
    while p.poll() is None and time.monotonic()<deadline:
        snapshot=struct.unpack('<iiiiqq',shared[:])
        if snapshot[1]%2==0 and snapshot[1]==struct.unpack('<iiiiqq',shared[:])[1]:samples.append(snapshot[2:3]+snapshot[4:])
        time.sleep(.001)
    try:stdout,stderr=p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill();p.communicate(timeout=10);raise
    final=struct.unpack('<iiiiqq',shared[:]);samples.append(final[2:3]+final[4:]);shared.close()
    starts=[0,0,1500,5000,7000,9600,9800,10000];ends=[0,1500,5000,7000,9600,9800,9999,10000]
    units=[starts[s]+(min(d,t)*(ends[s]-starts[s])//t if t else 0) for s,d,t in samples if 0<=s<8]
    checks['ActualBackendReportsVerifyCopyAndRegistration']=p.returncode==0 and {3,4,5}.issubset({s[0] for s in samples})
    checks['LiveProgressMonotonicAndNotPremature100']=all(b>=a for a,b in zip(units,units[1:])) and max(units,default=0)<10000 and final[2]==5
    checks['ProgressInstallationUninstalls']=uninstall(identity,installed,start,desktop,common)
    checks['UserLibraryAndSettingsPreservedAfterUninstall']=all(sha(Path(p))==h for p,h in keep.items())
    deadline=time.monotonic()+45
    while list(temporary.glob('MusicBoard-Uninstall-*.exe')) and time.monotonic()<deadline:time.sleep(.05)
    checks['OwnedTemporaryStagesAndUninstallersRemoved']=not list(temporary.glob('FullTools-stage-*')) and not list(temporary.glob('MusicBoard-Uninstall-*.exe'))
    checks['PrivateRegistrationAllRemoved']=all(registration(i) is None for i,_,_,_ in identities)
    report={'Passed':all(checks.values()),'Checks':checks,'FirstInstallMilliseconds':elapsed,'LiveBackendSteps':sorted({s[0] for s in samples}),'Commands':commands,'Scope':'Final single EXE and actual old public 0.3.5 payload; isolated custom directories, private HKCU identity and shortcuts, separate profile and library; no UI automation or real user installation changed.'}
    (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'Passed':report['Passed'],'Checks':checks,'Report':str(out/'report.json')},ensure_ascii=False))
    if not report['Passed']:raise RuntimeError('Installer acceptance failed; preserve evidence: '+str(out))

if __name__=='__main__':main()

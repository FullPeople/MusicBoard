"""Package only accepted bytes; mount, install, launch and remove an owned app copy."""
from pathlib import Path
import hashlib, json, os, platform, plistlib, shutil, subprocess

ROOT=Path(__file__).resolve().parents[2]; OUT=ROOT/'artifacts/macos'; APP=OUT/'MusicBoard.app'
report=json.loads((OUT/'native-checks/report.json').read_text())
if not report['Passed'] or not report['Checks'] or not all(report['Checks'].values()): raise RuntimeError('Native product checks are incomplete')
dist=OUT/'distribution'; dist.mkdir(exist_ok=False)
stage=OUT/'image-content'; stage.mkdir(exist_ok=False)
subprocess.run(['ditto',str(APP),str(stage/'MusicBoard.app')],check=True)
(stage/'Applications').symlink_to('/Applications')
readme='''MusicBoard 0.3.6 · macOS Preview 1

安装：把 MusicBoard 拖到 Applications。没有管理员权限时，可复制到个人 ~/Applications 文件夹。
Install: drag MusicBoard into Applications, or use your own ~/Applications folder without administrator privileges.

当前是原生 Mac 测试版，最低 macOS 13；不是 Windows 全功能等同版。
Native macOS preview, macOS 13 or later. Not full Windows feature parity.

播放不申请麦克风、摄像头、录屏、辅助功能或自动化权限。首次选择曲库时，通过文件夹选择器授权访问。
Playback needs no microphone, camera, screen recording, Accessibility or Automation permission. Choose a library with the folder picker.

此包仅作临时签名，尚无 Developer ID 签名和 Apple 公证。系统可能提示无法验证开发者。
This build is ad-hoc signed, without Developer ID signing or Apple notarization. Gatekeeper may block its first launch.
Apple 官方说明 / Apple guidance: https://support.apple.com/102445
不要关闭系统整体安全检查。Do not disable system-wide security checks.

主面板关闭后，唱片悬浮按钮和音频继续运行。使用菜单中的“退出 MusicBoard”完全退出。
Closing the panel keeps the orb and audio running. Choose Quit MusicBoard to exit completely.

曲库支持 BGM/分类/音频 和 SFX/分类/音效，也可拖入文件。支持音频直链和通过内置 yt-dlp 解析页面。
Use BGM/category/audio and SFX/category/effect folders, drop files, or import audio/page links via bundled yt-dlp.
部分来源需要登录、订阅或地区许可；测试版不保证任何平台的所有链接可用。在线内容只保留本次会话，下载内容保留在本机。
Some sources need login, subscription or regional access; not all links work. Online streams are session-only; downloads persist locally.

设置与下载 / Settings and downloads: ~/Library/Application Support/MusicBoard
卸载：先退出，再将 MusicBoard.app 移至废纸篓。默认保留设置和下载。
Uninstall: quit and move MusicBoard.app to Trash. Settings and downloads are preserved.

FFmpeg is LGPL 2.1 or later; exact sources and build options are included in the app's Contents/Resources/Licenses and build-provenance.json.
The app is MIT. yt-dlp is bundled as a separate executable; its upstream third-party notices are included.
'''
(stage/'开始使用 - Read Me.txt').write_text(readme,encoding='utf-8')
arch='arm64' if platform.machine()=='arm64' else 'x64'
image=dist/f'MusicBoard-0.3.6-macos-{arch}.dmg'
subprocess.run(['hdiutil','create','-volname','MusicBoard','-srcfolder',str(stage),'-format','UDZO','-ov',str(image)],check=True,timeout=180)
subprocess.run(['hdiutil','verify',str(image)],check=True,timeout=90)
mount=OUT/'mounted-image'; mount.mkdir()
owned=OUT/'owned-user-Applications'; owned.mkdir()
installed=owned/'MusicBoard.app'
checks={'NativeChecksPassedBeforePackaging':True}
def inventory(path):
    result={}
    for file in sorted(path.rglob('*')):
        if file.is_symlink(): result[str(file.relative_to(path))]={'Link':os.readlink(file)}
        elif file.is_file():
            with file.open('rb') as stream: result[str(file.relative_to(path))]={'SHA256':hashlib.file_digest(stream,'sha256').hexdigest()}
    return result
expected=inventory(APP)
try:
    subprocess.run(['hdiutil','attach','-readonly','-nobrowse','-mountpoint',str(mount),str(image)],check=True,timeout=90)
    checks['MountedImageContainsExactAcceptedApp']=inventory(mount/'MusicBoard.app')==expected
    checks['ApplicationsShortcutTargetsSystemFolder']=(mount/'Applications').is_symlink() and os.readlink(mount/'Applications')=='/Applications'
    subprocess.run(['ditto',str(mount/'MusicBoard.app'),str(installed)],check=True,timeout=90)
    checks['OrdinaryUserCopyInstallPreservesAllFiles']=inventory(installed)==expected
    subprocess.run(['codesign','--verify','--deep','--strict',str(installed)],check=True,timeout=90)
    checks['InstalledAppSignatureVerifies']=True
    subprocess.run([str(installed/'Contents/MacOS/MusicBoard'),'--profile',str(OUT/'owned-installed-profile'),'--self-test',str(OUT/'installed-checks')],check=True,timeout=120)
    installed_report=json.loads((OUT/'installed-checks/report.json').read_text())
    checks['InstalledApplicationWindowAndPlaybackActuallyRun']=installed_report['Passed'] and all(installed_report['Checks'].values())
    checks['InstalledAppBytesRemainUnchanged']=inventory(installed)==expected
    # This absolute path is an owned fixture directory, never the user's Applications.
    if installed.resolve().parent != owned.resolve() or installed.is_symlink(): raise RuntimeError('Unsafe uninstall fixture')
    shutil.rmtree(installed)
    checks['OwnedCopyCanBeUninstalledWithoutAdministrator']=not installed.exists()
    checks['UninstallPreservesProfile']=(OUT/'owned-installed-profile/preferences.json').is_file()
finally:
    subprocess.run(['hdiutil','detach',str(mount)],check=True,timeout=60)
if not all(checks.values()): raise RuntimeError('Distribution check failed: '+json.dumps(checks))
with image.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
record={'Passed':True,'Checks':checks,'ProductSource':json.loads((OUT/'build.json').read_text())['ProductSource'],'Version':'0.3.6-macos.1','Architecture':arch,'File':image.name,'Bytes':image.stat().st_size,'SHA256':digest,'NativeChecks':len(report['Checks']),'InstalledRepeatedChecks':len(installed_report['Checks']),'Signature':'Ad-hoc app signature only; no Developer ID/notarization','Scope':'Actual read-only DMG mount, exact file copy to an owned user install folder, real installed GUI/playback, normal fixture removal and preserved profile. No actual Finder drag/Gatekeeper/physical speakers claim.'}
(dist/f'MusicBoard-macos-{arch}-validation.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
(dist/f'SHA256SUMS-macos-{arch}.txt').write_text(digest+'  '+image.name+'\n',encoding='utf-8')
(dist/'开始使用 - Read Me.txt').write_text(readme,encoding='utf-8')
print(json.dumps(record))

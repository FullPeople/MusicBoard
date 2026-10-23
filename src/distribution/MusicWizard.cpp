// MIT: adapted from FullPeople/FullOffice d3f6036; see ORIGIN.md.
#include "BundleExtract.h"
#include "BundleConfig.h"
#include "WizardMetadata.h"
#include "core/localization.h"
#include "SetupProgress.h"
#include "core/sha256.h"
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <array>
#include <condition_variable>

namespace {
using musicboard::tr;
constexpr COLORREF ink=RGB(28,37,57), muted=RGB(102,114,136), accent=RGB(63,91,220);
constexpr COLORREF white=RGB(255,255,255), side=RGB(245,247,253), border=RGB(227,232,243);
constexpr DWORD window_style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN;
enum Id { Next=1001, Cancel, Path, Progress, Status, Back=1010, Browse, Accept, Desktop, Launch, License };
HWND window=nullptr, next_button=nullptr, cancel_button=nullptr, path_edit=nullptr, size_label=nullptr, status_label=nullptr, progress_bar=nullptr, license_edit=nullptr;
std::vector<HWND> controls;
HFONT body_font=nullptr,title_font=nullptr,small_font=nullptr,bold_font=nullptr;
HBRUSH white_brush=nullptr;
HICON product_icon=nullptr;
int dpi=96,page=0,license_selection=0;
bool accepted=false,quiet=false,building=false,launch_after=true;
struct InstallChoices { bool desktop = true; } choices;
std::filesystem::path install_root,registered_install_root,test_shortcut,test_desktop;
std::wstring test_registry;
fulltools::ExtractionProgress progress;
std::atomic<int> phase{0}; // 0 idle, 1 checking, 2 extracting, 3 transaction, 4 cleanup, 5 finished
std::atomic<DWORD> result_code{0};
std::mutex message_mutex;
std::wstring final_message;
std::thread worker;
fulltools::SetupProgressChannel progress_channel;
int displayed_progress = -1;
std::thread disk_worker;
std::mutex disk_mutex;
std::condition_variable disk_changed;
bool disk_stop = false, disk_requested = false, disk_known = false;
std::wstring disk_requested_root, disk_result_root;
std::uint64_t disk_available = 0;
std::array<std::wstring, 3> license_cache;

int px(int value){return MulDiv(value,dpi,96);}
bool fitted_window(UINT native_dpi,const RECT* suggested,RECT& result){
    RECT work{};MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    const auto screen=suggested?MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST):MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    if(screen&&GetMonitorInfoW(screen,&monitor))work=monitor.rcWork;
    else if(!SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0))return false;
    RECT frame{};if(!AdjustWindowRectExForDpi(&frame,window_style,FALSE,0,native_dpi))return false;
    const int available_width=work.right-work.left-(frame.right-frame.left)-16;
    const int available_height=work.bottom-work.top-(frame.bottom-frame.top)-16;
    if(available_width<=0||available_height<=0)return false;
    const int effective=std::max(1,std::min({static_cast<int>(native_dpi),MulDiv(available_width,96,890),MulDiv(available_height,96,574)}));
    RECT size{0,0,MulDiv(890,effective,96),MulDiv(574,effective,96)};
    if(!AdjustWindowRectExForDpi(&size,window_style,FALSE,0,native_dpi))return false;
    const LONG width=size.right-size.left,height=size.bottom-size.top;
    const LONG left=suggested?std::clamp<LONG>(suggested->left,work.left,std::max(work.left,work.right-width)):work.left+(work.right-work.left-width)/2;
    const LONG top=suggested?std::clamp<LONG>(suggested->top,work.top,std::max(work.top,work.bottom-height)):work.top+(work.bottom-work.top-height)/2;
    dpi=effective;result=RECT{left,top,left+width,top+height};return true;
}
std::wstring quote(const std::wstring& text){
    std::wstring out=L"\"";size_t slash=0;
    for(wchar_t c:text){if(c==L'\\'){++slash;continue;}out.append(c==L'\"'?slash*2+1:slash,L'\\');slash=0;out+=c;}
    out.append(slash*2,L'\\');out+=L'\"';return out;
}
std::wstring wide(const std::string& text){
    if(text.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring out(static_cast<size_t>(count),L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),count);return out;
}
std::wstring license_text(int selection){
    if(!license_cache[static_cast<size_t>(selection)].empty())return license_cache[static_cast<size_t>(selection)];
    const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(102+selection),RT_RCDATA);
    const auto loaded=resource?LoadResource(nullptr,resource):nullptr;
    const auto* data=loaded?static_cast<const char*>(LockResource(loaded)):nullptr;
    const auto text=data?wide(std::string(data,SizeofResource(nullptr,resource))):tr(L"未能读取许可条款，请重新下载安装包。",L"The license terms could not be loaded. Download the installer again.");
    std::wstring normalized;normalized.reserve(text.size()+text.size()/20);
    for(size_t index=0;index<text.size();++index){if(text[index]==L'\n'&&(index==0||text[index-1]!=L'\r'))normalized+=L'\r';normalized+=text[index];}
    license_cache[static_cast<size_t>(selection)]=normalized;return normalized;
}
std::wstring size_text(std::uint64_t bytes){
    std::wostringstream out;
    if(bytes>=1000000000)out<<std::fixed<<std::setprecision(2)<<static_cast<double>(bytes)/1000000000<<L" GB";
    else out<<(bytes+999999)/1000000<<L" MB";
    return out.str();
}
std::uint64_t selected_bytes(){return setup_metadata::total_bytes;}
std::wstring text_of(HWND control){
    const int count=GetWindowTextLengthW(control);std::wstring out(static_cast<size_t>(count)+1,L'\0');
    GetWindowTextW(control,out.data(),count+1);out.resize(static_cast<size_t>(count));return out;
}
bool checked(int id){return IsDlgButtonChecked(window,id)==BST_CHECKED;}
void remember(){
    if(page==0)accepted=checked(Accept);
    if(page==2){if(path_edit)install_root=text_of(path_edit);choices.desktop=checked(Desktop);}
    if(page==5&&result_code.load()==0){launch_after=checked(Launch);}
}
HWND child(const wchar_t* type,const std::wstring& text,DWORD style,int x,int y,int width,int height,int id=0,HFONT font=nullptr){
    HWND control=CreateWindowExW(type==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,type,text.c_str(),WS_CHILD|WS_VISIBLE|style,
        px(x),px(y),px(width),px(height),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(control){SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font?font:body_font),FALSE);SetWindowTheme(control,L"Explorer",nullptr);controls.push_back(control);}
    return control;
}
void label(const std::wstring& text,int x,int y,int width,int height,HFONT font=nullptr){
    const auto control=child(L"STATIC",text,SS_LEFT|SS_NOPREFIX,x,y,width,height,0,font);
    if(font==title_font||font==bold_font)SetPropW(control,L"Setup.Emphasis",reinterpret_cast<HANDLE>(1));
}
HWND checkbox(int id,const std::wstring& text,bool value,int x,int y,int width,bool enabled=true){
    HWND control=child(L"BUTTON",text,BS_AUTOCHECKBOX|WS_TABSTOP,x,y,width,28,id);
    SendMessageW(control,BM_SETCHECK,value?BST_CHECKED:BST_UNCHECKED,0);EnableWindow(control,enabled);return control;
}
void rounded(HDC dc,RECT rect,COLORREF fill,COLORREF edge,int radius){
    const auto brush=CreateSolidBrush(fill);const auto pen=CreatePen(PS_SOLID,1,edge);
    const auto old_brush=SelectObject(dc,brush),old_pen=SelectObject(dc,pen);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,px(radius),px(radius));
    SelectObject(dc,old_brush);SelectObject(dc,old_pen);DeleteObject(brush);DeleteObject(pen);
}
void draw_text(HDC dc,const std::wstring& text,HFONT font,COLORREF color,int x,int y,int width,int height,UINT format=DT_LEFT|DT_WORDBREAK|DT_NOPREFIX){
    const auto old=SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    RECT rect{px(x),px(y),px(x+width),px(y+height)};DrawTextW(dc,text.c_str(),-1,&rect,format);SelectObject(dc,old);
}
void make_fonts(){
    for(auto font:{body_font,title_font,small_font,bold_font})if(font)DeleteObject(font);
    const auto make=[](int size,int weight){return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");};
    body_font=make(14,FW_NORMAL);title_font=make(25,FW_SEMIBOLD);small_font=make(12,FW_NORMAL);bold_font=make(15,FW_SEMIBOLD);
}
void set_text(HWND control,const std::wstring& text){if(control&&text_of(control)!=text)SetWindowTextW(control,text.c_str());}
std::wstring current_drive(){
    const auto path=path_edit?std::filesystem::path(text_of(path_edit)):install_root;
    const auto root=path.root_path().wstring();
    return root.size()==3&&root[1]==L':'?root:L"";
}
void update_size(){
    if(!size_label)return;
    const auto root=current_drive();std::wstring suffix;
    {std::lock_guard lock(disk_mutex);if(root==disk_result_root&&disk_known)suffix=tr(L"    ·    当前磁盘可用 ",L"    ·    Available: ")+size_text(disk_available);}
    set_text(size_label,tr(L"预计占用 ",L"Installed size: ")+size_text(selected_bytes())+suffix);
}
void request_disk_space(){
    const auto root=current_drive();
    if(root.empty()){update_size();return;}
    {std::lock_guard lock(disk_mutex);if(root==disk_result_root||root==disk_requested_root)return;disk_requested_root=root;disk_requested=true;}
    disk_changed.notify_one();
}
void start_disk_worker(){
    disk_worker=std::thread([]{
        for(;;){
            std::wstring root;
            {std::unique_lock lock(disk_mutex);disk_changed.wait(lock,[]{return disk_stop||disk_requested;});if(disk_stop)return;root=disk_requested_root;disk_requested=false;}
            const auto type=GetDriveTypeW(root.c_str());ULARGE_INTEGER available{};
            const bool known=(type==DRIVE_FIXED||type==DRIVE_REMOVABLE||type==DRIVE_RAMDISK)&&GetDiskFreeSpaceExW(root.c_str(),&available,nullptr,nullptr);
            {std::lock_guard lock(disk_mutex);disk_result_root=root;disk_available=available.QuadPart;disk_known=known;}
            PostMessageW(window,WM_APP+2,0,0);
        }
    });
}
void show_page();
void finish(DWORD code,const std::wstring& message){
    {std::lock_guard lock(message_mutex);final_message=message;}result_code.store(code);phase.store(5);
    if(window)PostMessageW(window,WM_APP+1,0,0);
}
void install(){
    std::error_code location_error;
    if(!registered_install_root.empty()){
        auto chosen=std::filesystem::weakly_canonical(install_root,location_error);
        if(location_error){finish(1,tr(L"无法检查所选安装目录，请选择可写的文件夹。",L"Cannot check the selected installation folder. Choose a writable folder."));return;}
        auto registered=std::filesystem::weakly_canonical(registered_install_root,location_error);
        if(!chosen.has_filename()&&chosen!=chosen.root_path())chosen=chosen.parent_path();
        if(!registered.has_filename()&&registered!=registered.root_path())registered=registered.parent_path();
        if(location_error||!registered_install_root.is_absolute()||_wcsicmp(chosen.c_str(),registered.c_str())!=0){
            finish(1,tr(L"MusicBoard 已安装在其他位置。请沿用原安装目录，或先卸载后再选择新目录。卸载会保留曲库和设置。",L"MusicBoard is already installed in another folder. Use the existing folder, or uninstall before choosing a new one. Uninstalling keeps your library and settings."));return;
        }
    }
    if(std::filesystem::exists(install_root,location_error)&&(!std::filesystem::is_directory(install_root,location_error)||
        (!std::filesystem::is_empty(install_root,location_error)&&!std::filesystem::is_regular_file(install_root/L"install-journal.txt",location_error)))){
        finish(1,tr(L"请选择空文件夹，或现有 MusicBoard 的安装文件夹。请勿选择存放其他文件的目录。",L"Choose an empty folder or the existing MusicBoard installation folder. Do not select a folder containing other files."));return;
    }
    auto parent=install_root;
    while(!parent.empty()&&!std::filesystem::exists(parent,location_error)){const auto next=parent.parent_path();if(next==parent)break;parent=next;}
    const auto free=std::filesystem::space(parent,location_error);
    if(location_error||free.available<selected_bytes()+64ULL*1024*1024){finish(1,tr(L"无法确认安装磁盘的可用空间，或剩余空间不足。请选择可写的其他文件夹。",L"The installation drive could not be checked or has insufficient space. Choose another writable folder."));return;}
    const HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(101),RT_RCDATA);
    const HGLOBAL loaded=resource?LoadResource(nullptr,resource):nullptr;
    const auto* bytes=loaded?static_cast<const std::uint8_t*>(LockResource(loaded)):nullptr;
    const DWORD length=resource?SizeofResource(nullptr,resource):0;
    musicboard::Failure failure;std::string digest;
    const auto checksum_progress=[](std::uint64_t done,std::uint64_t total){progress_channel.write(fulltools::SetupStep::envelope,done,total);return !progress.cancelled.load();};
    if(!bytes||!musicboard::sha256_hex(bytes,length,digest,failure,checksum_progress)||digest!=payload_sha256){
        finish(progress.cancelled.load()?ERROR_CANCELLED:2,progress.cancelled.load()?tr(L"安装已取消，已安装的软件未被更改。",L"Installation cancelled. The installed application was not changed."):tr(L"安装包校验失败。请重新下载安装包，已安装的软件未被更改。",L"Installer verification failed. Download it again. The installed application was not changed."));return;}
    if(progress.cancelled.load()){finish(ERROR_CANCELLED,tr(L"安装已取消，尚未更改已安装的软件。",L"Installation cancelled before changing the installed application."));return;}
    wchar_t temporary[32768]{};GUID identifier{};wchar_t id[40]{};
    if(!GetTempPathW(32768,temporary)||FAILED(CoCreateGuid(&identifier))||!StringFromGUID2(identifier,id,40)){finish(2,tr(L"无法建立安装暂存目录，请检查临时目录是否可写。",L"Cannot create a staging folder. Check that your temporary folder is writable."));return;}
    const auto base=std::filesystem::path(temporary),stage=base/(L"FullTools-stage-"+std::wstring(id));
    struct Cleanup{
        std::filesystem::path path,parent;bool active=true;
        bool run(std::string& error){if(!active)return true;active=false;return fulltools::remove_owned_stage(path,parent,error);}
        ~Cleanup(){if(active){std::string ignored;static_cast<void>(fulltools::remove_owned_stage(path,parent,ignored));}}
    }cleanup{stage,base};
    const auto finish_staged=[&](DWORD code,const std::wstring& message){
        phase.store(4);if(code==0)progress_channel.write(fulltools::SetupStep::cleanup,0,1);
        std::string cleanup_error;const bool cleaned=cleanup.run(cleanup_error);
        if(code==0&&cleaned)progress_channel.write(fulltools::SetupStep::complete,1,1);
        finish(code,message+(cleaned?L"":tr(L"\r\n部分安装临时文件未能清理。",L"\r\nSome temporary installation files could not be removed.")));
    };
    phase.store(2);std::string error;
    fulltools::ExtractionOptions extraction;extraction.flush_staged_files=false;
    if(!fulltools::extract_bundle(bytes,length,stage,progress,error,extraction)){finish_staged(progress.cancelled.load()?ERROR_CANCELLED:2,progress.cancelled.load()?tr(L"安装已取消，已安装的软件未被更改。",L"Installation cancelled. The installed application was not changed."):tr(L"准备安装失败：",L"Cannot prepare the installation: ")+wide(error));return;}
    if(progress.cancelled.load()){finish_staged(ERROR_CANCELLED,tr(L"安装已取消，已安装的软件未被更改。",L"Installation cancelled. The installed application was not changed."));return;}
    const auto executable=stage/backend_name;std::error_code status;
    if(!std::filesystem::is_regular_file(executable,status)){finish_staged(2,tr(L"安装包缺少安装程序，请重新下载。",L"The installer is missing a required component. Download it again."));return;}
    std::wstring command=quote(executable.wstring())+L" --quiet --no-launch --package-root "+quote(stage.wstring())+L" --install-root "+quote(install_root.wstring())+
        L" --progress-channel "+quote(progress_channel.name())+L" --manifest-sha256 "+quote(wide(progress.manifest_sha256))+
        (choices.desktop?L" --desktop-shortcut":L" --no-desktop-shortcut");
    command += musicboard::language() == musicboard::Language::english ? L" --lang en" : L" --lang cn";
    if(!test_registry.empty())command+=L" --registry-subkey "+quote(test_registry);
    if(!test_shortcut.empty())command+=L" --shortcut-path "+quote(test_shortcut.wstring());
    if(!test_desktop.empty())command+=L" --desktop-shortcut-path "+quote(test_desktop.wstring());
    phase.store(3);
    // Keep diagnostic output outside the fully verified payload directory.
    const auto log=base/(L"MusicBoard-setup-error-"+std::wstring(id)+L".log");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log_handle=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(log_handle!=INVALID_HANDLE_VALUE){startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdError=log_handle;startup.hStdOutput=log_handle;startup.hStdInput=nullptr;}
    DWORD code=2;
    if(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,log_handle!=INVALID_HANDLE_VALUE,CREATE_NO_WINDOW,nullptr,stage.c_str(),&startup,&process)){
        CloseHandle(process.hThread);if(WaitForSingleObject(process.hProcess,INFINITE)==WAIT_OBJECT_0&&!GetExitCodeProcess(process.hProcess,&code))code=2;CloseHandle(process.hProcess);
    }
    if(log_handle!=INVALID_HANDLE_VALUE)CloseHandle(log_handle);
    std::ifstream input(log,std::ios::binary);std::string details((std::istreambuf_iterator<char>(input)),{});input.close();std::filesystem::remove(log,status);
    if(code==0)finish_staged(0,tr(L"MusicBoard 已安装完成。曲库、设置和日志会保留在原来的位置。",L"MusicBoard is installed. Your library, settings and logs stay in their original locations."));
    else finish_staged(code,tr(L"安装未完成。请先退出正在运行的 MusicBoard，并检查安装目录是否可写。",L"Installation did not finish. Exit MusicBoard and check that the installation folder is writable.")+(details.empty()?L"":tr(L"\r\n\r\n详细信息：",L"\r\n\r\nDetails: ")+wide(details.substr(0,1800))));
}
bool validate_location(){
    const auto text=install_root.wstring();
    if(install_root.empty()||!install_root.is_absolute()||install_root==install_root.root_path()||text.find_first_of(L"\"<>|?*")!=std::wstring::npos){
        if(!quiet)MessageBoxW(window,tr(L"请选择完整的安装文件夹，例如 D:\\Apps\\MusicBoard。",L"Choose a full installation folder, such as D:\\Apps\\MusicBoard."),tr(L"检查安装位置",L"Check installation folder"),MB_OK|MB_ICONINFORMATION);return false;}
    return true;
}
void begin(){
    if(phase.load()>0&&phase.load()<5)return;
    if(!quiet&&!accepted)return;
    if(!validate_location()){result_code.store(1);return;}
    if(worker.joinable())worker.join();
    GUID progress_id{};wchar_t identifier[40]{};
    if(FAILED(CoCreateGuid(&progress_id))||!StringFromGUID2(progress_id,identifier,40)||!progress_channel.create(L"Local\\MusicBoard.Setup.Progress."+std::wstring(identifier))){finish(2,tr(L"无法建立安装进度通道，请关闭安装程序后重试。",L"Cannot prepare installation progress. Close the installer and try again."));return;}
    progress.observer=[](std::uint64_t done,std::uint64_t total){progress_channel.write(fulltools::SetupStep::extract,done,total);};
    displayed_progress=-1;
    result_code.store(0);progress.cancelled.store(false);progress.total.store(0);progress.completed.store(0);phase.store(1);
    if(!quiet){page=4;show_page();SetTimer(window,1,100,nullptr);}
    try{worker=std::thread([]{try{install();}catch(const std::exception& error){finish(2,tr(L"安装遇到错误：",L"Installation error: ")+wide(error.what()));}});}
    catch(const std::exception& error){finish(2,tr(L"无法开始安装：",L"Cannot start the installation: ")+wide(error.what()));}
}
void browse(){
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){MessageBoxW(window,tr(L"无法打开文件夹选择窗口。你可以直接输入完整的安装路径。",L"The folder picker could not open. Enter the full installation path instead."),L"MusicBoard",MB_OK|MB_ICONINFORMATION);return;}
    DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    dialog->SetTitle(tr(L"选择 MusicBoard 的安装位置",L"Choose the MusicBoard installation folder"));dialog->SetOkButtonLabel(tr(L"选择文件夹",L"Select folder"));
    const auto root=current_drive();Microsoft::WRL::ComPtr<IShellItem> initial;
    if(!root.empty()&&SUCCEEDED(SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&initial))))dialog->SetFolder(initial.Get());
    if(SUCCEEDED(dialog->Show(window))){Microsoft::WRL::ComPtr<IShellItem> selected;PWSTR path=nullptr;
        if(SUCCEEDED(dialog->GetResult(&selected))&&SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH,&path))){
            std::filesystem::path folder(path);CoTaskMemFree(path);if(_wcsicmp(folder.filename().c_str(),L"MusicBoard")!=0)folder/=L"MusicBoard";SetWindowTextW(path_edit,folder.c_str());}}
}
void show_page(){
    SendMessageW(window,WM_SETREDRAW,FALSE,0);
    building=true;for(const HWND control:controls)DestroyWindow(control);controls.clear();
    path_edit=nullptr;size_label=nullptr;status_label=nullptr;progress_bar=nullptr;license_edit=nullptr;
    const wchar_t* titles[]{tr(L"欢迎安装 MusicBoard",L"Welcome to MusicBoard"),tr(L"了解安装内容",L"Included components"),tr(L"选择安装位置",L"Installation options"),tr(L"准备好开始安装",L"Ready to install"),tr(L"正在安装 MusicBoard",L"Installing MusicBoard"),result_code.load()==0?tr(L"安装完成，开始使用吧",L"Ready to play"):result_code.load()==ERROR_CANCELLED?tr(L"安装已取消",L"Installation cancelled"):tr(L"安装未完成",L"Installation incomplete")};
    const wchar_t* descriptions[]{tr(L"开始之前，请阅读开源许可和第三方声明。",L"Read the software licenses and third-party notices before continuing."),tr(L"完整安装音乐板和所需的媒体工具。",L"MusicBoard includes the media tools needed for playback and import."),tr(L"为当前 Windows 用户安装，并设置常用选项。",L"Install for your Windows account and choose your shortcuts."),tr(L"核对下面的选项，确认后开始安装。",L"Review your choices before starting the installation."),tr(L"请稍候。安装程序正在准备并校验所需文件。",L"Please wait while the installer prepares and verifies the files."),L""};
    label(titles[page],252,42,602,40,title_font);label(descriptions[page],252,91,602,26);
    if(page==0){
        const auto combo=child(WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,252,133,596,160,License);
        for(const auto text:{tr(L"MusicBoard 主程序 · MIT 许可证",L"MusicBoard · MIT license"),tr(L"第三方组件声明",L"Third-party notices"),tr(L"媒体工具与安装器开源许可",L"Media tools and installer licenses")})SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(license_selection),0);
        license_edit=child(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,252,174,596,247,1021,small_font);
        SendMessageW(license_edit,EM_SETLIMITTEXT,4000000,0);std::wstring text=license_text(license_selection);
        SetWindowTextW(license_edit,text.c_str());checkbox(Accept,tr(L"我已阅读并接受适用的许可条款",L"I have read and accept the applicable license terms"),accepted,252,439,596);
    }else if(page==1){
        checkbox(1020,tr(L"MusicBoard 完整程序（必需）",L"MusicBoard complete application (required)"),true,270,151,550,false);
        label(tr(L"背景音乐、音效台、曲库管理与软件更新。",L"Background music, sound effects, library management and software updates."),294,198,510,52,small_font);
        label(tr(L"包含链接导入和播放所需的媒体工具。",L"Includes the media tools needed for link import and playback."),270,293,550,48,bold_font);
        label(tr(L"软件不附带音乐。你的曲库和设置会保留在原来的位置。",L"No music is bundled. Your library and settings stay in their original locations."),270,352,550,64,small_font);
        size_label=child(L"STATIC",L"",SS_LEFT,252,437,596,25,1022,bold_font);update_size();
    }else if(page==2){
        label(tr(L"安装文件夹",L"Installation folder"),252,136,596,24,bold_font);path_edit=child(L"EDIT",install_root.wstring(),ES_AUTOHSCROLL|WS_TABSTOP,252,172,482,34,Path);
        child(L"BUTTON",tr(L"浏览…",L"Browse…"),BS_OWNERDRAW|WS_TABSTOP,746,172,102,34,Browse);
        size_label=child(L"STATIC",L"",SS_LEFT,252,218,596,25,1022,small_font);update_size();
        checkbox(Desktop,tr(L"创建桌面快捷方式",L"Create a desktop shortcut"),choices.desktop,252,283,596);
        label(tr(L"开始菜单和 Windows 应用列表会自动添加 MusicBoard。",L"MusicBoard is added to the Start menu and the Windows app list."),252,343,596,50,small_font);
        label(tr(L"开始菜单入口和卸载程序会自动创建。\r\n选择可写的文件夹即可，无需管理员权限。",L"An uninstaller is included. Choose a writable folder; no administrator privileges are needed."),252,428,596,50,small_font);
    }else if(page==3){
        label(tr(L"安装位置",L"Installation folder"),270,148,550,24,small_font);label(install_root.wstring(),270,180,550,48,bold_font);label(tr(L"安装组件",L"Components"),270,245,550,24,small_font);
        label(tr(L"MusicBoard 完整程序与媒体工具",L"MusicBoard complete application and media tools"),270,278,550,72);
        label(std::wstring(tr(L"桌面快捷方式：",L"Desktop shortcut: "))+(choices.desktop?tr(L"创建",L"Create"):tr(L"不创建",L"Do not create")),270,376,550,44,small_font);
        label(tr(L"预计占用 ",L"Installed size: ")+size_text(selected_bytes())+tr(L"    ·    当前用户安装",L"    ·    Current user"),252,454,596,26,bold_font);
    }else if(page==4){
        label(tr(L"正在安装所选功能",L"Setting up MusicBoard"),270,177,552,30,bold_font);status_label=child(L"STATIC",tr(L"正在检查安装位置…",L"Checking the installation folder…"),SS_LEFT,270,229,552,58,Status);
        progress_bar=child(PROGRESS_CLASSW,L"",PBS_SMOOTH,270,302,552,10,Progress);
        SetWindowTheme(progress_bar,L"",L"");SendMessageW(progress_bar,PBM_SETRANGE32,0,10000);SendMessageW(progress_bar,PBM_SETBARCOLOR,0,accent);
        SendMessageW(progress_bar,PBM_SETPOS,static_cast<WPARAM>(std::max(0,displayed_progress)),0);
        label(tr(L"进度按安装包校验、所选组件解压、文件核对和写入更新。\r\n安装完成前请保持此窗口打开。",L"Progress follows package verification, extraction and installation.\r\nKeep this window open until the installation finishes."),270,347,552,65,small_font);
    }else{
        std::wstring message;{std::lock_guard lock(message_mutex);message=final_message;}label(message,270,178,552,result_code.load()==0?75:230);
        if(result_code.load()==0){label(install_root.wstring(),270,272,552,62,small_font);checkbox(Launch,tr(L"完成后启动 MusicBoard",L"Launch MusicBoard when finished"),launch_after,270,357,552);}
    }
    HWND back_button=child(L"BUTTON",tr(L"上一步",L"Back"),BS_OWNERDRAW|WS_TABSTOP,516,517,100,36,Back);
    next_button=child(L"BUTTON",page==3?tr(L"开始安装",L"Install"):page==5?result_code.load()==0?tr(L"完成",L"Finish"):tr(L"返回重试",L"Try again"):tr(L"下一步",L"Next"),BS_OWNERDRAW|WS_TABSTOP,628,517,108,36,Next);
    cancel_button=child(L"BUTTON",page==5?tr(L"关闭",L"Close"):tr(L"取消",L"Cancel"),BS_OWNERDRAW|WS_TABSTOP,748,517,100,36,Cancel);
    ShowWindow(back_button,page>0&&page<4?SW_SHOW:SW_HIDE);EnableWindow(next_button,page!=4&&(page!=0||accepted));if(page==4)ShowWindow(next_button,SW_HIDE);
    building=false;SendMessageW(window,WM_SETREDRAW,TRUE,0);RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    if(page==1||page==2)request_disk_space();if(page<4)SetFocus(page==0?GetDlgItem(window,License):page==2?path_edit:next_button);
}
void paint(HDC dc){
    RECT client{};GetClientRect(window,&client);FillRect(dc,&client,white_brush);
    const auto brush=CreateSolidBrush(side);RECT sidebar{0,0,px(214),client.bottom};FillRect(dc,&sidebar,brush);DeleteObject(brush);
    DrawIconEx(dc,px(28),px(30),product_icon,px(40),px(40),0,nullptr,DI_NORMAL);
    draw_text(dc,L"MusicBoard",title_font,ink,28,88,178,36);draw_text(dc,tr(L"为跑团准备音乐与音效",L"Music and sound effects\r\nfor your tabletop sessions"),small_font,muted,28,130,170,48);
    const int active=std::min(page,3);const wchar_t* names[]{tr(L"许可条款",L"Licenses"),tr(L"安装内容",L"Components"),tr(L"安装选项",L"Options"),tr(L"确认与安装",L"Install")};
    const wchar_t* descriptions[]{tr(L"了解许可与声明",L"Terms and notices"),tr(L"完整程序与工具",L"App and media tools"),tr(L"位置与快捷方式",L"Folder and shortcuts"),tr(L"准备音乐板",L"Set up MusicBoard")};
    for(int index=0;index<4;++index){const int y=198+index*65;
        if(index==active)rounded(dc,RECT{px(16),px(y-8),px(198),px(y+48)},RGB(230,235,253),RGB(230,235,253),12);
        draw_text(dc,index<active?L"✓":std::to_wstring(index+1),bold_font,index<=active?accent:muted,30,y+3,25,30);
        draw_text(dc,names[index],bold_font,index==active?accent:ink,65,y,130,26);draw_text(dc,descriptions[index],small_font,muted,65,y+27,130,24);
    }
    draw_text(dc,L"WINDOWS · x64",small_font,muted,28,505,170,22);draw_text(dc,tr(L"版本 ",L"Version ")+std::wstring(setup_metadata::version),small_font,muted,28,530,170,22);
    const auto pen=CreatePen(PS_SOLID,1,border);const auto old=SelectObject(dc,pen);MoveToEx(dc,px(214),px(501),nullptr);LineTo(dc,client.right,px(501));SelectObject(dc,old);DeleteObject(pen);
    if(page==1)rounded(dc,RECT{px(252),px(132),px(848),px(420)},white,RGB(190,201,244),12);
    if(page==3)rounded(dc,RECT{px(252),px(132),px(848),px(435)},white,border,12);
    if(page>=4)rounded(dc,RECT{px(252),px(145),px(848),px(458)},white,border,12);
}
LRESULT CALLBACK procedure(HWND owner,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){
        PAINTSTRUCT data{};const auto dc=BeginPaint(owner,&data);RECT client{};GetClientRect(owner,&client);
        const auto memory=CreateCompatibleDC(dc);const auto bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);
        if(memory&&bitmap){const auto old=SelectObject(memory,bitmap);paint(memory);BitBlt(dc,0,0,client.right,client.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);}
        else paint(dc);
        if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);EndPaint(owner,&data);return 0;
    }
    if(message==WM_CTLCOLORSTATIC||message==WM_CTLCOLORBTN||message==WM_CTLCOLOREDIT){const auto dc=reinterpret_cast<HDC>(wparam);SetTextColor(dc,message==WM_CTLCOLOREDIT||GetPropW(reinterpret_cast<HWND>(lparam),L"Setup.Emphasis")?ink:muted);SetBkColor(dc,white);return reinterpret_cast<LRESULT>(white_brush);}
    if(message==WM_DRAWITEM){const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);const bool primary=item->CtlID==Next,enabled=IsWindowEnabled(item->hwndItem)!=FALSE,pressed=(item->itemState&ODS_SELECTED)!=0;
        const auto fill=!enabled?RGB(237,240,247):primary?pressed?RGB(46,68,176):accent:pressed?side:white;
        FillRect(item->hDC,&item->rcItem,white_brush);rounded(item->hDC,item->rcItem,fill,primary?fill:border,10);
        const auto old=SelectObject(item->hDC,body_font);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,!enabled?RGB(154,164,184):primary?white:ink);
        RECT rect=item->rcItem;const auto text=text_of(item->hwndItem);DrawTextW(item->hDC,text.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if((item->itemState&ODS_FOCUS)!=0){InflateRect(&rect,-px(4),-px(4));DrawFocusRect(item->hDC,&rect);}SelectObject(item->hDC,old);return TRUE;}
    if(message==WM_COMMAND&&!building){const int id=LOWORD(wparam),event=HIWORD(wparam);
        if(id==Path&&event==EN_CHANGE){update_size();SetTimer(owner,2,300,nullptr);return 0;}
        if(id==License&&event==CBN_SELCHANGE){
            const auto selected=static_cast<int>(SendDlgItemMessageW(owner,License,CB_GETCURSEL,0,0));
            if(selected>=0&&selected<3&&selected!=license_selection){license_selection=selected;set_text(license_edit,license_text(selected));SendMessageW(license_edit,EM_SETSEL,0,0);SendMessageW(license_edit,EM_SCROLLCARET,0,0);}return 0;
        }
        if(id==Accept){accepted=checked(Accept);EnableWindow(next_button,accepted);InvalidateRect(next_button,nullptr,TRUE);return 0;}
        if(id==Browse){browse();return 0;}
        if(id==Back){remember();--page;show_page();return 0;}
        if(id==Cancel||id==IDCANCEL){SendMessageW(owner,WM_CLOSE,0,0);return 0;}
        if(id==Next||id==IDOK){if(page==4||(page==0&&!accepted))return 0;remember();
            if(page==3)begin();else if(page==5&&result_code.load()==0){
                if(launch_after){const auto app=install_root/L"current/MusicBoard.exe";if(reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",app.c_str(),nullptr,app.parent_path().c_str(),SW_SHOWNORMAL))<=32){MessageBoxW(owner,tr(L"软件已安装，但未能启动。请从开始菜单打开 MusicBoard。",L"MusicBoard is installed but could not be launched. Open it from the Start menu."),L"MusicBoard",MB_OK|MB_ICONINFORMATION);return 0;}}
                DestroyWindow(owner);
            }else{page=page==5?2:page+1;if(page==3&&!validate_location())page=2;show_page();}return 0;}
    }
    if(message==WM_TIMER&&wparam==2){KillTimer(owner,2);if(page==1||page==2)request_disk_space();return 0;}
    if(message==WM_TIMER&&wparam==1&&page==4){
        fulltools::SetupSnapshot snapshot;if(!progress_channel.read(snapshot))return 0;
        const int units=std::max(displayed_progress,fulltools::progress_units(snapshot));
        if(units!=displayed_progress){SendMessageW(progress_bar,PBM_SETPOS,static_cast<WPARAM>(units),0);displayed_progress=units;}
        std::wstring status;
        switch(snapshot.step){
            case fulltools::SetupStep::envelope:status=tr(L"正在校验安装包…  ",L"Verifying the installer…  ")+size_text(snapshot.completed)+L" / "+size_text(snapshot.total);break;
            case fulltools::SetupStep::extract:status=tr(L"正在解压程序文件…  ",L"Extracting the application…  ")+size_text(snapshot.completed)+L" / "+size_text(snapshot.total);break;
            case fulltools::SetupStep::verify:status=tr(L"正在核对程序文件…  ",L"Checking application files…  ")+std::to_wstring(snapshot.completed)+L" / "+std::to_wstring(snapshot.total);break;
            case fulltools::SetupStep::copy:status=tr(L"正在写入程序文件…  ",L"Installing application files…  ")+size_text(snapshot.completed)+L" / "+size_text(snapshot.total);break;
            case fulltools::SetupStep::registration:status=tr(L"正在创建快捷方式和卸载入口…",L"Creating shortcuts and the uninstall entry…");break;
            case fulltools::SetupStep::cleanup:status=tr(L"正在清理本次安装的临时文件…",L"Removing temporary installation files…");break;
            case fulltools::SetupStep::complete:status=tr(L"安装已完成。",L"Installation complete.");break;
            default:status=tr(L"正在检查安装位置…",L"Checking the installation folder…");break;
        }
        if(progress.cancelled.load())status=tr(L"正在取消并清理暂存文件…",L"Cancelling and removing temporary files…");
        else if(phase.load()==4)status=tr(L"正在清理本次安装的临时文件…",L"Removing temporary installation files…");
        status+=tr(L"\r\n总进度 ",L"\r\nOverall progress: ")+std::to_wstring(std::max(0,units)/100)+L"%";set_text(status_label,status);
        const bool cancellable=phase.load()<3&&!progress.cancelled.load();if((IsWindowEnabled(cancel_button)!=FALSE)!=cancellable)EnableWindow(cancel_button,cancellable);return 0;
    }
    if(message==WM_APP+1){KillTimer(owner,1);if(worker.joinable())worker.join();page=5;show_page();return 0;}
    if(message==WM_APP+2){if(!building&&(page==1||page==2))update_size();return 0;}
    if(message==WM_DPICHANGED){remember();RECT rect{};if(fitted_window(HIWORD(wparam),reinterpret_cast<RECT*>(lparam),rect)){make_fonts();SetWindowPos(owner,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);show_page();}return 0;}
    if(message==WM_CLOSE){if(page==4){if(phase.load()<3){progress.cancelled.store(true);EnableWindow(cancel_button,FALSE);}return 0;}
        if(page<4&&MessageBoxW(owner,tr(L"要退出安装向导吗？尚未安装 MusicBoard。",L"Exit the installer? MusicBoard has not been installed yet."),tr(L"退出安装",L"Exit installer"),MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return 0;DestroyWindow(owner);return 0;}
    if(message==WM_DESTROY){{std::lock_guard lock(disk_mutex);disk_stop=true;}disk_changed.notify_one();PostQuitMessage(static_cast<int>(result_code.load()));return 0;}return DefWindowProcW(owner,message,wparam,lparam);
}
}
int wmain(int argc,wchar_t** argv){
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 1;
    PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){CoUninitialize();return 1;}
    install_root=std::filesystem::path(local)/L"Programs/MusicBoard";CoTaskMemFree(local);
    int desktop=-1;bool root_supplied=false,registry_supplied=false,shortcut_supplied=false,desktop_path_supplied=false;
    const auto next_value=[&](int& index,std::wstring& value){
        if(index+1>=argc)return false;
        value=argv[index+1];if(value.empty()||value.starts_with(L"--"))return false;
        ++index;return true;
    };
    for(int index=1;index<argc;++index){const std::wstring argument=argv[index];
        std::wstring value;
        if(argument==L"--quiet")quiet=true;
        else if(argument==L"--no-launch")launch_after=false;
        else if(argument==L"--lang"){if(!next_value(index,value)||(value!=L"cn"&&value!=L"en")){CoUninitialize();return 1;}musicboard::set_language(value==L"en"?musicboard::Language::english:musicboard::Language::chinese);}
        else if(argument==L"--install-root"){if(!next_value(index,value)){CoUninitialize();return 1;}install_root=value;root_supplied=true;}
        else if(argument==L"--registry-subkey"){registry_supplied=true;if(!next_value(index,value)){CoUninitialize();return 1;}test_registry=value;}
        else if(argument==L"--shortcut-path"){shortcut_supplied=true;if(!next_value(index,value)){CoUninitialize();return 1;}test_shortcut=value;}
        else if(argument==L"--desktop-shortcut-path"){desktop_path_supplied=true;if(!next_value(index,value)){CoUninitialize();return 1;}test_desktop=value;}
        else if(argument==L"--desktop-shortcut")desktop=1;else if(argument==L"--no-desktop-shortcut")desktop=0;
        else{CoUninitialize();return 1;}}
    if(registry_supplied||shortcut_supplied||desktop_path_supplied){
        const auto valid_link=[](const std::filesystem::path& path){
            if(!path.is_absolute()||!path.has_filename()||_wcsicmp(path.extension().c_str(),L".lnk")!=0||path.wstring().find_first_of(L"\"<>|?*")!=std::wstring::npos)return false;
            for(const auto& component:path.relative_path())if(component==L"."||component==L"..")return false;
            return true;
        };
        const bool valid=std::all_of(test_registry.begin(),test_registry.end(),[](wchar_t letter){return (letter>=L'A'&&letter<=L'Z')||(letter>=L'a'&&letter<=L'z')||(letter>=L'0'&&letter<=L'9')||letter==L'.'||letter==L'-'||letter==L'_';});
        if(!registry_supplied||!shortcut_supplied||!desktop_path_supplied||test_registry.empty()||!valid||test_registry.size()>120||_wcsicmp(test_registry.c_str(),L"MusicBoard")==0||!root_supplied||!valid_link(test_shortcut)||!valid_link(test_desktop)){CoUninitialize();return 1;}
        launch_after=false;
    }
    const auto registry_path=std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\")+(test_registry.empty()?L"MusicBoard":test_registry);
    wchar_t registered_path[32768]{};DWORD path_bytes=sizeof(registered_path);
    if(RegGetValueW(HKEY_CURRENT_USER,registry_path.c_str(),L"InstallLocation",RRF_RT_REG_SZ,nullptr,registered_path,&path_bytes)==ERROR_SUCCESS){registered_install_root=registered_path;if(!root_supplied)install_root=registered_install_root;}
    DWORD registered_desktop=0,desktop_bytes=sizeof(registered_desktop);
    if(RegGetValueW(HKEY_CURRENT_USER,registry_path.c_str(),L"DesktopShortcutEnabled",RRF_RT_REG_DWORD,nullptr,&registered_desktop,&desktop_bytes)==ERROR_SUCCESS)choices.desktop=registered_desktop!=0;
    if(desktop>=0)choices.desktop=desktop!=0;
    if(quiet){begin();if(worker.joinable())worker.join();CoUninitialize();return static_cast<int>(result_code.load());}
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    RECT rectangle{};if(!fitted_window(GetDpiForSystem(),nullptr,rectangle)){CoUninitialize();return 1;}
    white_brush=CreateSolidBrush(white);make_fonts();
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=procedure;
    product_icon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(1));cls.hIcon=product_icon;cls.hIconSm=product_icon;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=nullptr;cls.lpszClassName=L"MusicBoard.InstallWizard";
    if(!RegisterClassExW(&cls)){CoUninitialize();return 1;}
    const int width=rectangle.right-rectangle.left,height=rectangle.bottom-rectangle.top;
    window=CreateWindowExW(0,cls.lpszClassName,tr(L"MusicBoard 安装向导",L"MusicBoard Setup"),window_style,rectangle.left,rectangle.top,width,height,nullptr,nullptr,cls.hInstance,nullptr);
    if(!window){CoUninitialize();return 1;}start_disk_worker();show_page();ShowWindow(window,SW_SHOW);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0)if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    if(worker.joinable())worker.join();{std::lock_guard lock(disk_mutex);disk_stop=true;}disk_changed.notify_one();if(disk_worker.joinable())disk_worker.join();for(auto font:{body_font,title_font,small_font,bold_font})if(font)DeleteObject(font);
    DeleteObject(white_brush);CoUninitialize();return static_cast<int>(result_code.load());
}

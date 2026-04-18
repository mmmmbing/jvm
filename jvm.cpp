#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <iomanip>

#pragma comment(lib, "shlwapi.lib")

// ================= 编码转换辅助 =================
std::wstring s2w(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    if (!wstr.empty()) wstr.pop_back(); // 移除自动添加的 null 终止符
    return wstr;
}

std::string w2s(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    if (!str.empty()) str.pop_back();
    return str;
}

// ================= JSON 处理辅助 =================
struct JdkEntry {
    std::string name;
    std::string path;
};

std::vector<JdkEntry> loadJdks(const std::string& filepath) {
    std::vector<JdkEntry> jdks;
    std::ifstream file(filepath);
    if (!file.is_open()) return jdks;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    size_t pos = 0;
    while (true) {
        size_t namePos = content.find("\"name\"", pos);
        if (namePos == std::string::npos) break;

        size_t nameStart = content.find(':', namePos) + 1;
        size_t q1 = content.find('\"', nameStart);
        size_t q2 = content.find('\"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;

        std::string name = content.substr(q1 + 1, q2 - q1 - 1);

        size_t pathPos = content.find("\"path\"", q2);
        if (pathPos == std::string::npos) break;

        size_t pathStart = content.find(':', pathPos) + 1;
        q1 = content.find('\"', pathStart);
        q2 = content.find('\"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;

        std::string path = content.substr(q1 + 1, q2 - q1 - 1);
        jdks.push_back({name, path});
        pos = q2 + 1;
    }
    return jdks;
}

bool saveJdks(const std::string& filepath, const std::vector<JdkEntry>& jdks) {
    std::ofstream file(filepath, std::ios::trunc);
    if (!file.is_open()) return false;

    file << "{\n  \"jdks\": [\n";
    for (size_t i = 0; i < jdks.size(); ++i) {
        file << "    {\"name\": \"" << jdks[i].name << "\", \"path\": \"" << jdks[i].path << "\"}";
        if (i < jdks.size() - 1) file << ",";
        file << "\n";
    }
    file << "  ]\n}\n";
    return true;
}

// ================= 系统环境变量操作 =================
// 注册表路径：系统环境变量位于 HKLM
const wchar_t* ENV_KEY_PATH = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

std::wstring getSysEnv(const std::wstring& varName) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENV_KEY_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return L"";

    wchar_t buffer[32767];
    DWORD size = sizeof(buffer);
    LONG res = RegQueryValueExW(hKey, varName.c_str(), NULL, NULL, (LPBYTE)buffer, &size);
    RegCloseKey(hKey);

    if (res == ERROR_SUCCESS) return std::wstring(buffer, size / sizeof(wchar_t));
    return L"";
}

bool setSysEnv(const std::wstring& varName, const std::wstring& varValue, bool& wasCreated) {
    HKEY hKey;
    // 检查是否存在
    bool exists = (getSysEnv(varName) != L"");

    LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENV_KEY_PATH, 0, KEY_SET_VALUE, &hKey);
    if (res != ERROR_SUCCESS) return false;

    res = RegSetValueExW(hKey, varName.c_str(), 0, REG_EXPAND_SZ, 
                         (const BYTE*)varValue.c_str(), 
                         static_cast<DWORD>((varValue.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    if (res == ERROR_SUCCESS) {
        wasCreated = !exists;
        // 广播通知
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
        return true;
    }
    return false;
}

void addToSysPath(const std::wstring& newEntry) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENV_KEY_PATH, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        std::wcerr << L"  [错误] 无法打开系统环境变量注册表项（请以管理员身份运行）\n";
        return;
    }

    wchar_t currentPath[32767];
    DWORD size = sizeof(currentPath);
    bool exists = (RegQueryValueExW(hKey, L"PATH", NULL, NULL, (LPBYTE)currentPath, &size) == ERROR_SUCCESS);
    
    std::wstring pathStr = exists ? std::wstring(currentPath) : L"";
    bool alreadyPresent = (pathStr.find(newEntry) != std::wstring::npos);

    if (!alreadyPresent) {
        if (!pathStr.empty() && pathStr.back() != L';') pathStr += L';';
        pathStr += newEntry;
        
        RegSetValueExW(hKey, L"PATH", 0, REG_EXPAND_SZ, 
                       (const BYTE*)pathStr.c_str(), 
                       static_cast<DWORD>((pathStr.size() + 1) * sizeof(wchar_t)));
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
        std::wcout << L"  [成功] 已" << (exists ? L"修改" : L"创建") << L" PATH，追加: " << newEntry << L"\n";
    } else {
        std::wcout << L"  [提示] PATH 中已存在 " << newEntry << L"，无需重复添加。\n";
    }
    RegCloseKey(hKey);
}

// ================= 主逻辑 =================
void printHelp() {
    std::cout << "用法: jvm.exe <命令> [参数]\n";
    std::cout << "命令:\n";
    std::cout << "  init                  初始化：创建空 javapath.json，设置系统 JAVA_HOME 为空，添加 %%JAVA_HOME%%\\bin 到 PATH\n";
    std::cout << "  ls                    列出所有已注册的 JDK\n";
    std::cout << "  -j <名称> <路径>      注册/更新 JDK 到 javapath.json\n";
    std::cout << "  -s <名称>             将指定 JDK 的路径设置为系统 JAVA_HOME\n";
}

int main(int argc, char* argv[]) {
    // 设置控制台为 UTF-8，解决中文乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        printHelp();
        return 1;
    }

    // 获取 exe 同级目录
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) exeDir = exeDir.substr(0, lastSlash);
    std::string jsonFile = exeDir + "\\javapath.json";

    std::string cmd = argv[1];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    if (cmd == "init") {
        std::cout << "正在初始化系统环境...\n";
        
        // 1. 创建空 JSON
        if (saveJdks(jsonFile, {})) {
            std::cout << "  [成功] 已在同级目录创建空的 javapath.json\n";
        } else {
            std::cerr << "  [失败] 无法创建 javapath.json\n";
        }

        // 2. 设置 JAVA_HOME 为空
        bool created;
        if (setSysEnv(L"JAVA_HOME", L"", created)) {
            std::wcout << L"  [成功] 已" << (created ? L"创建" : L"修改") << L" 系统环境变量 JAVA_HOME 为空\n";
        } else {
            std::cerr << "  [失败] 无法设置 JAVA_HOME（请尝试以管理员身份运行）\n";
        }

        // 3. 添加 %JAVA_HOME%\bin 到 PATH
        addToSysPath(L"%JAVA_HOME%\\bin");
        
        std::cout << "\n初始化完成。请重启终端或资源管理器以使系统环境变量生效。\n";

    } else if (cmd == "ls") {
        auto jdks = loadJdks(jsonFile);
        if (jdks.empty()) {
            std::cout << "当前 javapath.json 中未注册任何 JDK。\n";
        } else {
            std::cout << "已注册的 JDK 列表:\n";
            std::cout << "------------------------------------------------\n";
            std::cout << std::left << std::setw(15) << "JDK 名称" << "路径\n";
            std::cout << "------------------------------------------------\n";
            for (const auto& jdk : jdks) {
                std::cout << std::left << std::setw(15) << jdk.name << jdk.path << "\n";
            }
            std::cout << "------------------------------------------------\n";
        }

    } else if (cmd == "-j") {
        if (argc != 4) {
            std::cerr << "用法: jvm.exe -j <JDK名称> <JDK路径>\n";
            return 1;
        }
        std::string name = argv[2];
        std::string path = argv[3];

        // 简单路径验证
        if (!PathFileExistsA(path.c_str())) {
            std::cout << "  [提示] 路径 " << path << " 不存在，但仍会记录到配置中。\n";
        }

        auto jdks = loadJdks(jsonFile);
        bool updated = false;
        for (auto& jdk : jdks) {
            if (jdk.name == name) {
                jdk.path = path;
                updated = true;
                break;
            }
        }
        if (!updated) jdks.push_back({name, path});

        if (saveJdks(jsonFile, jdks)) {
            std::cout << "[成功] JDK '" << name << "' 已" << (updated ? "更新" : "添加") << "到 javapath.json\n";
        } else {
            std::cerr << "[失败] 无法保存 javapath.json\n";
            return 1;
        }

    } else if (cmd == "-s") {
        if (argc != 3) {
            std::cerr << "用法: jvm.exe -s <JDK名称>\n";
            return 1;
        }
        std::string targetName = argv[2];
        auto jdks = loadJdks(jsonFile);
        std::string foundPath = "";

        for (const auto& jdk : jdks) {
            if (jdk.name == targetName) {
                foundPath = jdk.path;
                break;
            }
        }

        if (foundPath.empty()) {
            std::cerr << "[错误] 未找到名为 '" << targetName << "' 的 JDK。请先使用 -j 注册。\n";
            return 1;
        }

        bool created;
        std::wstring wPath = s2w(foundPath);
        if (setSysEnv(L"JAVA_HOME", wPath, created)) {
            std::wcout << L"[成功] 已将系统 JAVA_HOME " << (created ? L"创建并设置" : L"修改为") << L": " << wPath << L"\n";
            std::cout << "提示：请重启终端以使 JAVA_HOME 变更生效。\n";
        } else {
            std::cerr << "[失败] 无法设置 JAVA_HOME（请尝试以管理员身份运行）\n";
            return 1;
        }

    } else {
        std::cerr << "未知命令: " << cmd << "\n";
        printHelp();
        return 1;
    }

    return 0;
}

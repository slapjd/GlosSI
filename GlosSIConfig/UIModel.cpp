/*
Copyright 2021-2022 Peter Repukat - FlatspotSoftware

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "UIModel.h"

#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>

#include <WinReg/WinReg.hpp>

#ifdef _WIN32
#include "UWPFetch.h"
#include <Windows.h>
#endif

#include "../version.hpp"

UIModel::UIModel() : QObject(nullptr)
{
    auto path = std::filesystem::temp_directory_path()
                    .parent_path()
                    .parent_path()
                    .parent_path();

    path /= "Roaming";
    path /= "GlosSI";
    if (!std::filesystem::exists(path))
        std::filesystem::create_directories(path);

    config_path_ = path;
    config_dir_name_ = QString::fromStdWString((path /= "Targets").wstring());

    if (!std::filesystem::exists(path))
        std::filesystem::create_directories(path);

    parseShortcutVDF();
    readTargetConfigs();
}

void UIModel::readTargetConfigs()
{
    QDir dir(config_dir_name_);
    auto entries = dir.entryList(QDir::Files, QDir::SortFlag::Name);
    entries.removeIf([](const auto& entry) {
        return !entry.endsWith(".json");
    });

    std::for_each(entries.begin(), entries.end(), [this](const auto& name) {
        auto path = config_path_;
        path /= config_dir_name_.toStdWString();
        path /= name.toStdWString();
        QFile file(path);
        if (!file.open(QIODevice::Text | QIODevice::ReadOnly)) {
            // meh
            return;
        }
        const auto data = file.readAll();
        file.close();
        const auto jsondoc = QJsonDocument::fromJson(data);
        auto filejson = jsondoc.object();

        filejson["name"] = filejson.contains("name")
                               ? filejson["name"].toString()
            : QString(name).replace(QRegularExpression("\\.json"), "");

        targets_.append(filejson.toVariantMap());
    });

    emit targetListChanged();
}

QVariantList UIModel::getTargetList() const
{
    return targets_;
}

void UIModel::addTarget(QVariant shortcut)
{
    const auto map = shortcut.toMap();
    const auto json = QJsonObject::fromVariantMap(map);
    writeTarget(json, map["name"].toString());
    targets_.append(QJsonDocument(json).toVariant());
    emit targetListChanged();
}

void UIModel::updateTarget(int index, QVariant shortcut)
{
    const auto map = shortcut.toMap();
    const auto json = QJsonObject::fromVariantMap(map);

    auto oldName = targets_[index].toMap()["name"].toString().replace(QRegularExpression("[\\\\/:*?\"<>|]"), "") + ".json";
    auto path = config_path_;
    path /= config_dir_name_.toStdString();
    path /= (oldName).toStdString();
    std::filesystem::remove(path);

    writeTarget(json, map["name"].toString());

    targets_.replace(index, QJsonDocument(json).toVariant());
    emit targetListChanged();
}

void UIModel::deleteTarget(int index)
{
    auto oldName = targets_[index].toMap()["name"].toString().replace(QRegularExpression("[\\\\/:*?\"<>|]"), "") + ".json";
    auto path = config_path_;
    path /= config_dir_name_.toStdString();
    path /= (oldName).toStdString();
    std::filesystem::remove(path);
    targets_.remove(index);
    emit targetListChanged();
}

bool UIModel::isInSteam(QVariant shortcut)
{
    const auto map = shortcut.toMap();
    for (auto& steam_shortcut : shortcuts_vdf_) {
        if (map["name"].toString() == QString::fromStdString(steam_shortcut.appname)) {
            if (QString::fromStdString(steam_shortcut.exe).toLower().contains("glossitarget.exe")) {
                return true;
            }
        }
    }

    return false;
}

bool UIModel::addToSteam(QVariant shortcut, const QString& shortcutspath, bool from_cmd)
{
    QDir appDir = QGuiApplication::applicationDirPath();
    const auto map = shortcut.toMap();
    const auto name = map["name"].toString();
    const auto maybeLaunchPath = map["launchPath"].toString();
    const auto launch = map["launch"].toBool();

    VDFParser::Shortcut vdfshortcut;
    vdfshortcut.appname = name.toStdString();
    vdfshortcut.exe = ("\"" + appDir.absolutePath() + "/GlosSITarget.exe" + "\"").toStdString();
    vdfshortcut.StartDir = (launch && !maybeLaunchPath.isEmpty()
                                      ? (std::string("\"") + std::filesystem::path(maybeLaunchPath.toStdString()).parent_path().string() + "\"")
                                      : ("\"" + appDir.absolutePath() + "\"").toStdString());
    // ShortcutPath; default
    vdfshortcut.LaunchOptions = (QString(name).replace(QRegularExpression("[\\\\/:*?\"<>|]"), "") + ".json").toStdString();
    // IsHidden; default
    // AllowDesktopConfig; default
    // AllowOverlay; default
    // openvr; default
    // Devkit; default
    // DevkitGameID; default
    // DevkitOverrideAppID; default
    // LastPlayTime; default
    auto maybeIcon = map["icon"].toString();
    if (maybeIcon.isEmpty()) {
        if (launch && !maybeLaunchPath.isEmpty())
            vdfshortcut.icon =
                "\"" + (is_windows_ ? QString(maybeLaunchPath).replace(QRegularExpression("\\/"), "\\").toStdString() : maybeLaunchPath.toStdString()) + "\"";
    }
    else {
        vdfshortcut.icon =
            "\"" + (is_windows_ ? QString(maybeIcon).replace(QRegularExpression("\\/"), "\\").toStdString() : maybeIcon.toStdString()) + "\"";
    }
    // Add installed locally and GlosSI tag
    vdfshortcut.tags.push_back("Installed locally");
    vdfshortcut.tags.push_back("GlosSI");

    shortcuts_vdf_.push_back(vdfshortcut);

    return writeShortcutsVDF(L"add", name.toStdWString(), shortcutspath.toStdWString(), from_cmd);
}
bool UIModel::addToSteam(const QString& name, const QString& shortcutspath, bool from_cmd)
{
    qDebug() << "trying to add " << name << " to steam";
    const auto target = std::find_if(targets_.begin(), targets_.end(), [&name](const auto& target) {
        const auto map = target.toMap();
        const auto target_name = map["name"].toString().replace(QRegularExpression("[\\\\/:*?\"<>|]"), "");
        return name == target_name;
    });
    if (target != targets_.end()) {
        return addToSteam(*target, shortcutspath, from_cmd);
    }
    qDebug() << name << " not found!";
    return false;
}
bool UIModel::removeFromSteam(const QString& name, const QString& shortcutspath, bool from_cmd)
{
    qDebug() << "trying to remove " << name << " from steam";
    shortcuts_vdf_.erase(std::ranges::remove_if(shortcuts_vdf_, [&name](const auto& shortcut) {
                    return shortcut.appname == name.toStdString();
                }).begin(),
                         shortcuts_vdf_.end());
    return writeShortcutsVDF(L"remove", name.toStdWString(), shortcutspath.toStdWString(), from_cmd);
}

QVariantMap UIModel::manualProps(QVariant shortcut)
{
    QDir appDir = QGuiApplication::applicationDirPath();
    const auto map = shortcut.toMap();
    const auto name = map["name"].toString().replace(QRegularExpression("[\\\\/:*?\"<>|]"), "");
    const auto maybeLaunchPath = map["launchPath"].toString();
    const auto launch = map["launch"].toBool();

    QVariantMap res;
    res.insert("name", name);
    res.insert("config", name + ".json");
    res.insert("launch", ("\"" + appDir.absolutePath() + "/GlosSITarget.exe" + "\""));
    res.insert("launchDir", (
                                launch && !maybeLaunchPath.isEmpty()
                                    ? (QString("\"") + QString::fromStdString(std::filesystem::path(maybeLaunchPath.toStdString()).parent_path().string()) + "\"")
                                    : ("\"" + appDir.absolutePath() + "\"")));
    return res;
}

void UIModel::enableSteamInputXboxSupport()
{
    if (foundSteam()) {
        const std::filesystem::path config_path = std::wstring(getSteamPath()) + user_data_path_.toStdWString() + getSteamUserId() + user_config_file_.toStdWString();
        if (!std::filesystem::exists(config_path)) {
            qDebug() << "localconfig.vdf does not exist.";
        }
        QFile file(config_path);
        if (file.open(QIODevice::Text | QIODevice::ReadOnly)) {
            QTextStream in(&file);
            QStringList lines;
            QString line = in.readLine();
            // simple approach is enough...
            while (!in.atEnd()) {
                if (line.contains("SteamController_XBoxSupport")) {
                    if (line.contains("1")) {
                        qDebug() << "\"SteamController_XBoxSupport\" is already enabled! aborting write...";
                        file.close();
                        return;
                    }
                    qDebug() << "found \"SteamController_XBoxSupport\" line, replacing value...";
                    line.replace("0", "1");
                }
                lines.push_back(line);
                line = in.readLine();
            }
            file.close();
            QFile updatedFile(config_path);
            if (updatedFile.open(QFile::WriteOnly | QFile::Truncate | QFile::Text)) {
                qDebug() << "writing localconfig.vdf...";
                QTextStream out(&updatedFile);
                for (const auto& l : lines) {
                    out << l << "\n";
                }
            }
            updatedFile.close();
        }
    }
}

#ifdef _WIN32
QVariantList UIModel::uwpApps()
{
    return UWPFetch::UWPAppList();
}
#endif

bool UIModel::writeShortcutsVDF(const std::wstring& mode, const std::wstring& name, const std::wstring& shortcutspath, bool is_admin_try) const
{
#ifdef _WIN32
    const std::filesystem::path config_path = is_admin_try
                                                  ? shortcutspath
                                                  : std::wstring(getSteamPath()) + user_data_path_.toStdWString() + getSteamUserId() + shortcutsfile_.toStdWString();

    qDebug() << "Steam config Path: " << config_path;
    qDebug() << "Trying to write config as admin: " << is_admin_try;

    bool write_res;
    try {
        write_res = VDFParser::Parser::writeShortcuts(config_path, shortcuts_vdf_, qDebug());
    }
    catch (const std::exception& e) {
        qDebug() << "Couldn't backup shortcuts file: " << e.what();
    }

    if (!write_res && !is_admin_try) {
        wchar_t szPath[MAX_PATH];
        if (GetModuleFileName(NULL, szPath, ARRAYSIZE(szPath))) {
            // Launch itself as admin
            SHELLEXECUTEINFO sei = {sizeof(sei)};
            sei.lpVerb = L"runas";
            qDebug() << QString("exepath: %1").arg(szPath);
            sei.lpFile = szPath;
            const std::wstring paramstr = mode + L" " + name + L" \"" + config_path.wstring() + L"\"";
            sei.lpParameters = paramstr.c_str();
            sei.hwnd = NULL;
            sei.nShow = SW_NORMAL;
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            if (!ShellExecuteEx(&sei)) {
                DWORD dwError = GetLastError();
                if (dwError == ERROR_CANCELLED) {
                    qDebug() << "User cancelled UAC Prompt";
                    return false;
                }
            }
            else {
                qDebug() << QString("HProc: %1").arg((int)sei.hProcess);

                if (sei.hProcess && WAIT_OBJECT_0 == WaitForSingleObject(sei.hProcess, INFINITE)) {
                    DWORD exitcode = 1;
                    GetExitCodeProcess(sei.hProcess, &exitcode);
                    qDebug() << QString("Exitcode: %1").arg((int)exitcode);
                    if (exitcode == 0) {
                        return true;
                    }
                }
                return false;
            }
        }
    }
    return write_res;
#else
    return VDFParser::Parser::writeShortcuts(config_path, shortcuts_vdf_);
#endif
}

bool UIModel::getIsWindows() const
{
    return is_windows_;
}

bool UIModel::hasAcrylicEffect() const
{
    return has_acrylic_affect_;
}

void UIModel::setAcrylicEffect(bool has_acrylic_affect)
{
    has_acrylic_affect_ = has_acrylic_affect;
    emit acrylicChanged();
}

void UIModel::writeTarget(const QJsonObject& json, const QString& name) const
{
    auto path = config_path_;
    path /= config_dir_name_.toStdWString();
    path /= (QString(name).replace(QRegularExpression("[\\\\/:*?\"<>|]"), "") + ".json").toStdWString();
    QFile file(path);
    if (!file.open(QIODevice::Text | QIODevice::ReadWrite)) {
        qDebug() << "Couldn't open file for writing: " << path;
        return;
    }

    file.write(
        QString(QJsonDocument(json).toJson(QJsonDocument::Indented))
        .toStdString()
        .data()
    );
    file.close();
}

QString UIModel::getVersionString() const
{
    return QString(version::VERSION_STR);
}

std::filesystem::path UIModel::getSteamPath() const
{
    try {
#ifdef _WIN32
        // TODO: check if keys/value exist
        // steam should always be open and have written reg values...
        winreg::RegKey key{HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam"};
        if (!key.IsValid()) {
            return "";
        }
        const auto res = key.GetStringValue(L"SteamPath");
        return res;
    }
    catch (...) {
        return "";
    }
#else
    return L""; // TODO LINUX
#endif
}

std::wstring UIModel::getSteamUserId() const
{
#ifdef _WIN32
    try {
        // TODO: check if keys/value exist
        // steam should always be open and have written reg values...
        winreg::RegKey key{HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam\\ActiveProcess"};
        if (!key.IsValid()) {
            return L"0";
        }
        const auto res = std::to_wstring(key.GetDwordValue(L"ActiveUser"));
        if (res == L"0") {
            qDebug() << "Steam not open?";
        }
        return res;
    } catch(...) {
        return L"0";
    }
#else
    return L""; // TODO LINUX
#endif
}

bool UIModel::foundSteam() const
{
    if (getSteamPath() == "" || getSteamUserId() == L"0") {
        return false;
    }
    const std::filesystem::path user_config_dir = std::wstring(getSteamPath()) + user_data_path_.toStdWString() + getSteamUserId();
    if (!std::filesystem::exists(user_config_dir)) {
        return false;
    }
    return true;
}

void UIModel::parseShortcutVDF()
{
    const std::filesystem::path config_path = std::wstring(getSteamPath()) + user_data_path_.toStdWString() + getSteamUserId() + shortcutsfile_.toStdWString();
    if (!std::filesystem::exists(config_path)) {
        qDebug() << "Shortcuts file does not exist.";
        return;
    }

    try {
        shortcuts_vdf_ = VDFParser::Parser::parseShortcuts(config_path, qDebug());
    }
    catch (const std::exception& e) {
        qDebug() << "Error parsing VDF: " << e.what();
    }
}

bool UIModel::isSteamInputXboxSupportEnabled() const
{
    // return true as default to not bug the user in error cases.
    if (foundSteam()) {
        const std::filesystem::path config_path = std::wstring(getSteamPath()) + user_data_path_.toStdWString() + getSteamUserId() + user_config_file_.toStdWString();
        if (!std::filesystem::exists(config_path)) {
            qDebug() << "localconfig.vdf does not exist.";
            return true;
        }
        QFile file(config_path);
        if (file.open(QIODevice::Text | QIODevice::ReadOnly)) {
            QTextStream in(&file);
            QString line = in.readLine();
            // simple, regex approach should be enough...
            while (!in.atEnd()) {
                if (line.contains("SteamController_XBoxSupport")) {
                    file.close();
                    if (line.contains("1")) {
                        qDebug() << "\"SteamController_XBoxSupport\" is enabled!";
                        return true;
                    }
                    qDebug() << "\"SteamController_XBoxSupport\" is disabled!";
                    return false;
                }
                line = in.readLine();
            }
            qDebug() << "couldn't find \"SteamController_XBoxSupport\" in localconfig.vdf";
            file.close();
        }
        else {
            qDebug() << "could not open localconfig.vdf";
        }
    }
    return true;
}

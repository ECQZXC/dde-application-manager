// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktopentry.h"
#include "global.h"
#include <QFileInfo>
#include <QDir>
#include <algorithm>
#include <QRegularExpression>
#include <QDirIterator>
#include <QStringView>
#include <QVariant>
#include <iostream>

auto DesktopEntry::parserGroupHeader(const QString &str) noexcept
{
    auto groupHeader = str.sliced(1, str.size() - 2);
    auto it = m_entryMap.find(groupHeader);
    if (it == m_entryMap.cend()) {
        return m_entryMap.insert(groupHeader, {});
    }
    return it;
}

DesktopErrorCode DesktopEntry::parseEntry(const QString &str, decltype(m_entryMap)::iterator &currentGroup) noexcept
{
    if (str.startsWith("#")) {
        return DesktopErrorCode::NoError;
    }

    auto splitCharIndex = str.indexOf(']');
    if (splitCharIndex != -1) {
        for (; splitCharIndex < str.size(); ++splitCharIndex) {
            if (str.at(splitCharIndex) == '=') {
                break;
            }
        }
    } else {
        splitCharIndex = str.indexOf('=');
    }
    auto keyStr = str.first(splitCharIndex).trimmed();
    auto valueStr = str.sliced(splitCharIndex + 1).trimmed();
    QString key;
    QString valueKey{defaultKeyStr};

    constexpr auto MainKey = R"re((?<MainKey>[0-9a-zA-Z-]+))re";  // main key. eg.(Name, X-CUSTOM-KEY).
    constexpr auto Language = R"re((?:[a-z]+))re";                // language of locale postfix. eg.(en, zh)
    constexpr auto Country = R"re((?:_[A-Z]+))re";                // country of locale postfix. eg.(US, CN)
    constexpr auto Encoding = R"re((?:\.[0-9A-Z-]+))re";          // encoding of locale postfix. eg.(UFT-8)
    constexpr auto Modifier = R"re((?:@[a-z=;]+))re";             // modifier of locale postfix. eg(euro;collation=traditional)
    const static auto validKey =
        QString("^%1(?:\\[(?<LOCALE>%2%3?%4?%5?)\\])?$").arg(MainKey).arg(Language).arg(Country).arg(Encoding).arg(Modifier);
    // example: https://regex101.com/r/hylOay/1
    QRegularExpression re{validKey};
    re.optimize();
    auto matcher = re.match(keyStr);
    if (!matcher.hasMatch()) {
        qWarning() << "invalid key: " << keyStr;
        return DesktopErrorCode::EntryKeyInvalid;
    }

    key = matcher.captured("MainKey");

    if (auto locale = matcher.captured("LOCALE"); !locale.isEmpty()) {
        valueKey = locale;
    }

    auto cur = currentGroup->find(key);
    if (cur == currentGroup->end()) {
        currentGroup->insert(keyStr, {{valueKey, valueStr}});
        return DesktopErrorCode::NoError;
    }

    auto value = cur->find(valueKey);
    if (value == cur->end()) {
        cur->insert(valueKey, valueStr);
        return DesktopErrorCode::NoError;
    }

    qWarning() << "duplicated postfix and this line will be aborted, maybe format is invalid.\n"
               << "exist: " << value.key() << "[" << value.value() << "]"
               << "current: " << str;

    return DesktopErrorCode::NoError;
}

std::optional<DesktopFile> DesktopFile::searchDesktopFileByPath(const QString &desktopFile, DesktopErrorCode &err) noexcept
{
    constexpr decltype(auto) desktopPostfix = ".desktop";

    if (!desktopFile.endsWith(desktopPostfix)) {
        qWarning() << "file isn't a desktop file:" << desktopFile;
        err = DesktopErrorCode::MismatchedFile;
        return std::nullopt;
    }

    QFileInfo fileinfo{desktopFile};
    if (!fileinfo.isAbsolute() or !fileinfo.exists()) {
        qWarning() << "desktop file not found.";
        err = DesktopErrorCode::NotFound;
        return std::nullopt;
    }

    QString path{desktopFile};
    QString id;

    const auto &XDGDataDirs = getXDGDataDirs();
    auto idGen = std::any_of(XDGDataDirs.cbegin(), XDGDataDirs.cend(), [&desktopFile](const QString &suffixPath) {
        return desktopFile.startsWith(suffixPath);
    });

    if (idGen) {
        auto tmp = path.chopped(sizeof(desktopPostfix) - 1);
        auto components = tmp.split(QDir::separator()).toList();
        auto it = std::find(components.cbegin(), components.cend(), "applications");
        QString FileId;
        ++it;
        while (it != components.cend()) {
            FileId += (*(it++) + "-");
        }
        id = FileId.chopped(1);  // remove extra "-""
    }

    struct stat buf;
    if (auto ret = stat(path.toLatin1().data(), &buf); ret == -1) {
        err = DesktopErrorCode::OpenFailed;
        qWarning() << "get file" << path << "state failed:" << std::strerror(errno);
        return std::nullopt;
    }

    err = DesktopErrorCode::NoError;
    constexpr std::size_t nanoToSec = 1e9;

    return DesktopFile{std::move(path), std::move(id), buf.st_mtim.tv_sec * nanoToSec + buf.st_mtim.tv_nsec};
}

std::optional<DesktopFile> DesktopFile::searchDesktopFileById(const QString &appId, DesktopErrorCode &err) noexcept
{
    auto XDGDataDirs = getXDGDataDirs();

    for (const auto &dir : XDGDataDirs) {
        auto app = QFileInfo{dir + QDir::separator() + appId};
        while (!app.exists()) {
            auto filePath = app.absoluteFilePath();
            auto hyphenIndex = filePath.indexOf('-');
            if (hyphenIndex == -1) {
                break;
            }
            filePath.replace(hyphenIndex, 1, QDir::separator());
            app.setFile(filePath);
        }

        if (app.exists()) {
            return searchDesktopFileByPath(app.absoluteFilePath(), err);
        }
    }
    return std::nullopt;
}

bool DesktopFile::modified(std::size_t time) const noexcept
{
    return time != m_mtime;
}

DesktopErrorCode DesktopEntry::parse(const DesktopFile &appId) noexcept
{
    auto file = QFile(appId.filePath());
    if (!file.open(QFile::ExistingOnly | QFile::ReadOnly | QFile::Text)) {
        qWarning() << appId.filePath() << "can't open.";
        return DesktopErrorCode::OpenFailed;
    }

    QTextStream in{&file};
    return parse(in);
}

DesktopErrorCode DesktopEntry::parse(QTextStream &stream) noexcept
{
    if (stream.atEnd()) {
        return DesktopErrorCode::OpenFailed;
    }

    stream.setEncoding(QStringConverter::Utf8);
    decltype(m_entryMap)::iterator currentGroup;

    DesktopErrorCode err{DesktopErrorCode::NoError};
    while (!stream.atEnd()) {
        auto line = stream.readLine().trimmed();

        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith("[")) {
            if (!line.endsWith("]")) {
                return DesktopErrorCode::GroupHeaderInvalid;
            }
            currentGroup = parserGroupHeader(line);
            continue;
        }

        if (auto error = parseEntry(line, currentGroup); error != DesktopErrorCode::NoError) {
            err = error;
            qWarning() << "an error occurred,this line will be skipped:" << line;
        }
    }
    return err;
}

std::optional<QMap<QString, DesktopEntry::Value>> DesktopEntry::group(const QString &key) const noexcept
{
    if (auto group = m_entryMap.find(key); group != m_entryMap.cend()) {
        return *group;
    }
    return std::nullopt;
}

std::optional<DesktopEntry::Value> DesktopEntry::value(const QString &groupKey, const QString &valueKey) const noexcept
{
    const auto &destGroup = group(groupKey);
    if (!destGroup) {
        qWarning() << "group " << groupKey << " can't be found.";
        return std::nullopt;
    }

    auto it = destGroup->find(valueKey);
    if (it == destGroup->cend()) {
        qWarning() << "value " << valueKey << " can't be found.";
        return std::nullopt;
    }
    return *it;
}

QString DesktopEntry::Value::unescape(const QString &str) noexcept
{
    QString unescapedStr;
    for (qsizetype i = 0; i < str.size(); ++i) {
        auto c = str.at(i);
        if (c != '\\') {
            unescapedStr.append(c);
            continue;
        }

        switch (str.at(i + 1).toLatin1()) {
            default:
                unescapedStr.append(c);
                break;
            case 'n':
                unescapedStr.append('\n');
                ++i;
                break;
            case 't':
                unescapedStr.append('\t');
                ++i;
                break;
            case 'r':
                unescapedStr.append('\r');
                ++i;
                break;
            case '\\':
                unescapedStr.append('\\');
                ++i;
                break;
            case ';':
                unescapedStr.append(';');
                ++i;
                break;
            case 's':
                unescapedStr.append(' ');
                ++i;
                break;
        }
    }

    return unescapedStr;
}

QString DesktopEntry::Value::toString(bool &ok) const noexcept
{
    ok = false;
    auto str = this->find(defaultKeyStr);
    if (str == this->end()) {
        return {};
    }
    auto unescapedStr = unescape(*str);
    constexpr auto controlChars = "\\p{Cc}";
    constexpr auto asciiChars = "[^\x00-\x7f]";
    if (unescapedStr.contains(QRegularExpression{controlChars}) and unescapedStr.contains(QRegularExpression{asciiChars})) {
        return {};
    }

    ok = true;
    return unescapedStr;
}

QString DesktopEntry::Value::toLocaleString(const QLocale &locale, bool &ok) const noexcept
{
    ok = false;
    for (auto it = this->constKeyValueBegin(); it != this->constKeyValueEnd(); ++it) {
        auto [a, b] = *it;
        if (QLocale{a}.name() == locale.name()) {
            ok = true;
            return unescape(b);
        }
    }
    return toString(ok);
}

QString DesktopEntry::Value::toIconString(bool &ok) const noexcept
{
    return toString(ok);
}

bool DesktopEntry::Value::toBoolean(bool &ok) const noexcept
{
    ok = false;
    const auto &str = (*this)[defaultKeyStr];
    if (str == "true") {
        ok = true;
        return true;
    }
    if (str == "false") {
        ok = true;
        return false;
    }
    return false;
}

float DesktopEntry::Value::toNumeric(bool &ok) const noexcept
{
    const auto &str = (*this)[defaultKeyStr];
    QVariant v{str};
    return v.toFloat(&ok);
}

QDebug operator<<(QDebug debug, const DesktopEntry::Value &v)
{
    QDebugStateSaver saver{debug};
    debug << static_cast<const QMap<QString, QString> &>(v);
    return debug;
}

QDebug operator<<(QDebug debug, const DesktopErrorCode &v)
{
    QDebugStateSaver saver{debug};
    QString errMsg;
    switch (v) {
        case DesktopErrorCode::NoError: {
            errMsg = "no error.";
            break;
        }
        case DesktopErrorCode::NotFound: {
            errMsg = "file not found.";
            break;
        }
        case DesktopErrorCode::MismatchedFile: {
            errMsg = "file type is mismatched.";
            break;
        }
        case DesktopErrorCode::InvalidLocation: {
            errMsg = "file location is invalid, please check $XDG_DATA_DIRS.";
            break;
        }
        case DesktopErrorCode::OpenFailed: {
            errMsg = "couldn't open the file.";
            break;
        }
        case DesktopErrorCode::GroupHeaderInvalid: {
            errMsg = "groupHead syntax is invalid.";
            break;
        }
        case DesktopErrorCode::EntryKeyInvalid: {
            errMsg = "key syntax is invalid.";
            break;
        }
    }
    debug << errMsg;
    return debug;
}
//
//  Exporter.cpp
//  WechatExporter
//
//  Created by Matthew on 2020/9/30.
//  Copyright © 2020 Matthew. All rights reserved.
//

#include "Exporter.h"
#include <json/json.h>
#include "Downloader.h"
#include "WechatParser.h"

struct FriendDownloadHandler
{
    Downloader& downloadPool;
    std::string& userRoot;
    
    FriendDownloadHandler(Downloader& dlPool, std::string& usrRoot) : downloadPool(dlPool), userRoot(usrRoot)
    {
    }
    
    void operator()(const Friend& f)
    {
        std::string url = f.getPortrait();
        if (!url.empty())
        {
            downloadPool.addTask(url, combinePath(userRoot, f.getLocalPortrait()), 0);
        }
    }
};

Exporter::Exporter(const std::string& workDir, const std::string& backup, const std::string& output, Shell* shell, Logger* logger)
{
    m_running = false;
    m_iTunesDb = NULL;
    m_iTunesDbShare = NULL;
    m_workDir = workDir;
    m_backup = backup;
    m_output = output;
    m_shell = shell;
    m_logger = logger;
    m_notifier = NULL;
    m_cancelled = false;
    m_options = 0;
    m_extName = "html";
    m_templatesName = "templates";
}

Exporter::~Exporter()
{
    releaseITunes();
    m_shell = NULL;
    m_logger = NULL;
    m_notifier = NULL;
}

void Exporter::setNotifier(ExportNotifier *notifier)
{
    m_notifier = notifier;
}
bool Exporter::isRunning() const
{
    return m_running;
}

void Exporter::cancel()
{
    m_cancelled = true;
}

void Exporter::waitForComplition()
{
    if (!isRunning())
    {
        return;
    }

    m_thread.join();
}

void Exporter::setTextMode(bool textMode/* = true*/)
{
    if (textMode)
        m_options |=     SPO_TEXT_MODE;
    else
        m_options &= ~    SPO_TEXT_MODE;
}

void Exporter::setOrder(bool asc/* = true*/)
{
    if (asc)
        m_options &= ~SPO_DESC;
    else
        m_options |= SPO_DESC;
}

void Exporter::saveFilesInSessionFolder(bool flag/* = true*/)
{
    if (flag)
        m_options |= SPO_ICON_IN_SESSION;
    else
        m_options &= ~SPO_ICON_IN_SESSION;
}

void Exporter::setExtName(const std::string& extName)
{
    m_extName = extName;
}

void Exporter::setTemplatesName(const std::string& templatesName)
{
    m_templatesName = templatesName;
}

void Exporter::filterUsersAndSessions(const std::map<std::string, std::set<std::string>>& usersAndSessions)
{
    m_usersAndSessions = usersAndSessions;
}

bool Exporter::run()
{
    if (isRunning() || m_thread.joinable())
    {
        m_logger->write(getLocaleString("Previous task has not completed."));
        
        return false;
    }

    if (!m_shell->existsDirectory(m_output))
    {
        m_logger->write(formatString(getLocaleString("Can't access output directory: %s"), m_output.c_str()));
        return false;
    }
    
    m_running = true;

    std::thread th(&Exporter::runImpl, this);
    m_thread.swap(th);

    return true;
}

bool Exporter::loadUsersAndSessions(std::vector<std::pair<Friend, std::vector<Session>>>& usersAndSessions)
{
    loadStrings();
    
    if (!loadITunes(false))
    {
        m_logger->write(formatString(getLocaleString("Failed to parse the backup data of iTunes in the directory: %s"), m_backup.c_str()));
        notifyComplete();
        return false;
    }
    m_logger->debug("ITunes Database loaded.");
    
    WechatInfoParser wechatInfoParser(m_iTunesDb);
    if (wechatInfoParser.parse(m_wechatInfo))
    {
        m_logger->write(formatString(getLocaleString("iTunes Version: %s, iOS Version: %s, Wechat Version: %s"), m_iTunesDb->getVersion().c_str(), m_iTunesDb->getIOSVersion().c_str(), m_wechatInfo.getShortVersion().c_str()));
    }
    
    std::vector<Friend> users;
    LoginInfo2Parser loginInfo2Parser(m_iTunesDb);
    if (!loginInfo2Parser.parse(users))
    {
        return false;
    }

#ifndef NDEBUG
    m_logger->debug(loginInfo2Parser.getError());
#endif
    m_logger->debug("Wechat Users loaded.");
    usersAndSessions.reserve(users.size()); // Avoid re-allocation and causing the pointer changed
    for (std::vector<Friend>::const_iterator it = users.cbegin(); it != users.cend(); ++it)
    {
        std::vector<std::pair<Friend, std::vector<Session>>>::iterator it2 = usersAndSessions.emplace(usersAndSessions.cend(), std::pair<Friend, std::vector<Session>>(*it, std::vector<Session>()));
        Friends friends;
        loadUserFriendsAndSessions(it2->first, friends, it2->second, false);
    }

    return true;
}

bool Exporter::runImpl()
{
    time_t startTime;
    std::time(&startTime);
    notifyStart();
    
    loadStrings();
    loadTemplates();
    
    m_logger->write(formatString(getLocaleString("iTunes Backup: %s"), m_backup.c_str()));

    if (!loadITunes())
    {
        m_logger->write(formatString(getLocaleString("Failed to parse the backup data of iTunes in the directory: %s"), m_backup.c_str()));
        notifyComplete();
        return false;
    }
    m_logger->debug("ITunes Database loaded.");
    
    WechatInfoParser wechatInfoParser(m_iTunesDb);
    if (wechatInfoParser.parse(m_wechatInfo))
    {
        m_logger->write(formatString(getLocaleString("iTunes Version: %s, Wechat Version: %s"), m_iTunesDb->getVersion().c_str(), m_wechatInfo.getShortVersion().c_str()));
    }

    m_logger->write(getLocaleString("Finding Wechat accounts..."));

    std::vector<Friend> users;
    LoginInfo2Parser loginInfo2Parser(m_iTunesDb);
    if (!loginInfo2Parser.parse(users))
    {
        m_logger->write(getLocaleString("Failed to find Wechat account."));
        notifyComplete();
        return false;
    }

    m_logger->write(formatString(getLocaleString("%d Wechat account(s) found."), (int)(users.size())));

    std::string htmlBody;

    std::set<std::string> userFileNames;
    for (std::vector<Friend>::iterator it = users.begin(); it != users.end(); ++it)
    {
        if (m_cancelled)
        {
            break;
        }
        
        if (!m_usersAndSessions.empty())
        {
            if (m_usersAndSessions.find(it->getUsrName()) == m_usersAndSessions.cend())
            {
                continue;
            }
        }
        
        if (!buildFileNameForUser(*it, userFileNames))
        {
            m_logger->write(formatString(getLocaleString("Can't build directory name for user: %s. Skip it."), it->getUsrName().c_str()));
            continue;
        }

        std::string userOutputPath;
        exportUser(*it, userOutputPath);
        
        std::string userItem = getTemplate("listitem");
        userItem = replaceAll(userItem, "%%ITEMPICPATH%%", userOutputPath + "/Portrait/" + it->getLocalPortrait());
        if ((m_options & SPO_IGNORE_HTML_ENC) == 0)
        {
            userItem = replaceAll(userItem, "%%ITEMLINK%%", encodeUrl(it->getOutputFileName()) + "/index." + m_extName);
            userItem = replaceAll(userItem, "%%ITEMTEXT%%", safeHTML(it->getDisplayName()));
        }
        else
        {
            userItem = replaceAll(userItem, "%%ITEMLINK%%", it->getOutputFileName() + "/index." + m_extName);
            userItem = replaceAll(userItem, "%%ITEMTEXT%%", it->getDisplayName());
        }
        
        htmlBody += userItem;
    }
    
    std::string fileName = combinePath(m_output, "index." + m_extName);

    std::string html = getTemplate("listframe");
    html = replaceAll(html, "%%USERNAME%%", "");
    html = replaceAll(html, "%%TBODY%%", htmlBody);
    
    writeFile(fileName, html);
    
    time_t endTime = 0;
    std::time(&endTime);
    int seconds = static_cast<int>(difftime(endTime, startTime));
    std::ostringstream stream;
    
    int minutes = seconds / 60;
    int hours = minutes / 60;
    stream << std::setfill('0') << std::setw(2) << hours << ':'
        << std::setfill('0') << std::setw(2) << (minutes % 60) << ':'
        << std::setfill('0') << std::setw(2) << (seconds % 60);
    
    m_logger->write(formatString(getLocaleString((m_cancelled ? "Cancelled in %s." : "Completed in %s.")), stream.str().c_str()));
    
    notifyComplete(m_cancelled);
    
    return true;
}

bool Exporter::exportUser(Friend& user, std::string& userOutputPath)
{
    std::string uidMd5 = user.getHash();
    
    std::string userBase = combinePath("Documents", uidMd5);
    // Use display name first, it it can't be created, use uid hash
    userOutputPath = user.getOutputFileName();
    std::string outputBase = combinePath(m_output, userOutputPath);
    if (!m_shell->existsDirectory(outputBase))
    {
        if (!m_shell->makeDirectory(outputBase))
        {
            userOutputPath = user.getHash();
            outputBase = combinePath(m_output, userOutputPath);
            if (!m_shell->existsDirectory(outputBase))
            {
                if (!m_shell->makeDirectory(outputBase))
                {
                    return false;
                }
            }
        }
    }
    
    if ((m_options & SPO_IGNORE_AVATAR) == 0)
    {
        std::string portraitPath = combinePath(outputBase, "Portrait");
        m_shell->makeDirectory(portraitPath);
        std::string defaultPortrait = combinePath(portraitPath, "DefaultProfileHead@2x.png");
        m_shell->copyFile(combinePath(m_workDir, "res", "DefaultProfileHead@2x.png"), defaultPortrait, true);
    }
    if ((m_options & SPO_ICON_IN_SESSION) == 0 && (m_options & SPO_IGNORE_EMOJI) == 0)
    {
        std::string emojiPath = combinePath(outputBase, "Emoji");
        m_shell->makeDirectory(emojiPath);
    }
    
    m_logger->write(formatString(getLocaleString("Handling account: %s, Wechat Id: %s"), user.getDisplayName().c_str(), user.getUsrName().c_str()));
    
    m_logger->write(getLocaleString("Reading account info."));
    m_logger->write(getLocaleString("Reading chat info"));
    
    Friends friends;
    std::vector<Session> sessions;
    loadUserFriendsAndSessions(user, friends, sessions);
    
    m_logger->write(formatString(getLocaleString("%d chats found."), (int)(sessions.size())));
    
    Friend* myself = friends.getFriend(user.getHash());
    if (NULL == myself)
    {
        Friend& newUser = friends.addFriend(user.getHash());
        newUser = user;
        myself = &user;
    }
    
    // friends.handleFriend(FriendDownloadHandler(downloader, portraitPath));
    
    std::string userBody;
    
    std::map<std::string, std::set<std::string>>::const_iterator itUser = m_usersAndSessions.cend();
    if (!m_usersAndSessions.empty())
    {
        itUser = m_usersAndSessions.find(user.getUsrName());
    }
    
    std::function<std::string(const std::string&)> localeFunction = std::bind(&Exporter::getLocaleString, this, std::placeholders::_1);

    Downloader downloader(m_logger);
#ifndef NDEBUG
    m_logger->debug("UA: " + m_wechatInfo.buildUserAgent());
#endif
    downloader.setUserAgent(m_wechatInfo.buildUserAgent());
    if ((m_options & SPO_IGNORE_AVATAR) == 0)
    {
#ifndef NDEBUG
		m_logger->debug("Download avatar: *" + user.getPortrait() + "* => " + combinePath(outputBase, "Portrait", user.getLocalPortrait()));
#endif
        downloader.addTask(user.getPortrait(), combinePath(outputBase, "Portrait", user.getLocalPortrait()), 0);
    }
    
    SessionParser sessionParser(*myself, friends, *m_iTunesDb, *m_shell, m_options, downloader, localeFunction);
    std::set<std::string> sessionFileNames;
    for (std::vector<Session>::iterator it = sessions.begin(); it != sessions.end(); ++it)
    {
        if (m_cancelled)
        {
            break;
        }
        
        if (!m_usersAndSessions.empty())
        {
            if (itUser == m_usersAndSessions.cend() || itUser->second.find(it->getUsrName()) == itUser->second.cend())
            {
                continue;
            }
        }
        
        if (!buildFileNameForUser(*it, sessionFileNames))
        {
            m_logger->write(formatString(getLocaleString("Can't build directory name for chat: %s. Skip it."), it->getDisplayName().c_str()));
            continue;
        }
        
        std::string sessionDisplayName = it->getDisplayName();
#ifndef NDEBUG
        m_logger->write(formatString(getLocaleString("%d/%d: Handling the chat with %s"), (std::distance(sessions.begin(), it) + 1), sessions.size(), sessionDisplayName.c_str()) + " uid:" + it->getUsrName());
#else
        m_logger->write(formatString(getLocaleString("%d/%d: Handling the chat with %s"), (std::distance(sessions.begin(), it) + 1), sessions.size(), sessionDisplayName.c_str()));
#endif
        if (it->isSubscription())
        {
            m_logger->write(formatString(getLocaleString("Skip subscription: %s"), sessionDisplayName.c_str()));
            continue;
        }
        if ((m_options & SPO_IGNORE_AVATAR) == 0)
        {
            if (!(it->isPortraitEmpty()))
            {
                downloader.addTask(it->getPortrait(), combinePath(outputBase, "Portrait", it->getLocalPortrait()), 0);
            }
        }
        int count = exportSession(*myself, sessionParser, *it, userBase, outputBase);
        
        m_logger->write(formatString(getLocaleString("Succeeded handling %d messages."), count));
        
        if (count > 0)
        {
            std::string userItem = getTemplate("listitem");
            userItem = replaceAll(userItem, "%%ITEMPICPATH%%", "Portrait/" + it->getLocalPortrait());
            if ((m_options & SPO_IGNORE_HTML_ENC) == 0)
            {
                userItem = replaceAll(userItem, "%%ITEMLINK%%", encodeUrl(it->getOutputFileName()) + "." + m_extName);
                userItem = replaceAll(userItem, "%%ITEMTEXT%%", safeHTML(sessionDisplayName));
            }
            else
            {
                userItem = replaceAll(userItem, "%%ITEMLINK%%", it->getOutputFileName() + "." + m_extName);
                userItem = replaceAll(userItem, "%%ITEMTEXT%%", sessionDisplayName);
            }
            
            userBody += userItem;
        }
    }

    std::string html = getTemplate("listframe");
    html = replaceAll(html, "%%USERNAME%%", " - " + user.getDisplayName());
    html = replaceAll(html, "%%TBODY%%", userBody);
    
    std::string fileName = combinePath(outputBase, "index." + m_extName);
    writeFile(fileName, html);
    
    if (m_cancelled)
    {
        downloader.cancel();
    }
    else
    {
        int dlCount = downloader.getRunningCount();
        if (dlCount > 0)
        {
            m_logger->write(formatString(getLocaleString("Waiting for images(%d) downloading."), dlCount));
        }
    }
    downloader.finishAndWaitForExit();

    return true;
}

bool Exporter::loadUserFriendsAndSessions(const Friend& user, Friends& friends, std::vector<Session>& sessions, bool detailedInfo/* = true*/) const
{
    std::string uidMd5 = user.getHash();
    std::string userBase = combinePath("Documents", uidMd5);
    
    if (detailedInfo)
    {
        std::string wcdbPath = m_iTunesDb->findRealPath(combinePath(userBase, "DB", "WCDB_Contact.sqlite"));

        FriendsParser friendsParser(detailedInfo);
        friendsParser.parseWcdb(wcdbPath, friends);

        m_logger->debug("Wechat Friends(" + std::to_string(friends.friends.size()) + ") for: " + user.getDisplayName() + " loaded.");
    }

    SessionsParser sessionsParser(m_iTunesDb, m_iTunesDbShare, m_shell, m_wechatInfo.getCellDataVersion(), detailedInfo);
    
    sessionsParser.parse(user, sessions, friends);
 
    std::sort(sessions.begin(), sessions.end(), SessionLastMsgTimeCompare());
    
    m_logger->debug("Wechat Sessions for: " + user.getDisplayName() + " loaded.");
    return true;
}

int Exporter::exportSession(const Friend& user, SessionParser& sessionParser, const Session& session, const std::string& userBase, const std::string& outputBase)
{
    if (session.isDbFileEmpty())
    {
        return false;
    }
    
    std::string sessionBasePath = combinePath(outputBase, session.getOutputFileName() + "_files");
    if ((m_options & SPO_IGNORE_AVATAR) == 0)
    {
        std::string portraitPath = combinePath(sessionBasePath, "Portrait");
        m_shell->makeDirectory(portraitPath);
        std::string defaultPortrait = combinePath(portraitPath, "DefaultProfileHead@2x.png");
        m_shell->copyFile(combinePath(m_workDir, "res", "DefaultProfileHead@2x.png"), defaultPortrait, true);
    }
    if ((m_options & SPO_IGNORE_EMOJI) == 0)
    {
        m_shell->makeDirectory(combinePath(sessionBasePath, "Emoji"));
    }

    std::vector<std::string> messages;
        if (session.getRecordCount() > 0)
        {
            messages.reserve(session.getRecordCount());
        }
    std::function<bool(const std::vector<TemplateValues>&)> handler = std::bind(&Exporter::exportMessage, this, std::cref(session), std::placeholders::_1, std::ref(messages));
    
    int count = sessionParser.parse(userBase, outputBase, session, handler);
    if (count > 0 && !messages.empty())
    {
        const size_t pageSize = 1000;
        
        auto b = messages.cbegin();
        // No page for text mode
        auto e = (((m_options & SPO_TEXT_MODE) == SPO_TEXT_MODE) || messages.size() <= pageSize) ? messages.cend() : (b + pageSize);
        std::string moreMsgs = ((m_options & SPO_TEXT_MODE) == SPO_TEXT_MODE) ? "" : "[]"; // [] = empty json array
        if (e != messages.cend())
        {
            Json::Value jsonMsgs(Json::arrayValue);
            for (auto it = e; it != messages.cend(); ++it)
            {
                jsonMsgs.append(*it);
            }
            
            Json::FastWriter writer;
            moreMsgs = writer.write(jsonMsgs);
        }
        
        std::string fileName = combinePath(outputBase, session.getOutputFileName() + "." + m_extName);

        std::string html = getTemplate("frame");
        html = replaceAll(html, "%%DISPLAYNAME%%", session.getDisplayName());
        // html = replaceAll(html, "%%PAGES%%", std::to_string(pages));
        html = replaceAll(html, "%%BODY%%", join(b, e, ""));
        html = replaceAll(html, "%%JSONDATA%%", moreMsgs);
        
        writeFile(fileName, html);
        
        
    }
    
    return count;
}

bool Exporter::exportMessage(const Session& session, const std::vector<TemplateValues>& tvs, std::vector<std::string>& messages)
{
    std::string content;
    for (std::vector<TemplateValues>::const_iterator it = tvs.cbegin(); it != tvs.cend(); ++it)
    {
        content.append(buildContentFromTemplateValues(*it));
    }
    
    messages.push_back(content);
    return m_cancelled;
}

bool Exporter::buildFileNameForUser(Friend& user, std::set<std::string>& existingFileNames)
{
    std::string names[] = {user.getDisplayName(), user.getUsrName(), user.getHash()};
    
    bool succeeded = false;
    for (int idx = 0; idx < 3; ++idx)
    {
        std::string outputFileName = m_shell->removeInvalidCharsForFileName(names[idx]);
        if (isValidFileName(outputFileName))
        {
            if ( existingFileNames.find(outputFileName) != existingFileNames.cend())
            {
                int idx = 1;
                while (idx++)
                {
                    if (existingFileNames.find(outputFileName + "_" + std::to_string(idx)) == existingFileNames.cend())
                    {
                        outputFileName += "_" + std::to_string(idx);
                        break;
                    }
                }
            }
            user.setOutputFileName(outputFileName);
            existingFileNames.insert(outputFileName);
            succeeded = true;
            break;
        }
    }
    
    return succeeded;
}

bool Exporter::fillSession(Session& session, const Friends& friends) const
{
    if (session.isDisplayNameEmpty())
    {
        const Friend* f = friends.getFriend(session.getHash());
        if (NULL != f && !f->isDisplayNameEmpty())
        {
            session.setDisplayName(f->getDisplayName());
        }
    }

    return true;
}

void Exporter::releaseITunes()
{
    if (NULL != m_iTunesDb)
    {
        delete m_iTunesDb;
        m_iTunesDb = NULL;
    }
    if (NULL != m_iTunesDbShare)
    {
        delete m_iTunesDbShare;
        m_iTunesDbShare = NULL;
    }
}

bool Exporter::loadITunes(bool detailedInfo/* = true*/)
{
    releaseITunes();
    
    m_iTunesDb = new ITunesDb(m_backup, "Manifest.db");
    if (!detailedInfo)
    {
        std::function<bool(const char*, int)> fn = std::bind(&Exporter::filterITunesFile, this, std::placeholders::_1, std::placeholders::_2);
        m_iTunesDb->setLoadingFilter(fn);
    }
    if (!m_iTunesDb->load("AppDomain-com.tencent.xin", !detailedInfo))
    {
        return false;
    }
    m_iTunesDbShare = new ITunesDb(m_backup, "Manifest.db");
    
    if (!m_iTunesDbShare->load("AppDomainGroup-group.com.tencent.xin"))
    {
        // Optional
        // return false;
    }
    
    return true;
}

bool Exporter::loadTemplates()
{
    const char* names[] = {"frame", "msg", "video", "notice", "system", "audio", "image", "card", "emoji", "plainshare", "share", "thumb", "listframe", "listitem"};
    for (int idx = 0; idx < sizeof(names) / sizeof(const char*); idx++)
    {
        std::string name = names[idx];
        std::string path = combinePath(m_workDir, "res", m_templatesName, name + ".html");
        m_templates[name] = readFile(path);
    }
    return true;
}

bool Exporter::loadStrings()
{
    m_localeStrings.clear();

    std::string path = combinePath(m_workDir, "res", "locale.txt");

    Json::Reader reader;
    Json::Value value;
    if (reader.parse(readFile(path), value))
    {
        int sz = value.size();
        for (int idx = 0; idx < sz; ++idx)
        {
            std::string k = value[idx]["key"].asString();
            std::string v = value[idx]["value"].asString();
            if (m_localeStrings.find(k) != m_localeStrings.cend())
            {
                // return false;
            }
            m_localeStrings[k] = v;
        }
    }

    return true;
}

std::string Exporter::getTemplate(const std::string& key) const
{
    std::map<std::string, std::string>::const_iterator it = m_templates.find(key);
    return (it == m_templates.cend()) ? "" : it->second;
}

std::string Exporter::getLocaleString(const std::string& key) const
{
    // std::string value = key;
    std::map<std::string, std::string>::const_iterator it = m_localeStrings.find(key);
    return it == m_localeStrings.cend() ? key : it->second;
}

std::string Exporter::buildContentFromTemplateValues(const TemplateValues& values) const
{
    std::string content = getTemplate(values.getName());
    for (TemplateValues::const_iterator it = values.cbegin(); it != values.cend(); ++it)
    {
        if (startsWith(it->first, "%"))
        {
            content = replaceAll(content, it->first, it->second);
        }
    }
    
    std::string::size_type pos = 0;
    while ((pos = content.find("%%", pos)) != std::string::npos)
    {
        std::string::size_type posEnd = content.find("%%", pos + 2);
        if (posEnd == std::string::npos)
        {
            break;
        }
        
        content.erase(pos, posEnd + 2 - pos);
    }
    
    return content;
}


void Exporter::notifyStart()
{
    if (m_notifier)
    {
        m_notifier->onStart();
    }
}

void Exporter::notifyComplete(bool cancelled/* = false*/)
{
    if (m_notifier)
    {
        m_notifier->onComplete(cancelled);
    }
}

void Exporter::notifyProgress(uint32_t numberOfMessages, uint32_t numberOfTotalMessages)
{
    if (m_notifier)
    {
        m_notifier->onProgress(numberOfMessages, numberOfTotalMessages);
    }
}

bool Exporter::filterITunesFile(const char *file, int flags) const
{
    const char *str = std::strchr(file, '/');
    if (str != NULL)
    {
        str = std::strchr(str + 1, '/');
        if (str != NULL)
        {
            if (std::strncmp(str, "/Audio/", 7) == 0 ||
                std::strncmp(str, "/Img/", 5) == 0 ||
                std::strncmp(str, "/OpenData/", 10) == 0 ||
                std::strncmp(str, "/Video/", 7) == 0)
            {
                return false;
            }
        }
    }
    
    return true;
}

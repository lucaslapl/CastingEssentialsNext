#include "PlayerAliases.h"
#include "PluginBase/Interfaces.h"
#include "PluginBase/Player.h"
#include "PluginBase/TFDefinitions.h"
#include <cdll_int.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <steam/steam_api.h>
#include <toolframework/ienginetool.h>
#include <vprof.h>

#include <Windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

MODULE_REGISTER(PlayerAliases);

PlayerAliases::PlayerAliases()
    : ce_playeraliases_enabled(
          "ce_playeraliases_enabled", "0", FCVAR_NONE, "Enables player aliases.",
          [](IConVar* var, const char*, float) { GetModule()->ToggleEnabled(static_cast<ConVar*>(var)); }),

      ce_playeraliases_format_mode("ce_playeraliases_format_mode", "0", FCVAR_NONE,
                                   "0 = apply format to all players, 1 = apply format to aliased players only"),
      ce_playeraliases_format_blu("ce_playeraliases_format_blu", "%alias%", FCVAR_NONE, "Name format for BLU players."),
      ce_playeraliases_format_red("ce_playeraliases_format_red", "%alias%", FCVAR_NONE, "Name format for RED players."),
      ce_playeraliases_format_swap(
          "ce_playeraliases_format_swap", []() { GetModule()->SwapTeamFormats(); },
          "Swaps the values of ce_playeraliases_format_red and ce_playeraliases_format_blu."),

      ce_playeraliases_list(
          "ce_playeraliases_list", []() { GetModule()->PrintPlayerAliases(); },
          "Prints all player aliases to console."),
      ce_playeraliases_add(
          "ce_playeraliases_add", [](const CCommand& args) { GetModule()->AddPlayerAlias(args); },
          "Adds a new player alias."),
      ce_playeraliases_remove(
          "ce_playeraliases_remove", [](const CCommand& args) { GetModule()->RemovePlayerAlias(args); },
          "Removes an existing player alias."),

      ce_playeraliases_etf2l(
          "ce_playeraliases_etf2l", "0", FCVAR_NONE,
          "If 1, automatically fetches ETF2L names and uses them as aliases.",
          [](IConVar* var, const char*, float) { GetModule()->ToggleETF2L(static_cast<ConVar*>(var)); }),
      ce_playeraliases_etf2l_refresh(
          "ce_playeraliases_etf2l_refresh", []() { GetModule()->ETF2LRefresh(); },
          "Re-fetches ETF2L names for all connected players."),
      ce_playeraliases_etf2l_reset(
          "ce_playeraliases_etf2l_reset", []() { GetModule()->ETF2LReset(); },
          "Clears the on-disk ETF2L name cache."),
      m_GetPlayerInfoHook(
          std::bind(&PlayerAliases::GetPlayerInfoOverride, this, std::placeholders::_1, std::placeholders::_2))
{
    LoadETF2LCache();
}

PlayerAliases::~PlayerAliases()
{
    {
        std::lock_guard<std::mutex> lock(m_ETF2LQueueMutex);
        m_ETF2LShutdown = true;
    }
    m_ETF2LQueueCV.notify_all();
    if (m_ETF2LThread.joinable())
        m_ETF2LThread.join();
}

bool PlayerAliases::CheckDependencies()
{
    bool ready = true;

    if (!Interfaces::GetEngineClient())
    {
        PluginWarning("Required interface IVEngineClient for module %s not available!\n", GetModuleName());
        ready = false;
    }

    if (!Interfaces::AreSteamLibrariesAvailable())
    {
        PluginWarning("Required Steam libraries for module %s not available!\n", GetModuleName());
        ready = false;
    }

    if (!Player::CheckDependencies())
    {
        PluginWarning("Required player helper class for module %s not available!\n", GetModuleName());
        ready = false;
    }

    if (!GetHooks()->GetHook<HookFunc::IVEngineClient_GetPlayerInfo>())
    {
        PluginWarning("Required hook IVEngineClient::GetPlayerInfo for module %s not available!\n", GetModuleName());
        ready = false;
    }

    return ready;
}

bool PlayerAliases::GetPlayerInfoOverride(int ent_num, player_info_s* pinfo)
{
    VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_CE);
    if (ent_num < 1 || ent_num > Interfaces::GetEngineTool()->GetMaxClients())
        return false;

    Player* player = Player::GetPlayer(ent_num, __FUNCSIG__);
    if (!player)
        return false;

    static EUniverse universe = k_EUniverseInvalid;
    if (universe == k_EUniverseInvalid)
    {
        if (Interfaces::GetSteamAPIContext()->SteamUtils())
            universe = Interfaces::GetSteamAPIContext()->SteamUtils()->GetConnectedUniverse();
        else
        {
            PluginWarning("Steam libraries not available - assuming public universe for user Steam IDs!\n");
            universe = k_EUniversePublic;
        }
    }

    bool result = m_GetPlayerInfoHook.GetOriginal()(ent_num, pinfo);

    CSteamID playerSteamID(pinfo->friendsID, 1, universe, k_EAccountTypeIndividual);

    if (ce_playeraliases_etf2l.GetBool() && playerSteamID.IsValid())
    {
        DrainETF2LResults();
        QueueETF2LFetch(playerSteamID.ConvertToUint64());
    }

    const char* alias = GetAlias(playerSteamID);

    if (auto mode = ce_playeraliases_format_mode.GetInt(); mode == 0 || (mode == 1 && alias))
    {
        if (!alias)
            alias = pinfo->name;

        std::string gameName;
        switch (player->GetTeam())
        {
            case TFTeam::Red:
                gameName = ce_playeraliases_format_red.GetString();
                break;

            case TFTeam::Blue:
                gameName = ce_playeraliases_format_blu.GetString();
                break;

            default:
                gameName = "%alias%";
                break;
        }

        FindAndReplaceInString(gameName, "%alias%", alias);
        V_strcpy_safe(pinfo->name, gameName.c_str());

        GetHooks()->SetState<HookFunc::IVEngineClient_GetPlayerInfo>(Hooking::HookAction::SUPERCEDE);
        return result;
    }

    return true;
}

const char* PlayerAliases::GetAlias(const CSteamID& player) const
{
    if (!player.IsValid())
        return nullptr;

    auto found = m_CustomAliases.find(player);
    if (found != m_CustomAliases.end())
        return found->second.c_str();

    return nullptr;
}

void PlayerAliases::SwapTeamFormats()
{
    std::string red(ce_playeraliases_format_red.GetString());
    ce_playeraliases_format_red.SetValue(ce_playeraliases_format_blu.GetString());
    ce_playeraliases_format_blu.SetValue(red.c_str());
}

void PlayerAliases::PrintPlayerAliases()
{
    Msg("%i player aliases:\n", m_CustomAliases.size());

    for (auto alias : m_CustomAliases)
        Msg("    %s = %s\n", RenderSteamID(alias.first).c_str(), alias.second.c_str());
}

void PlayerAliases::AddPlayerAlias(const CCommand& brokenCommand)
{
    CCommand command;
    if (!ReparseForSteamIDs(brokenCommand, command))
        return;

    if (command.ArgC() != 3)
    {
        Warning("Usage: %s <steam id> <name>\n", ce_playeraliases_add.GetName());
        return;
    }

    const CSteamID id(command.Arg(1));
    if (!id.IsValid())
    {
        Warning("Failed to parse steamid \"%s\"!\n", command.Arg(1));
        return;
    }

    // Check for existing
    {
        const auto& found = m_CustomAliases.find(id);
        if (found != m_CustomAliases.end())
            m_CustomAliases.erase(found);
    }

    {
        const std::string& name = command.Arg(2);
        m_CustomAliases.insert(std::make_pair(id, name));
    }

    return;
}

void PlayerAliases::RemovePlayerAlias(const CCommand& brokenCommand)
{
    {
        CCommand command;
        if (!ReparseForSteamIDs(brokenCommand, command))
            return;

        if (command.ArgC() != 2)
            goto Usage;

        const CSteamID id(command.Arg(1));
        if (!id.IsValid())
        {
            Warning("Failed to parse steamid \"%s\"!\n", command.Arg(1));
            goto Usage;
        }

        // Find and remove existing
        {
            const auto& found = m_CustomAliases.find(id);
            if (found != m_CustomAliases.end())
                m_CustomAliases.erase(found);
            else
            {
                Warning("Unable to find existing alias for steamid \"%s\"!\n", command.Arg(1));
                return;
            }
        }

        return;
    }

Usage:
    Warning("Usage: %s <steam id>\n", ce_playeraliases_remove.GetName());
}

void PlayerAliases::FindAndReplaceInString(std::string& str, const std::string_view& find,
                                           const std::string_view& replace)
{
    if (find.empty())
        return;

    size_t start_pos = 0;

    while ((start_pos = str.find(find, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, find.length(), replace);
        start_pos += replace.length();
    }
}

void PlayerAliases::ToggleEnabled(const ConVar* var) { m_GetPlayerInfoHook.SetEnabled(var->GetBool()); }
static constexpr int kETF2LMaxFetchAttempts = 3;

void PlayerAliases::QueueETF2LFetch(uint64 steamID64)
{
    const auto state = m_ETF2LFetchStates.find(steamID64);
    if (state != m_ETF2LFetchStates.end())
    {
        // Only failed fetches are re-queued, up to a limited number of attempts.
        if (state->second != FetchState::Failed)
            return;
        if (m_ETF2LFetchAttempts[steamID64] >= kETF2LMaxFetchAttempts)
            return;
    }

    {
        std::lock_guard<std::mutex> lock(m_ETF2LCacheMutex);
        auto cached = m_ETF2LCache.find(steamID64);
        if (cached != m_ETF2LCache.end())
        {
            m_CustomAliases[CSteamID(steamID64)] = cached->second;
            m_ETF2LFetchStates[steamID64] = FetchState::Fetched;
            return;
        }
    }

    m_ETF2LFetchStates[steamID64] = FetchState::Pending;

    {
        std::lock_guard<std::mutex> lock(m_ETF2LQueueMutex);
        m_ETF2LQueue.push_back(steamID64);
    }
    m_ETF2LQueueCV.notify_one();

    if (!m_ETF2LThreadRunning.exchange(true))
        m_ETF2LThread = std::thread(&PlayerAliases::ETF2LWorker, this);
}

void PlayerAliases::ETF2LWorker()
{
    while (true)
    {
        uint64 steamID64;
        {
            std::unique_lock<std::mutex> lock(m_ETF2LQueueMutex);
            m_ETF2LQueueCV.wait(lock, [this]() { return m_ETF2LShutdown || !m_ETF2LQueue.empty(); });
            if (m_ETF2LShutdown && m_ETF2LQueue.empty())
                break;
            steamID64 = m_ETF2LQueue.front();
            m_ETF2LQueue.pop_front();
        }

        std::string name;
        if (!FetchETF2LName(steamID64, name))
        {
            std::lock_guard<std::mutex> lock(m_ETF2LResultMutex);
            m_ETF2LFailedFetches.push_back(steamID64);
            continue;
        }

        std::lock_guard<std::mutex> lock(m_ETF2LResultMutex);
        m_ETF2LResults[CSteamID(steamID64)] = name;
    }
}

void PlayerAliases::DrainETF2LResults()
{
    std::map<CSteamID, std::string> results;
    std::deque<uint64> failures;
    {
        std::lock_guard<std::mutex> lock(m_ETF2LResultMutex);
        results.swap(m_ETF2LResults);
        failures.swap(m_ETF2LFailedFetches);
    }
    if (results.empty() && failures.empty())
        return;

    bool dirty = false;
    for (const auto& result : results)
    {
        const uint64 steamID64 = result.first.ConvertToUint64();
        m_CustomAliases[result.first] = result.second;
        m_ETF2LFetchStates[steamID64] = FetchState::Fetched;
        {
            std::lock_guard<std::mutex> lock(m_ETF2LCacheMutex);
            m_ETF2LCache[steamID64] = result.second;
        }
        dirty = true;
    }
    for (uint64 steamID64 : failures)
    {
        m_ETF2LFetchAttempts[steamID64]++;
        m_ETF2LFetchStates[steamID64] = FetchState::Failed;
    }
    if (dirty)
        SaveETF2LCache();
}

bool PlayerAliases::FetchETF2LName(uint64 steamID64, std::string& name)
{
    const wchar_t kHostname[] = L"api.etf2l.org";
    const wchar_t kUserAgent[] = L"CastingEssentialsNext (github.com/drunderscore/CastingEssentialsNext)";

    HINTERNET session = WinHttpOpen(
        kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;

    // Bound each request so a hanging server can't block shutdown in ~PlayerAliases().
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, kHostname, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        return false;
    }

    const std::string path = std::string("/player/") + std::to_string(steamID64) + ".json";
    std::wstring widePath(path.begin(), path.end());

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", widePath.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    const BOOL sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL received = FALSE;
    std::string response;
    if (sent && WinHttpReceiveResponse(request, nullptr))
    {
        DWORD bytesRead = 0;
        char buffer[4096];
        do
        {
            bytesRead = 0;
            if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytesRead) || bytesRead == 0)
                break;
            response.append(buffer, bytesRead);
        } while (bytesRead > 0);
        received = !response.empty();
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!received)
        return false;

    return ExtractETF2LPlayerName(response, name);
}

// Returns the position just past the closing quote of the JSON string starting at pos,
// or npos if the string is unterminated.
static size_t SkipJSONString(const std::string& json, size_t pos)
{
    pos++;
    while (pos < json.length())
    {
        if (json[pos] == '\\')
            pos += 2;
        else if (json[pos] == '"')
            return pos + 1;
        else
            pos++;
    }
    return std::string::npos;
}

// Returns the position just past the JSON value (string, object, array or literal)
// starting at pos, or npos if the JSON is malformed.
static size_t SkipJSONValue(const std::string& json, size_t pos)
{
    if (pos >= json.length())
        return std::string::npos;

    if (json[pos] == '"')
        return SkipJSONString(json, pos);

    int depth = 0;
    while (pos < json.length())
    {
        const char c = json[pos];
        if (c == '"')
        {
            pos = SkipJSONString(json, pos);
            if (pos == std::string::npos)
                return std::string::npos;
            if (depth == 0)
                return pos;
            continue;
        }

        if (c == '{' || c == '[')
            depth++;
        else if (c == '}' || c == ']')
        {
            if (depth == 0)
                return pos;
            if (--depth == 0)
                return pos + 1;
        }
        else if (depth == 0 && c == ',')
            return pos;
        pos++;
    }
    return std::string::npos;
}

bool PlayerAliases::ExtractETF2LPlayerName(const std::string& json, std::string& out)
{
    // The ETF2L API serializes the members of the "player" object in an unstable order,
    // and the nested "teams" data has "name" keys of its own, so the player name has to
    // be looked up as a direct member of "player" instead of grabbing the first "name"
    // match anywhere in the response.
    size_t pos = json.find("\"player\"");
    if (pos == std::string::npos)
        return false;

    pos = json.find('{', pos);
    if (pos == std::string::npos)
        return false;

    pos++;
    while (pos < json.length())
    {
        // Skip the whitespace and separators between members.
        while (pos < json.length() &&
               (json[pos] == ' ' || json[pos] == ',' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
            pos++;

        if (pos >= json.length() || json[pos] == '}')
            return false;

        if (json[pos] != '"')
            return false;

        const size_t keyStart = pos + 1;
        pos = SkipJSONString(json, pos);
        if (pos == std::string::npos)
            return false;
        const std::string key = json.substr(keyStart, pos - keyStart - 1);

        while (pos < json.length() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
            pos++;
        if (pos >= json.length() || json[pos] != ':')
            return false;
        pos++;

        while (pos < json.length() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
            pos++;
        if (pos >= json.length())
            return false;

        if (key == "name")
        {
            if (json[pos] != '"')
                return false;

            const size_t start = pos + 1;
            pos = SkipJSONString(json, pos);
            if (pos == std::string::npos)
                return false;
            const size_t end = pos - 1;

            out.clear();
            for (size_t i = start; i < end; i++)
            {
                if (json[i] == '\\' && i + 1 < end)
                {
                    i++;
                    switch (json[i])
                    {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back('\t'); break;
                        case 'r': out.push_back('\r'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        default:  out.push_back(json[i]); break;
                    }
                }
                else
                    out.push_back(json[i]);
            }
            return !out.empty();
        }

        pos = SkipJSONValue(json, pos);
        if (pos == std::string::npos)
            return false;
    }

    return false;
}

std::string PlayerAliases::GetETF2LCachePath() const
{
    const char* gameDir = "tf";
    if (Interfaces::GetEngineClient())
        gameDir = Interfaces::GetEngineClient()->GetGameDirectory();

    return std::string(gameDir) + "/cfg/ce_playeraliases_etf2l_cache.cfg";
}

void PlayerAliases::LoadETF2LCache()
{
    const std::string path = GetETF2LCachePath();
    std::ifstream file(path);
    if (!file.is_open())
        return;

    std::lock_guard<std::mutex> lock(m_ETF2LCacheMutex);
    std::string line;
    while (std::getline(file, line))
    {
        const size_t space = line.find(' ');
        if (space == std::string::npos)
            continue;
        const uint64 steamID64 = std::strtoull(line.substr(0, space).c_str(), nullptr, 10);
        if (!steamID64)
            continue;
        m_ETF2LCache[steamID64] = line.substr(space + 1);
    }
}

void PlayerAliases::SaveETF2LCache()
{
    std::lock_guard<std::mutex> lock(m_ETF2LCacheMutex);
    std::ofstream file(GetETF2LCachePath(), std::ios::trunc);
    if (!file.is_open())
        return;
    for (const auto& entry : m_ETF2LCache)
        file << entry.first << ' ' << entry.second << '\n';
}

void PlayerAliases::ToggleETF2L(const ConVar* var)
{
    if (var->GetBool())
        LoadETF2LCache();
}

void PlayerAliases::ETF2LRefresh()
{
    m_ETF2LFetchStates.clear();
    m_ETF2LFetchAttempts.clear();
    {
        std::lock_guard<std::mutex> lock(m_ETF2LCacheMutex);
        m_ETF2LCache.clear();
    }
    Msg("ETF2L fetch states cleared. Reconnect or wait for the next GetPlayerInfo calls to re-fetch.\n");
}

void PlayerAliases::ETF2LReset()
{
    {
        std::lock_guard<std::mutex> lock(m_ETF2LCacheMutex);
        m_ETF2LCache.clear();
    }
    m_ETF2LFetchStates.clear();
    m_ETF2LFetchAttempts.clear();
    const std::string path = GetETF2LCachePath();
    std::remove(path.c_str());
    Msg("ETF2L cache cleared.\n");
}

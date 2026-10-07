#pragma once
#include "PluginBase/Hook.h"
#include "PluginBase/Modules.h"

#include <atomic>
#include <convar.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

struct player_info_s;

class PlayerAliases final : public Module<PlayerAliases>
{
public:
    PlayerAliases();
    virtual ~PlayerAliases();

    static bool CheckDependencies();
    static constexpr __forceinline const char* GetModuleName() { return "Player Aliases"; }

private:
    bool GetPlayerInfoOverride(int ent_num, player_info_s* pInfo);

    std::map<CSteamID, std::string> m_CustomAliases;
    Hook<HookFunc::IVEngineClient_GetPlayerInfo> m_GetPlayerInfoHook;

    static void FindAndReplaceInString(std::string& str, const std::string_view& find, const std::string_view& replace);

    const char* GetAlias(const CSteamID& player) const;

    // ETF2L integration
    enum class FetchState
    {
        Pending,
        Fetched,
        Failed,
    };
    std::map<CSteamID, FetchState> m_ETF2LFetchStates;
    std::map<uint64, int> m_ETF2LFetchAttempts;
    void QueueETF2LFetch(uint64 steamID64);
    void ETF2LWorker();
    static bool FetchETF2LName(uint64 steamID64, std::string& name);
    static bool ExtractJSONString(const std::string& json, const std::string& key, std::string& out);
    std::string GetETF2LCachePath() const;
    void LoadETF2LCache();
    void SaveETF2LCache();
    void ToggleETF2L(const ConVar* var);
    void DrainETF2LResults();
    void ETF2LRefresh();
    void ETF2LReset();

    std::thread m_ETF2LThread;
    std::mutex m_ETF2LQueueMutex;
    std::condition_variable m_ETF2LQueueCV;
    std::deque<uint64> m_ETF2LQueue;
    std::mutex m_ETF2LResultMutex;
    std::map<CSteamID, std::string> m_ETF2LResults;
    std::deque<uint64> m_ETF2LFailedFetches;
    std::mutex m_ETF2LCacheMutex;
    std::map<uint64, std::string> m_ETF2LCache;
    std::atomic_bool m_ETF2LThreadRunning{ false };
    std::atomic_bool m_ETF2LShutdown{ false };

    ConVar ce_playeraliases_enabled;
    ConVar ce_playeraliases_format_mode;
    ConVar ce_playeraliases_format_blu;
    ConVar ce_playeraliases_format_red;
    ConVar ce_playeraliases_etf2l;

    ConCommand ce_playeraliases_format_swap;
    void SwapTeamFormats();

    ConCommand ce_playeraliases_list;
    void PrintPlayerAliases();

    ConCommand ce_playeraliases_add;
    void AddPlayerAlias(const CCommand& command);
    ConCommand ce_playeraliases_remove;
    void RemovePlayerAlias(const CCommand& command);

    ConCommand ce_playeraliases_etf2l_refresh;
    ConCommand ce_playeraliases_etf2l_reset;

    void ToggleEnabled(const ConVar* var);
};
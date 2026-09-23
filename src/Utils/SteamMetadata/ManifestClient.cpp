#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    static bool ParsePlainUint(std::string_view body, uint64_t* out) {
        uint64_t code = 0;
        auto [_, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        if (ec != std::errc{}) return false;
        *out = code;
        return true;
    }

    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) {
        size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;
        size_t q1 = body.find('"', key + 9);
        if (q1 == std::string_view::npos) return false;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string_view::npos) return false;
        return ParsePlainUint(body.substr(q1 + 1, q2 - q1 - 1), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Adding a new provider: add one row to kProviders below.
    // host / port / tls / path are all derived from the URL template
    // by Make() at compile time.
    //
    // Order matters only as the tie-break for a cold start: the first
    // entry is the initial active provider, and failures walk forward
    // from there.

    struct Provider {
        std::string_view name;          // matches [manifest] url = "..."
        const char*      urlTemplate;   // full literal with one %llu — for log & path
        Parser           parse;
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse) {
        return {name, url, parse};
    }

    static constexpr Provider kProviders[] = {
        Make("opensteamtool", "https://manifest.opensteamtool.com/%llu",       ParsePlainUint),
        Make("wudrm",         "http://gmrc.wudrm.com/manifest/%llu",           ParsePlainUint),
        Make("steamrun",      "https://manifest.steam.run/api/manifest/%llu",  ParseSteamRunJson),
    };

    static constexpr size_t kProviderCount = std::size(kProviders);

    // Index of the last provider that answered, so the healthy one is tried
    // first on the next fetch.  Mutated under g_mutex.
    static size_t g_active = 0;                         // opensteamtool

    // Per-provider deadline; while it is in the future the provider is
    // skipped rather than paying its timeout again.
    static std::chrono::steady_clock::time_point g_deadUntil[kProviderCount];

    static std::mutex g_mutex;

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < kProviderCount; ++i)
            if (kProviders[i].name == name) {
                g_active = i;
                // Explicit choice (startup or config reload) overrides the
                // cooldowns learned at runtime, so it takes effect at once.
                for (auto& dead : g_deadUntil) dead = {};
                return true;
            }
        return false;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return kProviders[g_active].name.data();
    }

    // ── request ───────────────────────────────────────────────────

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
    }

    // ── fetch ─────────────────────────────────────────────────────

    // Tries the active provider first, then fails over through the rest of
    // the table.  A provider that just failed goes into cooldown so a dead
    // upstream costs one timeout instead of one per depot; when every
    // provider is cooling down this returns false without any network I/O.
    static bool FetchWithFailover(uint64_t gid, uint64_t* outCode) {
        const Config::ManifestTimeouts timeouts = Config::GetManifestTimeouts();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kFetchBudgetMs);

        size_t tried = 0;
        for (size_t k = 0; k < kProviderCount; ++k) {
            const size_t i = (g_active + k) % kProviderCount;
            const Provider& p = kProviders[i];

            if (g_deadUntil[i] > std::chrono::steady_clock::now()) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (cooldown)", p.name);
                continue;
            }

            // Never let one attempt outlive the shared budget — the hook
            // stops waiting at kFetchBudgetMs, so a slower answer is useless
            // to Steam and would only delay the next depot's fetch.
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                LOG_MANIFEST_WARN("Manifest gid={}: {}ms budget exhausted after {} attempt(s)",
                                  gid, kFetchBudgetMs, tried);
                break;
            }
            const auto cap = [left](uint32_t ms) {
                return static_cast<uint32_t>(std::min<int64_t>(ms, left));
            };

            char urlLog[256];
            std::snprintf(urlLog, sizeof(urlLog), p.urlTemplate, gid);

            auto r = OSTPlatform::Http::Execute(
                L"GET",
                urlLog,
                nullptr,
                0,
                nullptr,
                cap(timeouts.resolve),
                cap(timeouts.connect),
                cap(timeouts.send),
                cap(timeouts.recv));

            LOG_MANIFEST_INFO("Manifest {} status={} gid={}", p.name, r.status, gid);
            ++tried;

            if (r.ok && r.status == 200 && p.parse(r.body, outCode)) {
                if (i != g_active) {
                    LOG_MANIFEST_WARN("Manifest provider switched {} -> {}",
                                      kProviders[g_active].name, p.name);
                    g_active = i;
                }
                return true;
            }

            g_deadUntil[i] = std::chrono::steady_clock::now()
                           + std::chrono::milliseconds(kProviderCooldownMs);
        }

        if (tried == 0)
            LOG_MANIFEST_WARN("Manifest gid={}: every provider is in cooldown", gid);
        return false;
    }

    // ── public ────────────────────────────────────────────────────

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return FetchWithFailover(manifestGid, outRequestCode);
    }
}

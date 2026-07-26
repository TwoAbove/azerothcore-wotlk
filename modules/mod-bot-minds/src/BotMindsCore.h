/*
 * mod-bot-minds — percept tap and op mailbox (say / state / command).
 *
 * THREAD MODEL
 *  - Script hooks build percept JSON and enqueue it without doing file I/O.
 *  - The background worker flushes percepts, tails ops from EOF at attach,
 *    parses them, and enqueues valid ops for the owning map thread.
 *  - PlayerScript::OnPlayerUpdate executes queued ops on the map thread.
 */

#ifndef MOD_BOT_MINDS_CORE_H
#define MOD_BOT_MINDS_CORE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace BotMinds
{

struct Op
{
    std::string kind;     // say | state | command
    std::string channel;  // say
    std::string to;       // say (whisper target)
    std::string text;     // say, command
    std::string id;       // state/command: echoed in the result percept
};

class Core
{
public:
    static Core* instance();

    // WorldScript hooks (world thread)
    void LoadConfig(bool reload);
    void Shutdown();

    bool Enabled() const { return _enabled.load(std::memory_order_relaxed); }
    bool IsWatched(std::string const& name);

    // Thread-safe: enqueue one percept line for a watched bot.
    void Push(std::string const& bot, std::string&& json);
    // Map thread: pop the next pending op for this bot.
    bool PopOp(std::string const& bot, Op& out);

    // Percept line helpers (pure string operations, safe on any thread).
    static std::string Esc(std::string const& s);
    static uint64_t NowMs();
    // Opens a percept object: {"t":<ms>,"bot":"<name>","kind":"<kind>"  — caller appends fields and '}'.
    static std::string Base(std::string const& bot, char const* kind);

    uint32_t PulseMinutes() const { return _pulseMinutes.load(std::memory_order_relaxed); }

private:
    Core() = default;
    void WorkerLoop();
    void FlushPercepts();
    void TailOps();
    static bool ParseOpLine(std::string const& line, Op& op, std::string& error);

    struct PendingPercept
    {
        std::string bot;
        std::string line;
        uint8_t failures = 0;
    };

    std::atomic<bool> _enabled{false};
    std::atomic<bool> _running{false};
    std::thread _worker;
    std::mutex _workerMx;
    std::condition_variable _workerCv;
    std::atomic<uint32_t> _pulseMinutes{5};
    std::atomic<uint32_t> _opsPollMs{500};
    std::string _perceptDir;

    std::mutex _mx; // guards config state, _watch, _outQ, _opQ, and _opsResetBots
    std::unordered_set<std::string> _watch;
    std::deque<PendingPercept> _outQ;
    std::unordered_map<std::string, std::deque<Op>> _opQ;
    std::unordered_set<std::string> _opsResetBots;

    // Worker-thread-only state.
    std::unordered_map<std::string, uint64_t> _opsOffsets;
    std::unordered_map<std::string, std::string> _opsPartial;
    std::unordered_set<std::string> _opsDroppingLine;
};

} // namespace BotMinds

#define sBotMinds BotMinds::Core::instance()

#endif
